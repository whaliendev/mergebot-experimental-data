#define LOG_TAG "InputDispatcher"
#define ATRACE_TAG ATRACE_TAG_INPUT
#define LOG_NDEBUG 1
#include <android-base/chrono_utils.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android/os/IInputConstants.h>
#include <binder/Binder.h>
#include <ftl/enum.h>
#if defined(__ANDROID__)
#include <gui/SurfaceComposerClient.h>
#endif
#include <input/InputDevice.h>
#include <input/PrintTools.h>
#include <powermanager/PowerManager.h>
#include <unistd.h>
#include <utils/Trace.h>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstddef>
#include <ctime>
#include <queue>
#include <sstream>
#include "Connection.h"
#include "DebugConfig.h"
#include "InputDispatcher.h"
#define INDENT "  "
#define INDENT2 "    "
#define INDENT3 "      "
#define INDENT4 "        "
using namespace android::ftl::flag_operators;
using android::base::HwTimeoutMultiplier;
using android::base::Result;
using android::base::StringPrintf;
using android::gui::DisplayInfo;
using android::gui::FocusRequest;
using android::gui::TouchOcclusionMode;
using android::gui::WindowInfo;
using android::gui::WindowInfoHandle;
using android::os::InputEventInjectionResult;
using android::os::InputEventInjectionSync;
namespace android::inputdispatcher {
namespace {
class scoped_unlock {
public:
    explicit scoped_unlock(std::mutex& mutex) : mMutex(mutex) { mMutex.unlock(); }
    ~scoped_unlock() { mMutex.lock(); }
private:
    std::mutex& mMutex;
};
const std::chrono::duration DEFAULT_INPUT_DISPATCHING_TIMEOUT = std::chrono::milliseconds(
        android::os::IInputConstants::UNMULTIPLIED_DEFAULT_DISPATCHING_TIMEOUT_MILLIS *
        HwTimeoutMultiplier());
constexpr nsecs_t APP_SWITCH_TIMEOUT = 500 * 1000000LL;
const std::chrono::duration STALE_EVENT_TIMEOUT = std::chrono::seconds(10) * HwTimeoutMultiplier();
constexpr nsecs_t SLOW_EVENT_PROCESSING_WARNING_TIMEOUT = 2000 * 1000000LL;
constexpr std::chrono::milliseconds SLOW_INTERCEPTION_THRESHOLD = 50ms;
constexpr std::chrono::nanoseconds KEY_WAITING_FOR_EVENTS_TIMEOUT = 500ms;
constexpr size_t RECENT_QUEUE_MAX_SIZE = 10;
constexpr int LOGTAG_INPUT_INTERACTION = 62000;
constexpr int LOGTAG_INPUT_FOCUS = 62001;
constexpr int LOGTAG_INPUT_CANCEL = 62003;
const ui::Transform kIdentityTransform;
inline nsecs_t now() {
    return systemTime(SYSTEM_TIME_MONOTONIC);
}
inline const char* toString(bool value) {
    return value ? "true" : "false";
}
inline const std::string toString(const sp<IBinder>& binder) {
    if (binder == nullptr) {
        return "<null>";
    }
    return StringPrintf("%p", binder.get());
}
inline int32_t getMotionEventActionPointerIndex(int32_t action) {
    return (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
}
bool isValidKeyAction(int32_t action) {
    switch (action) {
        case AKEY_EVENT_ACTION_DOWN:
        case AKEY_EVENT_ACTION_UP:
            return true;
        default:
            return false;
    }
}
bool validateKeyEvent(int32_t action) {
    if (!isValidKeyAction(action)) {
        ALOGE("Key event has invalid action code 0x%x", action);
        return false;
    }
    return true;
}
bool isValidMotionAction(int32_t action, int32_t actionButton, int32_t pointerCount) {
    switch (MotionEvent::getActionMasked(action)) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_UP:
            return pointerCount == 1;
        case AMOTION_EVENT_ACTION_MOVE:
        case AMOTION_EVENT_ACTION_HOVER_ENTER:
        case AMOTION_EVENT_ACTION_HOVER_MOVE:
        case AMOTION_EVENT_ACTION_HOVER_EXIT:
            return pointerCount >= 1;
        case AMOTION_EVENT_ACTION_CANCEL:
        case AMOTION_EVENT_ACTION_OUTSIDE:
        case AMOTION_EVENT_ACTION_SCROLL:
            return true;
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_UP: {
            const int32_t index = MotionEvent::getActionIndex(action);
            return index >= 0 && index < pointerCount && pointerCount > 1;
        }
        case AMOTION_EVENT_ACTION_BUTTON_PRESS:
        case AMOTION_EVENT_ACTION_BUTTON_RELEASE:
            return actionButton != 0;
        default:
            return false;
    }
}
int64_t millis(std::chrono::nanoseconds t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
}
bool validateMotionEvent(int32_t action, int32_t actionButton, size_t pointerCount,
                         const PointerProperties* pointerProperties) {
    if (!isValidMotionAction(action, actionButton, pointerCount)) {
        ALOGE("Motion event has invalid action code 0x%x", action);
        return false;
    }
    if (pointerCount < 1 || pointerCount > MAX_POINTERS) {
        ALOGE("Motion event has invalid pointer count %zu; value must be between 1 and %zu.",
              pointerCount, MAX_POINTERS);
        return false;
    }
    std::bitset<MAX_POINTER_ID + 1> pointerIdBits;
    for (size_t i = 0; i < pointerCount; i++) {
        int32_t id = pointerProperties[i].id;
        if (id < 0 || id > MAX_POINTER_ID) {
            ALOGE("Motion event has invalid pointer id %d; value must be between 0 and %d", id,
                  MAX_POINTER_ID);
            return false;
        }
        if (pointerIdBits.test(id)) {
            ALOGE("Motion event has duplicate pointer id %d", id);
            return false;
        }
        pointerIdBits.set(id);
    }
    return true;
}
std::string dumpRegion(const Region& region) {
    if (region.isEmpty()) {
        return "<empty>";
    }
    std::string dump;
    bool first = true;
    Region::const_iterator cur = region.begin();
    Region::const_iterator const tail = region.end();
    while (cur != tail) {
        if (first) {
            first = false;
        } else {
            dump += "|";
        }
        dump += StringPrintf("[%d,%d][%d,%d]", cur->left, cur->top, cur->right, cur->bottom);
        cur++;
    }
    return dump;
}
std::string dumpQueue(const std::deque<DispatchEntry*>& queue, nsecs_t currentTime) {
    constexpr size_t maxEntries = 50;
    constexpr size_t skipBegin = maxEntries / 2;
    const size_t skipEnd = queue.size() - maxEntries / 2;
    std::string dump;
    for (size_t i = 0; i < queue.size(); i++) {
        const DispatchEntry& entry = *queue[i];
        if (i >= skipBegin && i < skipEnd) {
            dump += StringPrintf(INDENT4 "<skipped %zu entries>\n", skipEnd - skipBegin);
            i = skipEnd - 1;
            continue;
        }
        dump.append(INDENT4);
        dump += entry.eventEntry->getDescription();
        dump += StringPrintf(", seq=%" PRIu32 ", targetFlags=%s, resolvedAction=%d, age=%" PRId64
                             "ms",
                             entry.seq, entry.targetFlags.string().c_str(), entry.resolvedAction,
                             ns2ms(currentTime - entry.eventEntry->eventTime));
        if (entry.deliveryTime != 0) {
            dump += StringPrintf(", wait=%" PRId64 "ms", ns2ms(currentTime - entry.deliveryTime));
        }
        dump.append("\n");
    }
    return dump;
}
template <typename K, typename V>
V getValueByKey(const std::unordered_map<K, V>& map, K key) {
    auto it = map.find(key);
    return it != map.end() ? it->second : V{};
}
bool haveSameToken(const sp<WindowInfoHandle>& first, const sp<WindowInfoHandle>& second) {
    if (first == second) {
        return true;
    }
    if (first == nullptr || second == nullptr) {
        return false;
    }
    return first->getToken() == second->getToken();
}
bool haveSameApplicationToken(const WindowInfo* first, const WindowInfo* second) {
    if (first == nullptr || second == nullptr) {
        return false;
    }
    return first->applicationInfo.token != nullptr &&
            first->applicationInfo.token == second->applicationInfo.token;
}
template <typename T>
size_t firstMarkedBit(T set) {
    LOG_ALWAYS_FATAL_IF(set.none());
    size_t i = 0;
    while (!set.test(i)) {
        i++;
    }
    return i;
}
std::unique_ptr<DispatchEntry> createDispatchEntry(
        const InputTarget& inputTarget, std::shared_ptr<EventEntry> eventEntry,
        ftl::Flags<InputTarget::Flags> inputTargetFlags) {
    if (inputTarget.useDefaultPointerTransform()) {
        const ui::Transform& transform = inputTarget.getDefaultPointerTransform();
        return std::make_unique<DispatchEntry>(eventEntry, inputTargetFlags, transform,
                                               inputTarget.displayTransform,
                                               inputTarget.globalScaleFactor);
    }
    ALOG_ASSERT(eventEntry->type == EventEntry::Type::MOTION);
    const MotionEntry& motionEntry = static_cast<const MotionEntry&>(*eventEntry);
    std::vector<PointerCoords> pointerCoords;
    pointerCoords.resize(motionEntry.pointerCount);
    const ui::Transform& firstPointerTransform =
            inputTarget.pointerTransforms[firstMarkedBit(inputTarget.pointerIds)];
    ui::Transform inverseFirstTransform = firstPointerTransform.inverse();
    for (uint32_t pointerIndex = 0; pointerIndex < motionEntry.pointerCount; pointerIndex++) {
        const PointerProperties& pointerProperties = motionEntry.pointerProperties[pointerIndex];
        uint32_t pointerId = uint32_t(pointerProperties.id);
        const ui::Transform& currTransform = inputTarget.pointerTransforms[pointerId];
        pointerCoords[pointerIndex].copyFrom(motionEntry.pointerCoords[pointerIndex]);
        pointerCoords[pointerIndex].transform(currTransform);
        pointerCoords[pointerIndex].transform(inverseFirstTransform);
    }
    std::unique_ptr<MotionEntry> combinedMotionEntry =
            std::make_unique<MotionEntry>(motionEntry.id, motionEntry.eventTime,
                                          motionEntry.deviceId, motionEntry.source,
                                          motionEntry.displayId, motionEntry.policyFlags,
                                          motionEntry.action, motionEntry.actionButton,
                                          motionEntry.flags, motionEntry.metaState,
                                          motionEntry.buttonState, motionEntry.classification,
                                          motionEntry.edgeFlags, motionEntry.xPrecision,
                                          motionEntry.yPrecision, motionEntry.xCursorPosition,
                                          motionEntry.yCursorPosition, motionEntry.downTime,
                                          motionEntry.pointerCount, motionEntry.pointerProperties,
                                          pointerCoords.data());
    if (motionEntry.injectionState) {
        combinedMotionEntry->injectionState = motionEntry.injectionState;
        combinedMotionEntry->injectionState->refCount += 1;
    }
    std::unique_ptr<DispatchEntry> dispatchEntry =
            std::make_unique<DispatchEntry>(std::move(combinedMotionEntry), inputTargetFlags,
                                            firstPointerTransform, inputTarget.displayTransform,
                                            inputTarget.globalScaleFactor);
    return dispatchEntry;
}
status_t openInputChannelPair(const std::string& name, std::shared_ptr<InputChannel>& serverChannel,
                              std::unique_ptr<InputChannel>& clientChannel) {
    std::unique_ptr<InputChannel> uniqueServerChannel;
    status_t result = InputChannel::openInputChannelPair(name, uniqueServerChannel, clientChannel);
    serverChannel = std::move(uniqueServerChannel);
    return result;
}
template <typename T>
bool sharedPointersEqual(const std::shared_ptr<T>& lhs, const std::shared_ptr<T>& rhs) {
    if (lhs == nullptr && rhs == nullptr) {
        return true;
    }
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    return *lhs == *rhs;
}
KeyEvent createKeyEvent(const KeyEntry& entry) {
    KeyEvent event;
    event.initialize(entry.id, entry.deviceId, entry.source, entry.displayId, INVALID_HMAC,
                     entry.action, entry.flags, entry.keyCode, entry.scanCode, entry.metaState,
                     entry.repeatCount, entry.downTime, entry.eventTime);
    return event;
}
bool shouldReportMetricsForConnection(const Connection& connection) {
    if (connection.monitor) {
        return false;
    }
    if (!connection.responsive) {
        return false;
    }
    return true;
}
bool shouldReportFinishedEvent(const DispatchEntry& dispatchEntry, const Connection& connection) {
    const EventEntry& eventEntry = *dispatchEntry.eventEntry;
    const int32_t& inputEventId = eventEntry.id;
    if (inputEventId != dispatchEntry.resolvedEventId) {
        return false;
    }
    if (inputEventId == android::os::IInputConstants::INVALID_INPUT_EVENT_ID) {
        return false;
    }
    if (eventEntry.isSynthesized()) {
        return false;
    }
    const EventEntry::Type& inputEventEntryType = eventEntry.type;
    if (inputEventEntryType == EventEntry::Type::KEY) {
        const KeyEntry& keyEntry = static_cast<const KeyEntry&>(eventEntry);
        if (keyEntry.flags & AKEY_EVENT_FLAG_CANCELED) {
            return false;
        }
    } else if (inputEventEntryType == EventEntry::Type::MOTION) {
        const MotionEntry& motionEntry = static_cast<const MotionEntry&>(eventEntry);
        if (motionEntry.action == AMOTION_EVENT_ACTION_CANCEL ||
            motionEntry.action == AMOTION_EVENT_ACTION_HOVER_EXIT) {
            return false;
        }
    } else {
        return false;
    }
    if (!shouldReportMetricsForConnection(connection)) {
        return false;
    }
    return true;
}
bool isConnectionResponsive(const Connection& connection) {
    const nsecs_t currentTime = now();
    for (const DispatchEntry* entry : connection.waitQueue) {
        if (entry->timeoutTime < currentTime) {
            return false;
        }
    }
    return true;
}
bool isUserActivityEvent(const EventEntry& eventEntry) {
    switch (eventEntry.type) {
        case EventEntry::Type::FOCUS:
        case EventEntry::Type::POINTER_CAPTURE_CHANGED:
        case EventEntry::Type::DRAG:
        case EventEntry::Type::TOUCH_MODE_CHANGED:
        case EventEntry::Type::SENSOR:
        case EventEntry::Type::CONFIGURATION_CHANGED:
            return false;
        case EventEntry::Type::DEVICE_RESET:
        case EventEntry::Type::KEY:
        case EventEntry::Type::MOTION:
            return true;
    }
}
bool windowAcceptsTouchAt(const WindowInfo& windowInfo, int32_t displayId, float x, float y,
                          bool isStylus, const ui::Transform& displayTransform) {
    const auto inputConfig = windowInfo.inputConfig;
    if (windowInfo.displayId != displayId ||
        inputConfig.test(WindowInfo::InputConfig::NOT_VISIBLE)) {
        return false;
    }
    const bool windowCanInterceptTouch = isStylus && windowInfo.interceptsStylus();
    if (inputConfig.test(WindowInfo::InputConfig::NOT_TOUCHABLE) && !windowCanInterceptTouch) {
        return false;
    }
    const auto touchableRegion = displayTransform.transform(windowInfo.touchableRegion);
    const auto p = displayTransform.transform(x, y);
    if (!touchableRegion.contains(std::floor(p.x), std::floor(p.y))) {
        return false;
    }
    return true;
}
bool isPointerFromStylus(const MotionEntry& entry, int32_t pointerIndex) {
    return isFromSource(entry.source, AINPUT_SOURCE_STYLUS) &&
            isStylusToolType(entry.pointerProperties[pointerIndex].toolType);
}
bool canReceiveForegroundTouches(const WindowInfo& info) {
    return !info.inputConfig.test(gui::WindowInfo::InputConfig::NOT_TOUCHABLE) && !info.isSpy();
}
bool isWindowOwnedBy(const sp<WindowInfoHandle>& windowHandle, int32_t pid, int32_t uid) {
    if (windowHandle == nullptr) {
        return false;
    }
    const WindowInfo* windowInfo = windowHandle->getInfo();
    if (pid == windowInfo->ownerPid && uid == windowInfo->ownerUid) {
        return true;
    }
    return false;
}
std::optional<std::string> verifyTargetedInjection(const sp<WindowInfoHandle>& window,
                                                   const EventEntry& entry) {
    if (entry.injectionState == nullptr || !entry.injectionState->targetUid) {
        return {};
    }
    const int32_t uid = *entry.injectionState->targetUid;
    if (window == nullptr) {
        return StringPrintf("No valid window target for injection into uid %d.", uid);
    }
    if (entry.injectionState->targetUid != window->getInfo()->ownerUid) {
        return StringPrintf("Injected event targeted at uid %d would be dispatched to window '%s' "
                            "owned by uid %d.",
                            uid, window->getName().c_str(), window->getInfo()->ownerUid);
    }
    return {};
}
std::pair<float, float> resolveTouchedPosition(const MotionEntry& entry) {
    const bool isFromMouse = isFromSource(entry.source, AINPUT_SOURCE_MOUSE);
    if (isFromMouse) {
        return {entry.xCursorPosition, entry.yCursorPosition};
    }
    const int32_t pointerIndex = getMotionEventActionPointerIndex(entry.action);
    return {entry.pointerCoords[pointerIndex].getAxisValue(AMOTION_EVENT_AXIS_X),
            entry.pointerCoords[pointerIndex].getAxisValue(AMOTION_EVENT_AXIS_Y)};
}
std::optional<nsecs_t> getDownTime(const EventEntry& eventEntry) {
    if (eventEntry.type == EventEntry::Type::KEY) {
        const KeyEntry& keyEntry = static_cast<const KeyEntry&>(eventEntry);
        return keyEntry.downTime;
    } else if (eventEntry.type == EventEntry::Type::MOTION) {
        const MotionEntry& motionEntry = static_cast<const MotionEntry&>(eventEntry);
        return motionEntry.downTime;
    }
    return std::nullopt;
}
std::vector<TouchedWindow> getHoveringWindowsLocked(const TouchState* oldState,
                                                    const TouchState& newTouchState,
                                                    const MotionEntry& entry) {
    std::vector<TouchedWindow> out;
    const int32_t maskedAction = MotionEvent::getActionMasked(entry.action);
    if (maskedAction != AMOTION_EVENT_ACTION_HOVER_ENTER &&
        maskedAction != AMOTION_EVENT_ACTION_HOVER_MOVE &&
        maskedAction != AMOTION_EVENT_ACTION_HOVER_EXIT) {
        return out;
    }
    const int32_t pointerId = entry.pointerProperties[0].id;
    std::set<sp<WindowInfoHandle>> oldWindows;
    if (oldState != nullptr) {
        oldWindows = oldState->getWindowsWithHoveringPointer(entry.deviceId, pointerId);
    }
    std::set<sp<WindowInfoHandle>> newWindows =
            newTouchState.getWindowsWithHoveringPointer(entry.deviceId, pointerId);
    for (const sp<WindowInfoHandle>& oldWindow : oldWindows) {
        if (newWindows.find(oldWindow) == newWindows.end()) {
            TouchedWindow touchedWindow;
            touchedWindow.windowHandle = oldWindow;
            touchedWindow.targetFlags = InputTarget::Flags::DISPATCH_AS_HOVER_EXIT;
            touchedWindow.pointerIds.set(pointerId);
            out.push_back(touchedWindow);
        }
    }
    for (const sp<WindowInfoHandle>& newWindow : newWindows) {
        TouchedWindow touchedWindow;
        touchedWindow.windowHandle = newWindow;
        if (oldWindows.find(newWindow) == oldWindows.end()) {
            touchedWindow.targetFlags = InputTarget::Flags::DISPATCH_AS_HOVER_ENTER;
        } else {
            LOG_ALWAYS_FATAL_IF(maskedAction != AMOTION_EVENT_ACTION_HOVER_MOVE);
            touchedWindow.targetFlags = InputTarget::Flags::DISPATCH_AS_IS;
        }
        touchedWindow.pointerIds.set(pointerId);
        if (canReceiveForegroundTouches(*newWindow->getInfo())) {
            touchedWindow.targetFlags |= InputTarget::Flags::FOREGROUND;
        }
        out.push_back(touchedWindow);
    }
    return out;
}
template <typename T>
std::vector<T>& operator+=(std::vector<T>& left, const std::vector<T>& right) {
    left.insert(left.end(), right.begin(), right.end());
    return left;
}
}
InputDispatcher::InputDispatcher(const sp<InputDispatcherPolicyInterface>& policy)
      : InputDispatcher(policy, STALE_EVENT_TIMEOUT) {}
InputDispatcher::InputDispatcher(const sp<InputDispatcherPolicyInterface>& policy,
                                 std::chrono::nanoseconds staleEventTimeout)
      : mPolicy(policy),
        mPendingEvent(nullptr),
        mLastDropReason(DropReason::NOT_DROPPED),
        mIdGenerator(IdGenerator::Source::INPUT_DISPATCHER),
        mAppSwitchSawKeyDown(false),
        mAppSwitchDueTime(LLONG_MAX),
        mNextUnblockedEvent(nullptr),
        mMonitorDispatchingTimeout(DEFAULT_INPUT_DISPATCHING_TIMEOUT),
        mDispatchEnabled(false),
        mDispatchFrozen(false),
        mInputFilterEnabled(false),
        mMaximumObscuringOpacityForTouch(1.0f),
        mFocusedDisplayId(ADISPLAY_ID_DEFAULT),
        mWindowTokenWithPointerCapture(nullptr),
        mStaleEventTimeout(staleEventTimeout),
        mLatencyAggregator(),
        mLatencyTracker(&mLatencyAggregator) {
    mLooper = sp<Looper>::make(false);
    mReporter = createInputReporter();
    mWindowInfoListener = sp<DispatcherWindowListener>::make(*this);
#if defined(__ANDROID__)
    SurfaceComposerClient::getDefault()->addWindowInfosListener(mWindowInfoListener);
#endif
    mKeyRepeatState.lastKeyEntry = nullptr;
    policy->getDispatcherConfiguration(&mConfig);
}
InputDispatcher::~InputDispatcher() {
    std::scoped_lock _l(mLock);
    resetKeyRepeatLocked();
    releasePendingEventLocked();
    drainInboundQueueLocked();
    mCommandQueue.clear();
    while (!mConnectionsByToken.empty()) {
        sp<Connection> connection = mConnectionsByToken.begin()->second;
        removeInputChannelLocked(connection->inputChannel->getConnectionToken(), false);
    }
}
status_t InputDispatcher::start() {
    if (mThread) {
        return ALREADY_EXISTS;
    }
    mThread = std::make_unique<InputThread>(
            "InputDispatcher", [this]() { dispatchOnce(); }, [this]() { mLooper->wake(); });
    return OK;
}
status_t InputDispatcher::stop() {
    if (mThread && mThread->isCallingThread()) {
        ALOGE("InputDispatcher cannot be stopped from its own thread!");
        return INVALID_OPERATION;
    }
    mThread.reset();
    return OK;
}
void InputDispatcher::dispatchOnce() {
    nsecs_t nextWakeupTime = LLONG_MAX;
    {
        std::scoped_lock _l(mLock);
        mDispatcherIsAlive.notify_all();
        if (!haveCommandsLocked()) {
            dispatchOnceInnerLocked(&nextWakeupTime);
        }
        if (runCommandsLockedInterruptable()) {
            nextWakeupTime = LLONG_MIN;
        }
        const nsecs_t nextAnrCheck = processAnrsLocked();
        nextWakeupTime = std::min(nextWakeupTime, nextAnrCheck);
        if (nextWakeupTime == LLONG_MAX) {
            mDispatcherEnteredIdle.notify_all();
        }
    }
    nsecs_t currentTime = now();
    int timeoutMillis = toMillisecondTimeoutDelay(currentTime, nextWakeupTime);
    mLooper->pollOnce(timeoutMillis);
}
void InputDispatcher::processNoFocusedWindowAnrLocked() {
    std::shared_ptr<InputApplicationHandle> focusedApplication =
            getValueByKey(mFocusedApplicationHandlesByDisplay, mAwaitedApplicationDisplayId);
    if (focusedApplication == nullptr ||
        focusedApplication->getApplicationToken() !=
                mAwaitedFocusedApplication->getApplicationToken()) {
        ALOGE("Waited for a focused window, but focused application has already changed to %s",
              focusedApplication->getName().c_str());
        return;
    }
    const sp<WindowInfoHandle>& focusedWindowHandle =
            getFocusedWindowHandleLocked(mAwaitedApplicationDisplayId);
    if (focusedWindowHandle != nullptr) {
        return;
    }
    onAnrLocked(mAwaitedFocusedApplication);
}
nsecs_t InputDispatcher::processAnrsLocked() {
    const nsecs_t currentTime = now();
    nsecs_t nextAnrCheck = LLONG_MAX;
    if (mNoFocusedWindowTimeoutTime.has_value() && mAwaitedFocusedApplication != nullptr) {
        if (currentTime >= *mNoFocusedWindowTimeoutTime) {
            processNoFocusedWindowAnrLocked();
            mAwaitedFocusedApplication.reset();
            mNoFocusedWindowTimeoutTime = std::nullopt;
            return LLONG_MIN;
        } else {
            nextAnrCheck = *mNoFocusedWindowTimeoutTime;
        }
    }
    nextAnrCheck = std::min(nextAnrCheck, mAnrTracker.firstTimeout());
    if (currentTime < nextAnrCheck) {
        return nextAnrCheck;
    }
    sp<Connection> connection = getConnectionLocked(mAnrTracker.firstToken());
    if (connection == nullptr) {
        ALOGE("Could not find connection for entry %" PRId64, mAnrTracker.firstTimeout());
        return nextAnrCheck;
    }
    connection->responsive = false;
    mAnrTracker.eraseToken(connection->inputChannel->getConnectionToken());
    onAnrLocked(connection);
    return LLONG_MIN;
}
std::chrono::nanoseconds InputDispatcher::getDispatchingTimeoutLocked(
        const sp<Connection>& connection) {
    if (connection->monitor) {
        return mMonitorDispatchingTimeout;
    }
    const sp<WindowInfoHandle> window =
            getWindowHandleLocked(connection->inputChannel->getConnectionToken());
    if (window != nullptr) {
        return window->getDispatchingTimeout(DEFAULT_INPUT_DISPATCHING_TIMEOUT);
    }
    return DEFAULT_INPUT_DISPATCHING_TIMEOUT;
}
void InputDispatcher::dispatchOnceInnerLocked(nsecs_t* nextWakeupTime) {
    nsecs_t currentTime = now();
    if (!mDispatchEnabled) {
        resetKeyRepeatLocked();
    }
    if (mDispatchFrozen) {
        if (DEBUG_FOCUS) {
            ALOGD("Dispatch frozen.  Waiting some more.");
        }
        return;
    }
    bool isAppSwitchDue = mAppSwitchDueTime <= currentTime;
    if (mAppSwitchDueTime < *nextWakeupTime) {
        *nextWakeupTime = mAppSwitchDueTime;
    }
    if (!mPendingEvent) {
        if (mInboundQueue.empty()) {
            if (isAppSwitchDue) {
                resetPendingAppSwitchLocked(false);
                isAppSwitchDue = false;
            }
            if (mKeyRepeatState.lastKeyEntry) {
                if (currentTime >= mKeyRepeatState.nextRepeatTime) {
                    mPendingEvent = synthesizeKeyRepeatLocked(currentTime);
                } else {
                    if (mKeyRepeatState.nextRepeatTime < *nextWakeupTime) {
                        *nextWakeupTime = mKeyRepeatState.nextRepeatTime;
                    }
                }
            }
            if (!mPendingEvent) {
                return;
            }
        } else {
            mPendingEvent = mInboundQueue.front();
            mInboundQueue.pop_front();
            traceInboundQueueLengthLocked();
        }
        if (mPendingEvent->policyFlags & POLICY_FLAG_PASS_TO_USER) {
            pokeUserActivityLocked(*mPendingEvent);
        }
    }
    ALOG_ASSERT(mPendingEvent != nullptr);
    bool done = false;
    DropReason dropReason = DropReason::NOT_DROPPED;
    if (!(mPendingEvent->policyFlags & POLICY_FLAG_PASS_TO_USER)) {
        dropReason = DropReason::POLICY;
    } else if (!mDispatchEnabled) {
        dropReason = DropReason::DISABLED;
    }
    if (mNextUnblockedEvent == mPendingEvent) {
        mNextUnblockedEvent = nullptr;
    }
    switch (mPendingEvent->type) {
        case EventEntry::Type::CONFIGURATION_CHANGED: {
            const ConfigurationChangedEntry& typedEntry =
                    static_cast<const ConfigurationChangedEntry&>(*mPendingEvent);
            done = dispatchConfigurationChangedLocked(currentTime, typedEntry);
            dropReason = DropReason::NOT_DROPPED;
            break;
        }
        case EventEntry::Type::DEVICE_RESET: {
            const DeviceResetEntry& typedEntry =
                    static_cast<const DeviceResetEntry&>(*mPendingEvent);
            done = dispatchDeviceResetLocked(currentTime, typedEntry);
            dropReason = DropReason::NOT_DROPPED;
            break;
        }
        case EventEntry::Type::FOCUS: {
            std::shared_ptr<FocusEntry> typedEntry =
                    std::static_pointer_cast<FocusEntry>(mPendingEvent);
            dispatchFocusLocked(currentTime, typedEntry);
            done = true;
            dropReason = DropReason::NOT_DROPPED;
            break;
        }
        case EventEntry::Type::TOUCH_MODE_CHANGED: {
            const auto typedEntry = std::static_pointer_cast<TouchModeEntry>(mPendingEvent);
            dispatchTouchModeChangeLocked(currentTime, typedEntry);
            done = true;
            dropReason = DropReason::NOT_DROPPED;
            break;
        }
        case EventEntry::Type::POINTER_CAPTURE_CHANGED: {
            const auto typedEntry =
                    std::static_pointer_cast<PointerCaptureChangedEntry>(mPendingEvent);
            dispatchPointerCaptureChangedLocked(currentTime, typedEntry, dropReason);
            done = true;
            break;
        }
        case EventEntry::Type::DRAG: {
            std::shared_ptr<DragEntry> typedEntry =
                    std::static_pointer_cast<DragEntry>(mPendingEvent);
            dispatchDragLocked(currentTime, typedEntry);
            done = true;
            break;
        }
        case EventEntry::Type::KEY: {
            std::shared_ptr<KeyEntry> keyEntry = std::static_pointer_cast<KeyEntry>(mPendingEvent);
            if (isAppSwitchDue) {
                if (isAppSwitchKeyEvent(*keyEntry)) {
                    resetPendingAppSwitchLocked(true);
                    isAppSwitchDue = false;
                } else if (dropReason == DropReason::NOT_DROPPED) {
                    dropReason = DropReason::APP_SWITCH;
                }
            }
            if (dropReason == DropReason::NOT_DROPPED && isStaleEvent(currentTime, *keyEntry)) {
                dropReason = DropReason::STALE;
            }
            if (dropReason == DropReason::NOT_DROPPED && mNextUnblockedEvent) {
                dropReason = DropReason::BLOCKED;
            }
            done = dispatchKeyLocked(currentTime, keyEntry, &dropReason, nextWakeupTime);
            break;
        }
        case EventEntry::Type::MOTION: {
            std::shared_ptr<MotionEntry> motionEntry =
                    std::static_pointer_cast<MotionEntry>(mPendingEvent);
            if (dropReason == DropReason::NOT_DROPPED && isAppSwitchDue) {
                dropReason = DropReason::APP_SWITCH;
            }
            if (dropReason == DropReason::NOT_DROPPED && isStaleEvent(currentTime, *motionEntry)) {
                dropReason = DropReason::STALE;
            }
            if (dropReason == DropReason::NOT_DROPPED && mNextUnblockedEvent) {
                dropReason = DropReason::BLOCKED;
            }
            done = dispatchMotionLocked(currentTime, motionEntry, &dropReason, nextWakeupTime);
            break;
        }
        case EventEntry::Type::SENSOR: {
            std::shared_ptr<SensorEntry> sensorEntry =
                    std::static_pointer_cast<SensorEntry>(mPendingEvent);
            if (dropReason == DropReason::NOT_DROPPED && isAppSwitchDue) {
                dropReason = DropReason::APP_SWITCH;
            }
            nsecs_t bootTime = systemTime(SYSTEM_TIME_BOOTTIME);
            if (dropReason == DropReason::NOT_DROPPED && isStaleEvent(bootTime, *sensorEntry)) {
                dropReason = DropReason::STALE;
            }
            dispatchSensorLocked(currentTime, sensorEntry, &dropReason, nextWakeupTime);
            done = true;
            break;
        }
    }
    if (done) {
        if (dropReason != DropReason::NOT_DROPPED) {
            dropInboundEventLocked(*mPendingEvent, dropReason);
        }
        mLastDropReason = dropReason;
        releasePendingEventLocked();
        *nextWakeupTime = LLONG_MIN;
    }
}
bool InputDispatcher::isStaleEvent(nsecs_t currentTime, const EventEntry& entry) {
    return std::chrono::nanoseconds(currentTime - entry.eventTime) >= mStaleEventTimeout;
}
bool InputDispatcher::shouldPruneInboundQueueLocked(const MotionEntry& motionEntry) {
    const bool isPointerDownEvent = motionEntry.action == AMOTION_EVENT_ACTION_DOWN &&
            isFromSource(motionEntry.source, AINPUT_SOURCE_CLASS_POINTER);
    if (isPointerDownEvent && mAwaitedFocusedApplication != nullptr) {
        const int32_t displayId = motionEntry.displayId;
        const auto [x, y] = resolveTouchedPosition(motionEntry);
        const bool isStylus = isPointerFromStylus(motionEntry, 0);
        auto [touchedWindowHandle, _] = findTouchedWindowAtLocked(displayId, x, y, isStylus);
        if (touchedWindowHandle != nullptr &&
            touchedWindowHandle->getApplicationToken() !=
                    mAwaitedFocusedApplication->getApplicationToken()) {
            ALOGI("Pruning input queue because user touched a different application while waiting "
                  "for %s",
                  mAwaitedFocusedApplication->getName().c_str());
            return true;
        }
        const std::vector<sp<WindowInfoHandle>> touchedSpies =
                findTouchedSpyWindowsAtLocked(displayId, x, y, isStylus);
        for (const auto& windowHandle : touchedSpies) {
            const sp<Connection> connection = getConnectionLocked(windowHandle->getToken());
            if (connection != nullptr && connection->responsive) {
                ALOGW("Pruning the input queue because %s is unresponsive, but we have a "
                      "responsive spy window that may handle the event.",
                      mAwaitedFocusedApplication->getName().c_str());
                return true;
            }
        }
    }
    if (isPointerDownEvent && mKeyIsWaitingForEventsTimeout) {
        ALOGD("Received a new pointer down event, stop waiting for events to process and "
              "just send the pending key event to the focused window.");
        mKeyIsWaitingForEventsTimeout = now();
    }
    return false;
}
bool InputDispatcher::enqueueInboundEventLocked(std::unique_ptr<EventEntry> newEntry) {
    bool needWake = mInboundQueue.empty();
    mInboundQueue.push_back(std::move(newEntry));
    EventEntry& entry = *(mInboundQueue.back());
    traceInboundQueueLengthLocked();
    switch (entry.type) {
        case EventEntry::Type::KEY: {
            LOG_ALWAYS_FATAL_IF((entry.policyFlags & POLICY_FLAG_TRUSTED) == 0,
                                "Unexpected untrusted event.");
            const KeyEntry& keyEntry = static_cast<const KeyEntry&>(entry);
            if (isAppSwitchKeyEvent(keyEntry)) {
                if (keyEntry.action == AKEY_EVENT_ACTION_DOWN) {
                    mAppSwitchSawKeyDown = true;
                } else if (keyEntry.action == AKEY_EVENT_ACTION_UP) {
                    if (mAppSwitchSawKeyDown) {
                        if (DEBUG_APP_SWITCH) {
                            ALOGD("App switch is pending!");
                        }
                        mAppSwitchDueTime = keyEntry.eventTime + APP_SWITCH_TIMEOUT;
                        mAppSwitchSawKeyDown = false;
                        needWake = true;
                    }
                }
            }
            if (keyEntry.action == AKEY_EVENT_ACTION_UP && mPendingEvent &&
                mPendingEvent->type == EventEntry::Type::KEY) {
                KeyEntry& pendingKey = static_cast<KeyEntry&>(*mPendingEvent);
                if (pendingKey.keyCode == keyEntry.keyCode &&
                    pendingKey.interceptKeyResult ==
                            KeyEntry::InterceptKeyResult::TRY_AGAIN_LATER) {
                    pendingKey.interceptKeyResult = KeyEntry::InterceptKeyResult::UNKNOWN;
                    pendingKey.interceptKeyWakeupTime = 0;
                    needWake = true;
                }
            }
            break;
        }
        case EventEntry::Type::MOTION: {
            LOG_ALWAYS_FATAL_IF((entry.policyFlags & POLICY_FLAG_TRUSTED) == 0,
                                "Unexpected untrusted event.");
            if (shouldPruneInboundQueueLocked(static_cast<MotionEntry&>(entry))) {
                mNextUnblockedEvent = mInboundQueue.back();
                needWake = true;
            }
            break;
        }
        case EventEntry::Type::FOCUS: {
            LOG_ALWAYS_FATAL("Focus events should be inserted using enqueueFocusEventLocked");
            break;
        }
        case EventEntry::Type::TOUCH_MODE_CHANGED:
        case EventEntry::Type::CONFIGURATION_CHANGED:
        case EventEntry::Type::DEVICE_RESET:
        case EventEntry::Type::SENSOR:
        case EventEntry::Type::POINTER_CAPTURE_CHANGED:
        case EventEntry::Type::DRAG: {
            break;
        }
    }
    return needWake;
}
void InputDispatcher::addRecentEventLocked(std::shared_ptr<EventEntry> entry) {
    if (entry->type != EventEntry::Type::SENSOR) {
        mRecentQueue.push_back(entry);
    }
    if (mRecentQueue.size() > RECENT_QUEUE_MAX_SIZE) {
        mRecentQueue.pop_front();
    }
}
std::pair<sp<WindowInfoHandle>, std::vector<InputTarget>>
InputDispatcher::findTouchedWindowAtLocked(int32_t displayId, float x, float y, bool isStylus,
                                           bool ignoreDragWindow) const {
    std::vector<InputTarget> outsideTargets;
    const auto& windowHandles = getWindowHandlesLocked(displayId);
    for (const sp<WindowInfoHandle>& windowHandle : windowHandles) {
        if (ignoreDragWindow && haveSameToken(windowHandle, mDragState->dragWindow)) {
            continue;
        }
        const WindowInfo& info = *windowHandle->getInfo();
        if (!info.isSpy() &&
            windowAcceptsTouchAt(info, displayId, x, y, isStylus, getTransformLocked(displayId))) {
            return {windowHandle, outsideTargets};
        }
        if (info.inputConfig.test(WindowInfo::InputConfig::WATCH_OUTSIDE_TOUCH)) {
            addWindowTargetLocked(windowHandle, InputTarget::Flags::DISPATCH_AS_OUTSIDE,
                                                 {}, std::nullopt,
                                  outsideTargets);
        }
    }
    return {nullptr, {}};
}
std::vector<sp<WindowInfoHandle>> InputDispatcher::findTouchedSpyWindowsAtLocked(
        int32_t displayId, float x, float y, bool isStylus) const {
    std::vector<sp<WindowInfoHandle>> spyWindows;
    const auto& windowHandles = getWindowHandlesLocked(displayId);
    for (const sp<WindowInfoHandle>& windowHandle : windowHandles) {
        const WindowInfo& info = *windowHandle->getInfo();
        if (!windowAcceptsTouchAt(info, displayId, x, y, isStylus, getTransformLocked(displayId))) {
            continue;
        }
        if (!info.isSpy()) {
            return spyWindows;
        }
        spyWindows.push_back(windowHandle);
    }
    return spyWindows;
}
void InputDispatcher::dropInboundEventLocked(const EventEntry& entry, DropReason dropReason) {
    const char* reason;
    switch (dropReason) {
        case DropReason::POLICY:
            if (debugInboundEventDetails()) {
                ALOGD("Dropped event because policy consumed it.");
            }
            reason = "inbound event was dropped because the policy consumed it";
            break;
        case DropReason::DISABLED:
            if (mLastDropReason != DropReason::DISABLED) {
                ALOGI("Dropped event because input dispatch is disabled.");
            }
            reason = "inbound event was dropped because input dispatch is disabled";
            break;
        case DropReason::APP_SWITCH:
            ALOGI("Dropped event because of pending overdue app switch.");
            reason = "inbound event was dropped because of pending overdue app switch";
            break;
        case DropReason::BLOCKED:
            ALOGI("Dropped event because the current application is not responding and the user "
                  "has started interacting with a different application.");
            reason = "inbound event was dropped because the current application is not responding "
                     "and the user has started interacting with a different application";
            break;
        case DropReason::STALE:
            ALOGI("Dropped event because it is stale.");
            reason = "inbound event was dropped because it is stale";
            break;
        case DropReason::NO_POINTER_CAPTURE:
            ALOGI("Dropped event because there is no window with Pointer Capture.");
            reason = "inbound event was dropped because there is no window with Pointer Capture";
            break;
        case DropReason::NOT_DROPPED: {
            LOG_ALWAYS_FATAL("Should not be dropping a NOT_DROPPED event");
            return;
        }
    }
    switch (entry.type) {
        case EventEntry::Type::KEY: {
            CancelationOptions options(CancelationOptions::Mode::CANCEL_NON_POINTER_EVENTS, reason);
            synthesizeCancelationEventsForAllConnectionsLocked(options);
            break;
        }
        case EventEntry::Type::MOTION: {
            const MotionEntry& motionEntry = static_cast<const MotionEntry&>(entry);
            if (motionEntry.source & AINPUT_SOURCE_CLASS_POINTER) {
                CancelationOptions options(CancelationOptions::Mode::CANCEL_POINTER_EVENTS, reason);
                synthesizeCancelationEventsForAllConnectionsLocked(options);
            } else {
                CancelationOptions options(CancelationOptions::Mode::CANCEL_NON_POINTER_EVENTS,
                                           reason);
                synthesizeCancelationEventsForAllConnectionsLocked(options);
            }
            break;
        }
        case EventEntry::Type::SENSOR: {
            break;
        }
        case EventEntry::Type::POINTER_CAPTURE_CHANGED:
        case EventEntry::Type::DRAG: {
            break;
        }
        case EventEntry::Type::FOCUS:
        case EventEntry::Type::TOUCH_MODE_CHANGED:
        case EventEntry::Type::CONFIGURATION_CHANGED:
        case EventEntry::Type::DEVICE_RESET: {
            LOG_ALWAYS_FATAL("Should not drop %s events", ftl::enum_string(entry.type).c_str());
            break;
        }
    }
}
static bool isAppSwitchKeyCode(int32_t keyCode) {
    return keyCode == AKEYCODE_HOME || keyCode == AKEYCODE_ENDCALL ||
            keyCode == AKEYCODE_APP_SWITCH;
}
bool InputDispatcher::isAppSwitchKeyEvent(const KeyEntry& keyEntry) {
    return !(keyEntry.flags & AKEY_EVENT_FLAG_CANCELED) && isAppSwitchKeyCode(keyEntry.keyCode) &&
            (keyEntry.policyFlags & POLICY_FLAG_TRUSTED) &&
            (keyEntry.policyFlags & POLICY_FLAG_PASS_TO_USER);
}
bool InputDispatcher::isAppSwitchPendingLocked() {
    return mAppSwitchDueTime != LLONG_MAX;
}
void InputDispatcher::resetPendingAppSwitchLocked(bool handled) {
    mAppSwitchDueTime = LLONG_MAX;
    if (DEBUG_APP_SWITCH) {
        if (handled) {
            ALOGD("App switch has arrived.");
        } else {
            ALOGD("App switch was abandoned.");
        }
    }
}
bool InputDispatcher::haveCommandsLocked() const {
    return !mCommandQueue.empty();
}
bool InputDispatcher::runCommandsLockedInterruptable() {
    if (mCommandQueue.empty()) {
        return false;
    }
    do {
        auto command = std::move(mCommandQueue.front());
        mCommandQueue.pop_front();
        command();
    } while (!mCommandQueue.empty());
    return true;
}
void InputDispatcher::postCommandLocked(Command&& command) {
    mCommandQueue.push_back(command);
}
void InputDispatcher::drainInboundQueueLocked() {
    while (!mInboundQueue.empty()) {
        std::shared_ptr<EventEntry> entry = mInboundQueue.front();
        mInboundQueue.pop_front();
        releaseInboundEventLocked(entry);
    }
    traceInboundQueueLengthLocked();
}
void InputDispatcher::releasePendingEventLocked() {
    if (mPendingEvent) {
        releaseInboundEventLocked(mPendingEvent);
        mPendingEvent = nullptr;
    }
}
void InputDispatcher::releaseInboundEventLocked(std::shared_ptr<EventEntry> entry) {
    InjectionState* injectionState = entry->injectionState;
    if (injectionState && injectionState->injectionResult == InputEventInjectionResult::PENDING) {
        if (DEBUG_DISPATCH_CYCLE) {
            ALOGD("Injected inbound event was dropped.");
        }
        setInjectionResult(*entry, InputEventInjectionResult::FAILED);
    }
    if (entry == mNextUnblockedEvent) {
        mNextUnblockedEvent = nullptr;
    }
    addRecentEventLocked(entry);
}
void InputDispatcher::resetKeyRepeatLocked() {
    if (mKeyRepeatState.lastKeyEntry) {
        mKeyRepeatState.lastKeyEntry = nullptr;
    }
}
std::shared_ptr<KeyEntry> InputDispatcher::synthesizeKeyRepeatLocked(nsecs_t currentTime) {
    std::shared_ptr<KeyEntry> entry = mKeyRepeatState.lastKeyEntry;
    uint32_t policyFlags = entry->policyFlags &
            (POLICY_FLAG_RAW_MASK | POLICY_FLAG_PASS_TO_USER | POLICY_FLAG_TRUSTED);
    std::shared_ptr<KeyEntry> newEntry =
            std::make_unique<KeyEntry>(mIdGenerator.nextId(), currentTime, entry->deviceId,
                                       entry->source, entry->displayId, policyFlags, entry->action,
                                       entry->flags, entry->keyCode, entry->scanCode,
                                       entry->metaState, entry->repeatCount + 1, entry->downTime);
    newEntry->syntheticRepeat = true;
    mKeyRepeatState.lastKeyEntry = newEntry;
    mKeyRepeatState.nextRepeatTime = currentTime + mConfig.keyRepeatDelay;
    return newEntry;
}
bool InputDispatcher::dispatchConfigurationChangedLocked(nsecs_t currentTime,
                                                         const ConfigurationChangedEntry& entry) {
    if (DEBUG_OUTBOUND_EVENT_DETAILS) {
        ALOGD("dispatchConfigurationChanged - eventTime=%" PRId64, entry.eventTime);
    }
    resetKeyRepeatLocked();
    auto command = [this, eventTime = entry.eventTime]() REQUIRES(mLock) {
        scoped_unlock unlock(mLock);
        mPolicy->notifyConfigurationChanged(eventTime);
    };
    postCommandLocked(std::move(command));
    return true;
}
bool InputDispatcher::dispatchDeviceResetLocked(nsecs_t currentTime,
                                                const DeviceResetEntry& entry) {
    if (DEBUG_OUTBOUND_EVENT_DETAILS) {
        ALOGD("dispatchDeviceReset - eventTime=%" PRId64 ", deviceId=%d", entry.eventTime,
              entry.deviceId);
    }
    if (mKeyRepeatState.lastKeyEntry && mKeyRepeatState.lastKeyEntry->deviceId == entry.deviceId) {
        resetKeyRepeatLocked();
    }
    CancelationOptions options(CancelationOptions::Mode::CANCEL_ALL_EVENTS, "device was reset");
    options.deviceId = entry.deviceId;
    synthesizeCancelationEventsForAllConnectionsLocked(options);
    return true;
}
void InputDispatcher::enqueueFocusEventLocked(const sp<IBinder>& windowToken, bool hasFocus,
                                              const std::string& reason) {
    if (mPendingEvent != nullptr) {
        mInboundQueue.push_front(mPendingEvent);
        mPendingEvent = nullptr;
    }
    std::unique_ptr<FocusEntry> focusEntry =
            std::make_unique<FocusEntry>(mIdGenerator.nextId(), now(), windowToken, hasFocus,
                                         reason);
    std::deque<std::shared_ptr<EventEntry>>::reverse_iterator it =
            std::find_if(mInboundQueue.rbegin(), mInboundQueue.rend(),
                         [](const std::shared_ptr<EventEntry>& event) {
                             return event->type == EventEntry::Type::FOCUS;
                         });
    mInboundQueue.insert(it.base(), std::move(focusEntry));
}
void InputDispatcher::dispatchFocusLocked(nsecs_t currentTime, std::shared_ptr<FocusEntry> entry) {
    std::shared_ptr<InputChannel> channel = getInputChannelLocked(entry->connectionToken);
    if (channel == nullptr) {
        return;
    }
    InputTarget target;
    target.inputChannel = channel;
    target.flags = InputTarget::Flags::DISPATCH_AS_IS;
    entry->dispatchInProgress = true;
    std::string message = std::string("Focus ") + (entry->hasFocus ? "entering " : "leaving ") +
            channel->getName();
    std::string reason = std::string("reason=").append(entry->reason);
    android_log_event_list(LOGTAG_INPUT_FOCUS) << message << reason << LOG_ID_EVENTS;
    dispatchEventLocked(currentTime, entry, {target});
}
void InputDispatcher::dispatchPointerCaptureChangedLocked(
        nsecs_t currentTime, const std::shared_ptr<PointerCaptureChangedEntry>& entry,
        DropReason& dropReason) {
    dropReason = DropReason::NOT_DROPPED;
    const bool haveWindowWithPointerCapture = mWindowTokenWithPointerCapture != nullptr;
    sp<IBinder> token;
    if (entry->pointerCaptureRequest.enable) {
        if (haveWindowWithPointerCapture &&
            (entry->pointerCaptureRequest == mCurrentPointerCaptureRequest)) {
            ALOGI("Skipping dispatch of Pointer Capture being enabled: no state change.");
            return;
        }
        if (!mCurrentPointerCaptureRequest.enable) {
            ALOGW("No window requested Pointer Capture.");
            dropReason = DropReason::NO_POINTER_CAPTURE;
            return;
        }
        if (entry->pointerCaptureRequest.seq != mCurrentPointerCaptureRequest.seq) {
            ALOGI("Skipping dispatch of Pointer Capture being enabled: sequence number mismatch.");
            return;
        }
        token = mFocusResolver.getFocusedWindowToken(mFocusedDisplayId);
        LOG_ALWAYS_FATAL_IF(!token, "Cannot find focused window for Pointer Capture.");
        mWindowTokenWithPointerCapture = token;
    } else {
        if (!haveWindowWithPointerCapture) {
            dropReason = DropReason::NOT_DROPPED;
            return;
        }
        token = mWindowTokenWithPointerCapture;
        mWindowTokenWithPointerCapture = nullptr;
        if (mCurrentPointerCaptureRequest.enable) {
            setPointerCaptureLocked(false);
        }
    }
    auto channel = getInputChannelLocked(token);
    if (channel == nullptr) {
        mWindowTokenWithPointerCapture = nullptr;
        if (mCurrentPointerCaptureRequest.enable) {
            setPointerCaptureLocked(false);
        }
        return;
    }
    InputTarget target;
    target.inputChannel = channel;
    target.flags = InputTarget::Flags::DISPATCH_AS_IS;
    entry->dispatchInProgress = true;
    dispatchEventLocked(currentTime, entry, {target});
    dropReason = DropReason::NOT_DROPPED;
}
void InputDispatcher::dispatchTouchModeChangeLocked(nsecs_t currentTime,
                                                    const std::shared_ptr<TouchModeEntry>& entry) {
    const std::vector<sp<WindowInfoHandle>>& windowHandles =
            getWindowHandlesLocked(entry->displayId);
    if (windowHandles.empty()) {
        return;
    }
    const std::vector<InputTarget> inputTargets =
            getInputTargetsFromWindowHandlesLocked(windowHandles);
    if (inputTargets.empty()) {
        return;
    }
    entry->dispatchInProgress = true;
    dispatchEventLocked(currentTime, entry, inputTargets);
}
std::vector<InputTarget> InputDispatcher::getInputTargetsFromWindowHandlesLocked(
        const std::vector<sp<WindowInfoHandle>>& windowHandles) const {
    std::vector<InputTarget> inputTargets;
    for (const sp<WindowInfoHandle>& handle : windowHandles) {
        const sp<IBinder>& token = handle->getToken();
        if (token == nullptr) {
            continue;
        }
        std::shared_ptr<InputChannel> channel = getInputChannelLocked(token);
        if (channel == nullptr) {
            continue;
        }
        InputTarget target;
        target.inputChannel = channel;
        target.flags = InputTarget::Flags::DISPATCH_AS_IS;
        inputTargets.push_back(target);
    }
    return inputTargets;
}
bool InputDispatcher::dispatchKeyLocked(nsecs_t currentTime, std::shared_ptr<KeyEntry> entry,
                                        DropReason* dropReason, nsecs_t* nextWakeupTime) {
    if (!entry->dispatchInProgress) {
        if (entry->repeatCount == 0 && entry->action == AKEY_EVENT_ACTION_DOWN &&
            (entry->policyFlags & POLICY_FLAG_TRUSTED) &&
            (!(entry->policyFlags & POLICY_FLAG_DISABLE_KEY_REPEAT))) {
            if (mKeyRepeatState.lastKeyEntry &&
                mKeyRepeatState.lastKeyEntry->keyCode == entry->keyCode &&
                mKeyRepeatState.lastKeyEntry->deviceId == entry->deviceId) {
                entry->repeatCount = mKeyRepeatState.lastKeyEntry->repeatCount + 1;
                resetKeyRepeatLocked();
                mKeyRepeatState.nextRepeatTime = LLONG_MAX;
            } else {
                resetKeyRepeatLocked();
                mKeyRepeatState.nextRepeatTime = entry->eventTime + mConfig.keyRepeatTimeout;
            }
            mKeyRepeatState.lastKeyEntry = entry;
        } else if (entry->action == AKEY_EVENT_ACTION_UP && mKeyRepeatState.lastKeyEntry &&
                   mKeyRepeatState.lastKeyEntry->deviceId != entry->deviceId) {
            if (debugInboundEventDetails()) {
                ALOGD("deviceId=%d got KEY_UP as stale", entry->deviceId);
            }
        } else if (!entry->syntheticRepeat) {
            resetKeyRepeatLocked();
        }
        if (entry->repeatCount == 1) {
            entry->flags |= AKEY_EVENT_FLAG_LONG_PRESS;
        } else {
            entry->flags &= ~AKEY_EVENT_FLAG_LONG_PRESS;
        }
        entry->dispatchInProgress = true;
        logOutboundKeyDetails("dispatchKey - ", *entry);
    }
    if (entry->interceptKeyResult == KeyEntry::InterceptKeyResult::TRY_AGAIN_LATER) {
        if (currentTime < entry->interceptKeyWakeupTime) {
            if (entry->interceptKeyWakeupTime < *nextWakeupTime) {
                *nextWakeupTime = entry->interceptKeyWakeupTime;
            }
            return false;
        }
        entry->interceptKeyResult = KeyEntry::InterceptKeyResult::UNKNOWN;
        entry->interceptKeyWakeupTime = 0;
    }
    if (entry->interceptKeyResult == KeyEntry::InterceptKeyResult::UNKNOWN) {
        if (entry->policyFlags & POLICY_FLAG_PASS_TO_USER) {
            sp<IBinder> focusedWindowToken =
                    mFocusResolver.getFocusedWindowToken(getTargetDisplayId(*entry));
            auto command = [this, focusedWindowToken, entry]() REQUIRES(mLock) {
                doInterceptKeyBeforeDispatchingCommand(focusedWindowToken, *entry);
            };
            postCommandLocked(std::move(command));
            return false;
        } else {
            entry->interceptKeyResult = KeyEntry::InterceptKeyResult::CONTINUE;
        }
    } else if (entry->interceptKeyResult == KeyEntry::InterceptKeyResult::SKIP) {
        if (*dropReason == DropReason::NOT_DROPPED) {
            *dropReason = DropReason::POLICY;
        }
    }
    if (*dropReason != DropReason::NOT_DROPPED) {
        setInjectionResult(*entry,
                           *dropReason == DropReason::POLICY ? InputEventInjectionResult::SUCCEEDED
                                                             : InputEventInjectionResult::FAILED);
        mReporter->reportDroppedKey(entry->id);
        return true;
    }
    InputEventInjectionResult injectionResult;
    sp<WindowInfoHandle> focusedWindow =
            findFocusedWindowTargetLocked(currentTime, *entry, nextWakeupTime,
                                                    injectionResult);
    if (injectionResult == InputEventInjectionResult::PENDING) {
        return false;
    }
    setInjectionResult(*entry, injectionResult);
    if (injectionResult != InputEventInjectionResult::SUCCEEDED) {
        return true;
    }
    LOG_ALWAYS_FATAL_IF(focusedWindow == nullptr);
    std::vector<InputTarget> inputTargets;
    addWindowTargetLocked(focusedWindow,
                          InputTarget::Flags::FOREGROUND | InputTarget::Flags::DISPATCH_AS_IS,
                                         {}, getDownTime(*entry), inputTargets);
    addGlobalMonitoringTargetsLocked(inputTargets, getTargetDisplayId(*entry));
    dispatchEventLocked(currentTime, entry, inputTargets);
    return true;
}
void InputDispatcher::logOutboundKeyDetails(const char* prefix, const KeyEntry& entry) {
    if (DEBUG_OUTBOUND_EVENT_DETAILS) {
        ALOGD("%seventTime=%" PRId64 ", deviceId=%d, source=0x%x, displayId=%" PRId32 ", "
              "policyFlags=0x%x, action=0x%x, flags=0x%x, keyCode=0x%x, scanCode=0x%x, "
              "metaState=0x%x, repeatCount=%d, downTime=%" PRId64,
              prefix, entry.eventTime, entry.deviceId, entry.source, entry.displayId,
              entry.policyFlags, entry.action, entry.flags, entry.keyCode, entry.scanCode,
              entry.metaState, entry.repeatCount, entry.downTime);
    }
}
void InputDispatcher::dispatchSensorLocked(nsecs_t currentTime,
                                           const std::shared_ptr<SensorEntry>& entry,
                                           DropReason* dropReason, nsecs_t* nextWakeupTime) {
    if (DEBUG_OUTBOUND_EVENT_DETAILS) {
        ALOGD("notifySensorEvent eventTime=%" PRId64 ", hwTimestamp=%" PRId64 ", deviceId=%d, "
              "source=0x%x, sensorType=%s",
              entry->eventTime, entry->hwTimestamp, entry->deviceId, entry->source,
              ftl::enum_string(entry->sensorType).c_str());
    }
    auto command = [this, entry]() REQUIRES(mLock) {
        scoped_unlock unlock(mLock);
        if (entry->accuracyChanged) {
            mPolicy->notifySensorAccuracy(entry->deviceId, entry->sensorType, entry->accuracy);
        }
        mPolicy->notifySensorEvent(entry->deviceId, entry->sensorType, entry->accuracy,
                                   entry->hwTimestamp, entry->values);
    };
    postCommandLocked(std::move(command));
}
bool InputDispatcher::flushSensor(int deviceId, InputDeviceSensorType sensorType) {
    if (DEBUG_OUTBOUND_EVENT_DETAILS) {
        ALOGD("flushSensor deviceId=%d, sensorType=%s", deviceId,
              ftl::enum_string(sensorType).c_str());
    }
    {
        std::scoped_lock _l(mLock);
        for (auto it = mInboundQueue.begin(); it != mInboundQueue.end(); it++) {
            std::shared_ptr<EventEntry> entry = *it;
            if (entry->type == EventEntry::Type::SENSOR) {
                it = mInboundQueue.erase(it);
                releaseInboundEventLocked(entry);
            }
        }
    }
    return true;
}
bool InputDispatcher::dispatchMotionLocked(nsecs_t currentTime, std::shared_ptr<MotionEntry> entry,
                                           DropReason* dropReason, nsecs_t* nextWakeupTime) {
    ATRACE_CALL();
    if (!entry->dispatchInProgress) {
        entry->dispatchInProgress = true;
        logOutboundMotionDetails("dispatchMotion - ", *entry);
    }
    if (*dropReason != DropReason::NOT_DROPPED) {
        setInjectionResult(*entry,
                           *dropReason == DropReason::POLICY ? InputEventInjectionResult::SUCCEEDED
                                                             : InputEventInjectionResult::FAILED);
        return true;
    }
    const bool isPointerEvent = isFromSource(entry->source, AINPUT_SOURCE_CLASS_POINTER);
    std::vector<InputTarget> inputTargets;
    bool conflictingPointerActions = false;
    InputEventInjectionResult injectionResult;
    if (isPointerEvent) {
        if (mDragState &&
            (entry->action & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            pilferPointersLocked(mDragState->dragWindow->getToken());
        }
        inputTargets =
                findTouchedWindowTargetsLocked(currentTime, *entry, &conflictingPointerActions,
                                                         injectionResult);
        LOG_ALWAYS_FATAL_IF(injectionResult != InputEventInjectionResult::SUCCEEDED &&
                            !inputTargets.empty());
    } else {
        sp<WindowInfoHandle> focusedWindow =
                findFocusedWindowTargetLocked(currentTime, *entry, nextWakeupTime, injectionResult);
        if (injectionResult == InputEventInjectionResult::SUCCEEDED) {
            LOG_ALWAYS_FATAL_IF(focusedWindow == nullptr);
            addWindowTargetLocked(focusedWindow,
                                  InputTarget::Flags::FOREGROUND |
                                          InputTarget::Flags::DISPATCH_AS_IS,
                                                 {}, getDownTime(*entry), inputTargets);
        }
    }
    if (injectionResult == InputEventInjectionResult::PENDING) {
        return false;
    }
    setInjectionResult(*entry, injectionResult);
    if (injectionResult == InputEventInjectionResult::TARGET_MISMATCH) {
        return true;
    }
    if (injectionResult != InputEventInjectionResult::SUCCEEDED) {
        CancelationOptions::Mode mode(
                isPointerEvent ? CancelationOptions::Mode::CANCEL_POINTER_EVENTS
                               : CancelationOptions::Mode::CANCEL_NON_POINTER_EVENTS);
        CancelationOptions options(mode, "input event injection failed");
        synthesizeCancelationEventsForMonitorsLocked(options);
        return true;
    }
    addGlobalMonitoringTargetsLocked(inputTargets, getTargetDisplayId(*entry));
    if (conflictingPointerActions) {
        CancelationOptions options(CancelationOptions::Mode::CANCEL_POINTER_EVENTS,
                                   "conflicting pointer actions");
        synthesizeCancelationEventsForAllConnectionsLocked(options);
    }
    dispatchEventLocked(currentTime, entry, inputTargets);
    return true;
}
void InputDispatcher::enqueueDragEventLocked(const sp<WindowInfoHandle>& windowHandle,
                                             bool isExiting, const int32_t rawX,
                                             const int32_t rawY) {
    const vec2 xy = windowHandle->getInfo()->transform.transform(vec2(rawX, rawY));
    std::unique_ptr<DragEntry> dragEntry =
            std::make_unique<DragEntry>(mIdGenerator.nextId(), now(), windowHandle->getToken(),
                                        isExiting, xy.x, xy.y);
    enqueueInboundEventLocked(std::move(dragEntry));
}
void InputDispatcher::dispatchDragLocked(nsecs_t currentTime, std::shared_ptr<DragEntry> entry) {
    std::shared_ptr<InputChannel> channel = getInputChannelLocked(entry->connectionToken);
    if (channel == nullptr) {
        return;
    }
    InputTarget target;
    target.inputChannel = channel;
    target.flags = InputTarget::Flags::DISPATCH_AS_IS;
    entry->dispatchInProgress = true;
    dispatchEventLocked(currentTime, entry, {target});
}
void InputDispatcher::logOutboundMotionDetails(const char* prefix, const MotionEntry& entry) {
    if (DEBUG_OUTBOUND_EVENT_DETAILS) {
        ALOGD("%seventTime=%" PRId64 ", deviceId=%d, source=%s, displayId=%" PRId32
              ", policyFlags=0x%x, "
              "action=%s, actionButton=0x%x, flags=0x%x, "
              "metaState=0x%x, buttonState=0x%x,"
              "edgeFlags=0x%x, xPrecision=%f, yPrecision=%f, downTime=%" PRId64,
              prefix, entry.eventTime, entry.deviceId,
              inputEventSourceToString(entry.source).c_str(), entry.displayId, entry.policyFlags,
              MotionEvent::actionToString(entry.action).c_str(), entry.actionButton, entry.flags,
              entry.metaState, entry.buttonState, entry.edgeFlags, entry.xPrecision,
              entry.yPrecision, entry.downTime);
        for (uint32_t i = 0; i < entry.pointerCount; i++) {
            ALOGD("  Pointer %d: id=%d, toolType=%d, "
                  "x=%f, y=%f, pressure=%f, size=%f, "
                  "touchMajor=%f, touchMinor=%f, toolMajor=%f, toolMinor=%f, "
                  "orientation=%f",
                  i, entry.pointerProperties[i].id, entry.pointerProperties[i].toolType,
                  entry.pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_X),
                  entry.pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_Y),
                  entry.pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_PRESSURE),
                  entry.pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_SIZE),
                  entry.pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_TOUCH_MAJOR),
                  entry.pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_TOUCH_MINOR),
                  entry.pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_TOOL_MAJOR),
                  entry.pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_TOOL_MINOR),
                  entry.pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_ORIENTATION));
        }
    }
}
void InputDispatcher::dispatchEventLocked(nsecs_t currentTime,
                                          std::shared_ptr<EventEntry> eventEntry,
                                          const std::vector<InputTarget>& inputTargets) {
    ATRACE_CALL();
    if (DEBUG_DISPATCH_CYCLE) {
        ALOGD("dispatchEventToCurrentInputTargets");
    }
    updateInteractionTokensLocked(*eventEntry, inputTargets);
    ALOG_ASSERT(eventEntry->dispatchInProgress);
    pokeUserActivityLocked(*eventEntry);
    for (const InputTarget& inputTarget : inputTargets) {
        sp<Connection> connection =
                getConnectionLocked(inputTarget.inputChannel->getConnectionToken());
        if (connection != nullptr) {
            prepareDispatchCycleLocked(currentTime, connection, eventEntry, inputTarget);
        } else {
            if (DEBUG_FOCUS) {
                ALOGD("Dropping event delivery to target with channel '%s' because it "
                      "is no longer registered with the input dispatcher.",
                      inputTarget.inputChannel->getName().c_str());
            }
        }
    }
}
void InputDispatcher::cancelEventsForAnrLocked(const sp<Connection>& connection) {
    ALOGW("Canceling events for %s because it is unresponsive",
          connection->inputChannel->getName().c_str());
    if (connection->status == Connection::Status::NORMAL) {
        CancelationOptions options(CancelationOptions::Mode::CANCEL_ALL_EVENTS,
                                   "application not responding");
        synthesizeCancelationEventsForConnectionLocked(connection, options);
    }
}
void InputDispatcher::resetNoFocusedWindowTimeoutLocked() {
    if (DEBUG_FOCUS) {
        ALOGD("Resetting ANR timeouts.");
    }
    mNoFocusedWindowTimeoutTime = std::nullopt;
    mAwaitedFocusedApplication.reset();
}
int32_t InputDispatcher::getTargetDisplayId(const EventEntry& entry) {
    int32_t displayId;
    switch (entry.type) {
        case EventEntry::Type::KEY: {
            const KeyEntry& keyEntry = static_cast<const KeyEntry&>(entry);
            displayId = keyEntry.displayId;
            break;
        }
        case EventEntry::Type::MOTION: {
            const MotionEntry& motionEntry = static_cast<const MotionEntry&>(entry);
            displayId = motionEntry.displayId;
            break;
        }
        case EventEntry::Type::TOUCH_MODE_CHANGED:
        case EventEntry::Type::POINTER_CAPTURE_CHANGED:
        case EventEntry::Type::FOCUS:
        case EventEntry::Type::CONFIGURATION_CHANGED:
        case EventEntry::Type::DEVICE_RESET:
        case EventEntry::Type::SENSOR:
        case EventEntry::Type::DRAG: {
            ALOGE("%s events do not have a target display", ftl::enum_string(entry.type).c_str());
            return ADISPLAY_ID_NONE;
        }
    }
    return displayId == ADISPLAY_ID_NONE ? mFocusedDisplayId : displayId;
}
bool InputDispatcher::shouldWaitToSendKeyLocked(nsecs_t currentTime,
                                                const char* focusedWindowName) {
    if (mAnrTracker.empty()) {
        mKeyIsWaitingForEventsTimeout = std::nullopt;
        return false;
    }
    if (!mKeyIsWaitingForEventsTimeout.has_value()) {
        mKeyIsWaitingForEventsTimeout = currentTime +
                std::chrono::duration_cast<std::chrono::nanoseconds>(KEY_WAITING_FOR_EVENTS_TIMEOUT)
                        .count();
        return true;
    }
    if (currentTime < *mKeyIsWaitingForEventsTimeout) {
        return true;
    }
    ALOGW("Dispatching key to %s even though there are other unprocessed events",
          focusedWindowName);
    mKeyIsWaitingForEventsTimeout = std::nullopt;
    return false;
}
sp<WindowInfoHandle> InputDispatcher::findFocusedWindowTargetLocked(
        nsecs_t currentTime, const EventEntry& entry, nsecs_t* nextWakeupTime,
        InputEventInjectionResult& outInjectionResult) {
    std::string reason;
    outInjectionResult = InputEventInjectionResult::FAILED;
    int32_t displayId = getTargetDisplayId(entry);
    sp<WindowInfoHandle> focusedWindowHandle = getFocusedWindowHandleLocked(displayId);
    std::shared_ptr<InputApplicationHandle> focusedApplicationHandle =
            getValueByKey(mFocusedApplicationHandlesByDisplay, displayId);
    if (focusedWindowHandle == nullptr && focusedApplicationHandle == nullptr) {
        ALOGI("Dropping %s event because there is no focused window or focused application in "
              "display %" PRId32 ".",
              ftl::enum_string(entry.type).c_str(), displayId);
        return nullptr;
    }
    if (focusedWindowHandle != nullptr && shouldDropInput(entry, focusedWindowHandle)) {
        return nullptr;
    }
    if (focusedWindowHandle == nullptr && focusedApplicationHandle != nullptr) {
        if (!mNoFocusedWindowTimeoutTime.has_value()) {
            std::chrono::nanoseconds timeout = focusedApplicationHandle->getDispatchingTimeout(
                    DEFAULT_INPUT_DISPATCHING_TIMEOUT);
            mNoFocusedWindowTimeoutTime = currentTime + timeout.count();
            mAwaitedFocusedApplication = focusedApplicationHandle;
            mAwaitedApplicationDisplayId = displayId;
            ALOGW("Waiting because no window has focus but %s may eventually add a "
                  "window when it finishes starting up. Will wait for %" PRId64 "ms",
                  mAwaitedFocusedApplication->getName().c_str(), millis(timeout));
            *nextWakeupTime = *mNoFocusedWindowTimeoutTime;
            outInjectionResult = InputEventInjectionResult::PENDING;
            return nullptr;
        } else if (currentTime > *mNoFocusedWindowTimeoutTime) {
            ALOGE("Dropping %s event because there is no focused window",
                  ftl::enum_string(entry.type).c_str());
            return nullptr;
        } else {
            outInjectionResult = InputEventInjectionResult::PENDING;
            return nullptr;
        }
    }
    resetNoFocusedWindowTimeoutLocked();
    if (const auto err = verifyTargetedInjection(focusedWindowHandle, entry); err) {
        ALOGW("Dropping injected event: %s", (*err).c_str());
        outInjectionResult = InputEventInjectionResult::TARGET_MISMATCH;
        return nullptr;
    }
    if (focusedWindowHandle->getInfo()->inputConfig.test(
                WindowInfo::InputConfig::PAUSE_DISPATCHING)) {
        ALOGI("Waiting because %s is paused", focusedWindowHandle->getName().c_str());
        outInjectionResult = InputEventInjectionResult::PENDING;
        return nullptr;
    }
    if (entry.type == EventEntry::Type::KEY) {
        if (shouldWaitToSendKeyLocked(currentTime, focusedWindowHandle->getName().c_str())) {
            *nextWakeupTime = *mKeyIsWaitingForEventsTimeout;
            outInjectionResult = InputEventInjectionResult::PENDING;
            return nullptr;
        }
    }
    outInjectionResult = InputEventInjectionResult::SUCCEEDED;
    return focusedWindowHandle;
}
std::vector<Monitor> InputDispatcher::selectResponsiveMonitorsLocked(
        const std::vector<Monitor>& monitors) const {
    std::vector<Monitor> responsiveMonitors;
    std::copy_if(monitors.begin(), monitors.end(), std::back_inserter(responsiveMonitors),
                 [this](const Monitor& monitor) REQUIRES(mLock) {
                     sp<Connection> connection =
                             getConnectionLocked(monitor.inputChannel->getConnectionToken());
                     if (connection == nullptr) {
                         ALOGE("Could not find connection for monitor %s",
                               monitor.inputChannel->getName().c_str());
                         return false;
                     }
                     if (!connection->responsive) {
                         ALOGW("Unresponsive monitor %s will not get the new gesture",
                               connection->inputChannel->getName().c_str());
                         return false;
                     }
                     return true;
                 });
    return responsiveMonitors;
}
bool InputDispatcher::shouldSplitTouch(const TouchState& touchState,
                                       const MotionEntry& entry) const {
    if (isFromSource(entry.source, AINPUT_SOURCE_MOUSE)) {
        return false;
    }
    for (const TouchedWindow& touchedWindow : touchState.windows) {
        if (touchedWindow.windowHandle->getInfo()->isSpy()) {
            continue;
        }
        if (touchedWindow.windowHandle->getInfo()->supportsSplitTouch()) {
            continue;
        }
        if (touchedWindow.windowHandle->getInfo()->inputConfig.test(
                    gui::WindowInfo::InputConfig::IS_WALLPAPER)) {
            continue;
        }
        if (entry.deviceId == touchState.deviceId && touchedWindow.pointerIds.any()) {
            return false;
        }
    }
    return true;
}
std::vector<InputTarget> InputDispatcher::findTouchedWindowTargetsLocked(
        nsecs_t currentTime, const MotionEntry& entry, bool* outConflictingPointerActions,
        InputEventInjectionResult& outInjectionResult) {
    ATRACE_CALL();
    std::vector<InputTarget> targets;
    const int32_t displayId = entry.displayId;
    const int32_t action = entry.action;
    const int32_t maskedAction = action & AMOTION_EVENT_ACTION_MASK;
    outInjectionResult = InputEventInjectionResult::PENDING;
    const TouchState* oldState = nullptr;
    TouchState tempTouchState;
    if (const auto it = mTouchStatesByDisplay.find(displayId); it != mTouchStatesByDisplay.end()) {
        oldState = &(it->second);
        tempTouchState = *oldState;
    }
    bool isSplit = shouldSplitTouch(tempTouchState, entry);
    const bool switchedDevice = (oldState != nullptr) &&
            (oldState->deviceId != entry.deviceId || oldState->source != entry.source);
    const bool isHoverAction = (maskedAction == AMOTION_EVENT_ACTION_HOVER_MOVE ||
                                maskedAction == AMOTION_EVENT_ACTION_HOVER_ENTER ||
                                maskedAction == AMOTION_EVENT_ACTION_HOVER_EXIT);
    const bool wasDown = oldState != nullptr && oldState->isDown();
    const bool isDown = (maskedAction == AMOTION_EVENT_ACTION_DOWN) ||
            (maskedAction == AMOTION_EVENT_ACTION_POINTER_DOWN && !wasDown);
    const bool newGesture = isDown || maskedAction == AMOTION_EVENT_ACTION_SCROLL || isHoverAction;
    const bool isFromMouse = isFromSource(entry.source, AINPUT_SOURCE_MOUSE);
    if (switchedDevice && wasDown && !isDown) {
        LOG(INFO) << "Dropping event because a pointer for device " << oldState->deviceId
                  << " is already down in display " << displayId << ": " << entry.getDescription();
        outInjectionResult = InputEventInjectionResult::FAILED;
        return {};
    }
    if (newGesture) {
        tempTouchState.reset();
        tempTouchState.deviceId = entry.deviceId;
        tempTouchState.source = entry.source;
        isSplit = false;
    } else if (switchedDevice && maskedAction == AMOTION_EVENT_ACTION_MOVE) {
        ALOGI("Dropping move event because a pointer for a different device is already active "
              "in display %" PRId32,
              displayId);
        outInjectionResult = InputEventInjectionResult::FAILED;
        return {};
    }
    if (isHoverAction) {
        tempTouchState.clearHoveringPointers();
    }
    if (newGesture || (isSplit && maskedAction == AMOTION_EVENT_ACTION_POINTER_DOWN)) {
        const auto [x, y] = resolveTouchedPosition(entry);
        const int32_t pointerIndex = getMotionEventActionPointerIndex(action);
        const bool isStylus = isPointerFromStylus(entry, pointerIndex);
        auto [newTouchedWindowHandle, outsideTargets] =
                findTouchedWindowAtLocked(displayId, x, y, isStylus);
        if (isDown) {
            targets += outsideTargets;
        }
        if (newTouchedWindowHandle == nullptr) {
            ALOGD("No new touched window at (%.1f, %.1f) in display %" PRId32, x, y, displayId);
            newTouchedWindowHandle = tempTouchState.getFirstForegroundWindowHandle();
        }
        if (const auto err = verifyTargetedInjection(newTouchedWindowHandle, entry); err) {
            ALOGW("Dropping injected touch event: %s", (*err).c_str());
            outInjectionResult = os::InputEventInjectionResult::TARGET_MISMATCH;
            newTouchedWindowHandle = nullptr;
            return {};
        }
        if (newTouchedWindowHandle != nullptr) {
            if (newTouchedWindowHandle->getInfo()->supportsSplitTouch()) {
                isSplit = !isFromMouse;
            } else if (isSplit) {
                newTouchedWindowHandle = nullptr;
            }
        } else {
            isSplit = !isFromMouse;
        }
        std::vector<sp<WindowInfoHandle>> newTouchedWindows =
                findTouchedSpyWindowsAtLocked(displayId, x, y, isStylus);
        if (newTouchedWindowHandle != nullptr) {
            newTouchedWindows.insert(newTouchedWindows.begin(), newTouchedWindowHandle);
        }
        if (newTouchedWindows.empty()) {
            ALOGI("Dropping event because there is no touchable window at (%.1f, %.1f) on display "
                  "%d.",
                  x, y, displayId);
            outInjectionResult = InputEventInjectionResult::FAILED;
            return {};
        }
        for (const sp<WindowInfoHandle>& windowHandle : newTouchedWindows) {
            if (!canWindowReceiveMotionLocked(windowHandle, entry)) {
                continue;
            }
            if (isHoverAction) {
                const int32_t pointerId = entry.pointerProperties[0].id;
                if (maskedAction == AMOTION_EVENT_ACTION_HOVER_EXIT) {
                    tempTouchState.removeHoveringPointer(entry.deviceId, pointerId);
                } else {
                    tempTouchState.addHoveringPointerToWindow(windowHandle, entry.deviceId,
                                                              pointerId);
                }
            }
            ftl::Flags<InputTarget::Flags> targetFlags = InputTarget::Flags::DISPATCH_AS_IS;
            if (canReceiveForegroundTouches(*windowHandle->getInfo())) {
                targetFlags |= InputTarget::Flags::FOREGROUND;
            }
            if (isSplit) {
                targetFlags |= InputTarget::Flags::SPLIT;
            }
            if (isWindowObscuredAtPointLocked(windowHandle, x, y)) {
                targetFlags |= InputTarget::Flags::WINDOW_IS_OBSCURED;
            } else if (isWindowObscuredLocked(windowHandle)) {
                targetFlags |= InputTarget::Flags::WINDOW_IS_PARTIALLY_OBSCURED;
            }
            std::bitset<MAX_POINTER_ID + 1> pointerIds;
            if (!isHoverAction) {
                pointerIds.set(entry.pointerProperties[pointerIndex].id);
            }
            const bool isDownOrPointerDown = maskedAction == AMOTION_EVENT_ACTION_DOWN ||
                    maskedAction == AMOTION_EVENT_ACTION_POINTER_DOWN;
            tempTouchState.addOrUpdateWindow(windowHandle, targetFlags, pointerIds,
                                             isDownOrPointerDown
                                                     ? std::make_optional(entry.eventTime)
                                                     : std::nullopt);
            if (isDownOrPointerDown) {
                if (targetFlags.test(InputTarget::Flags::FOREGROUND) &&
                    windowHandle->getInfo()->inputConfig.test(
                            gui::WindowInfo::InputConfig::DUPLICATE_TOUCH_TO_WALLPAPER)) {
                    sp<WindowInfoHandle> wallpaper = findWallpaperWindowBelow(windowHandle);
                    if (wallpaper != nullptr) {
                        ftl::Flags<InputTarget::Flags> wallpaperFlags =
                                InputTarget::Flags::WINDOW_IS_OBSCURED |
                                InputTarget::Flags::WINDOW_IS_PARTIALLY_OBSCURED |
                                InputTarget::Flags::DISPATCH_AS_IS;
                        if (isSplit) {
                            wallpaperFlags |= InputTarget::Flags::SPLIT;
                        }
                        tempTouchState.addOrUpdateWindow(wallpaper, wallpaperFlags, pointerIds,
                                                         entry.eventTime);
                    }
                }
            }
        }
        const int32_t pointerId = entry.pointerProperties[pointerIndex].id;
        for (TouchedWindow& touchedWindow : tempTouchState.windows) {
            if (touchedWindow.pointerIds.test(pointerId) &&
                touchedWindow.pilferedPointerIds.count() > 0) {
                touchedWindow.pilferedPointerIds.set(pointerId);
            }
        }
        tempTouchState.cancelPointersForNonPilferingWindows();
    } else {
        if (!tempTouchState.isDown()) {
            LOG(INFO) << "Dropping event because the pointer is not down or we previously "
                         "dropped the pointer down event in display "
                      << displayId << ": " << entry.getDescription();
            outInjectionResult = InputEventInjectionResult::FAILED;
            return {};
        }
        addDragEventLocked(entry);
        if (maskedAction == AMOTION_EVENT_ACTION_MOVE && entry.pointerCount == 1 &&
            tempTouchState.isSlippery()) {
            const auto [x, y] = resolveTouchedPosition(entry);
            const bool isStylus = isPointerFromStylus(entry, 0);
            sp<WindowInfoHandle> oldTouchedWindowHandle =
                    tempTouchState.getFirstForegroundWindowHandle();
            auto [newTouchedWindowHandle, _] = findTouchedWindowAtLocked(displayId, x, y, isStylus);
            if (const auto err = verifyTargetedInjection(newTouchedWindowHandle, entry); err) {
                ALOGW("Dropping injected event: %s", (*err).c_str());
                outInjectionResult = os::InputEventInjectionResult::TARGET_MISMATCH;
                return {};
            }
            if (newTouchedWindowHandle != nullptr &&
                shouldDropInput(entry, newTouchedWindowHandle)) {
                newTouchedWindowHandle = nullptr;
            }
            if (oldTouchedWindowHandle != newTouchedWindowHandle &&
                oldTouchedWindowHandle != nullptr && newTouchedWindowHandle != nullptr) {
                if (DEBUG_FOCUS) {
                    ALOGD("Touch is slipping out of window %s into window %s in display %" PRId32,
                          oldTouchedWindowHandle->getName().c_str(),
                          newTouchedWindowHandle->getName().c_str(), displayId);
                }
                std::bitset<MAX_POINTER_ID + 1> pointerIds;
                const int32_t pointerId = entry.pointerProperties[0].id;
                pointerIds.set(pointerId);
                const TouchedWindow& touchedWindow =
                        tempTouchState.getTouchedWindow(oldTouchedWindowHandle);
                addWindowTargetLocked(oldTouchedWindowHandle,
                                      InputTarget::Flags::DISPATCH_AS_SLIPPERY_EXIT, pointerIds,
                                      touchedWindow.firstDownTimeInTarget, targets);
                if (newTouchedWindowHandle->getInfo()->supportsSplitTouch()) {
                    isSplit = !isFromMouse;
                }
                ftl::Flags<InputTarget::Flags> targetFlags =
                        InputTarget::Flags::DISPATCH_AS_SLIPPERY_ENTER;
                if (canReceiveForegroundTouches(*newTouchedWindowHandle->getInfo())) {
                    targetFlags |= InputTarget::Flags::FOREGROUND;
                }
                if (isSplit) {
                    targetFlags |= InputTarget::Flags::SPLIT;
                }
                if (isWindowObscuredAtPointLocked(newTouchedWindowHandle, x, y)) {
                    targetFlags |= InputTarget::Flags::WINDOW_IS_OBSCURED;
                } else if (isWindowObscuredLocked(newTouchedWindowHandle)) {
                    targetFlags |= InputTarget::Flags::WINDOW_IS_PARTIALLY_OBSCURED;
                }
                tempTouchState.addOrUpdateWindow(newTouchedWindowHandle, targetFlags, pointerIds,
                                                 entry.eventTime);
                slipWallpaperTouch(targetFlags, oldTouchedWindowHandle, newTouchedWindowHandle,
                                   tempTouchState, pointerId, targets);
                tempTouchState.removeTouchedPointerFromWindow(pointerId, oldTouchedWindowHandle);
            }
        }
        if (!isSplit && maskedAction == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            const int32_t pointerIndex = getMotionEventActionPointerIndex(action);
            for (size_t i = 0; i < tempTouchState.windows.size(); i++) {
                TouchedWindow& touchedWindow = tempTouchState.windows[i];
                if (mDragState && mDragState->dragWindow == touchedWindow.windowHandle) {
                    continue;
                }
                touchedWindow.pointerIds.set(entry.pointerProperties[pointerIndex].id);
            }
        }
    }
    {
        std::vector<TouchedWindow> hoveringWindows =
                getHoveringWindowsLocked(oldState, tempTouchState, entry);
        for (const TouchedWindow& touchedWindow : hoveringWindows) {
            addWindowTargetLocked(touchedWindow.windowHandle, touchedWindow.targetFlags,
                                  touchedWindow.pointerIds, touchedWindow.firstDownTimeInTarget,
                                  targets);
        }
    }
    if (std::none_of(tempTouchState.windows.begin(), tempTouchState.windows.end(),
                     [](const TouchedWindow& touchedWindow) {
                         return !canReceiveForegroundTouches(
                                        *touchedWindow.windowHandle->getInfo()) ||
                                 touchedWindow.targetFlags.test(InputTarget::Flags::FOREGROUND);
                     })) {
        ALOGI("Dropping event because there is no touched window on display %d to receive it: %s",
              displayId, entry.getDescription().c_str());
        outInjectionResult = InputEventInjectionResult::FAILED;
        return {};
    }
    if (entry.injectionState != nullptr) {
        std::string errs;
        for (const TouchedWindow& touchedWindow : tempTouchState.windows) {
            if (touchedWindow.targetFlags.test(InputTarget::Flags::DISPATCH_AS_OUTSIDE)) {
                continue;
            }
            const auto err = verifyTargetedInjection(touchedWindow.windowHandle, entry);
            if (err) errs += "\n  - " + *err;
        }
        if (!errs.empty()) {
            ALOGW("Dropping targeted injection: At least one touched window is not owned by uid "
                  "%d:%s",
                  *entry.injectionState->targetUid, errs.c_str());
            outInjectionResult = InputEventInjectionResult::TARGET_MISMATCH;
            return {};
        }
    }
    if (maskedAction == AMOTION_EVENT_ACTION_DOWN) {
        sp<WindowInfoHandle> foregroundWindowHandle =
                tempTouchState.getFirstForegroundWindowHandle();
        if (foregroundWindowHandle) {
            const int32_t foregroundWindowUid = foregroundWindowHandle->getInfo()->ownerUid;
            for (InputTarget& target : targets) {
                if (target.flags.test(InputTarget::Flags::DISPATCH_AS_OUTSIDE)) {
                    sp<WindowInfoHandle> targetWindow =
                            getWindowHandleLocked(target.inputChannel->getConnectionToken());
                    if (targetWindow->getInfo()->ownerUid != foregroundWindowUid) {
                        target.flags |= InputTarget::Flags::ZERO_COORDS;
                    }
                }
            }
        }
    }
    for (const TouchedWindow& touchedWindow : tempTouchState.windows) {
        if (touchedWindow.pointerIds.none() && !touchedWindow.hasHoveringPointers(entry.deviceId)) {
            continue;
        }
        addWindowTargetLocked(touchedWindow.windowHandle, touchedWindow.targetFlags,
                              touchedWindow.pointerIds, touchedWindow.firstDownTimeInTarget,
                              targets);
    }
    outInjectionResult = InputEventInjectionResult::SUCCEEDED;
    tempTouchState.filterNonAsIsTouchWindows();
    if (switchedDevice) {
        if (DEBUG_FOCUS) {
            ALOGD("Conflicting pointer actions: Switched to a different device.");
        }
        *outConflictingPointerActions = true;
    }
    if (isHoverAction) {
        if (oldState && oldState->isDown()) {
            ALOGD_IF(DEBUG_FOCUS,
                     "Conflicting pointer actions: Hover received while pointer was down.");
            *outConflictingPointerActions = true;
        }
        if (maskedAction == AMOTION_EVENT_ACTION_HOVER_ENTER ||
            maskedAction == AMOTION_EVENT_ACTION_HOVER_MOVE) {
            tempTouchState.deviceId = entry.deviceId;
            tempTouchState.source = entry.source;
        }
    } else if (maskedAction == AMOTION_EVENT_ACTION_UP) {
        tempTouchState.removeTouchedPointer(entry.pointerProperties[0].id);
    } else if (maskedAction == AMOTION_EVENT_ACTION_CANCEL) {
        tempTouchState.reset();
    } else if (maskedAction == AMOTION_EVENT_ACTION_DOWN) {
        if (oldState && (oldState->isDown() || oldState->hasHoveringPointers())) {
            ALOGD("Conflicting pointer actions: Down received while already down or hovering.");
            *outConflictingPointerActions = true;
        }
    } else if (maskedAction == AMOTION_EVENT_ACTION_POINTER_UP) {
        int32_t pointerIndex = getMotionEventActionPointerIndex(action);
        uint32_t pointerId = entry.pointerProperties[pointerIndex].id;
        for (size_t i = 0; i < tempTouchState.windows.size();) {
            TouchedWindow& touchedWindow = tempTouchState.windows[i];
            touchedWindow.pointerIds.reset(pointerId);
            if (touchedWindow.pointerIds.none()) {
                tempTouchState.windows.erase(tempTouchState.windows.begin() + i);
                continue;
            }
            i += 1;
        }
    }
    if (maskedAction != AMOTION_EVENT_ACTION_SCROLL) {
        if (displayId >= 0) {
            tempTouchState.clearWindowsWithoutPointers();
            mTouchStatesByDisplay[displayId] = tempTouchState;
        } else {
            mTouchStatesByDisplay.erase(displayId);
        }
    }
    if (tempTouchState.windows.empty()) {
        mTouchStatesByDisplay.erase(displayId);
    }
    return targets;
}
void InputDispatcher::finishDragAndDrop(int32_t displayId, float x, float y) {
    constexpr bool isStylus = false;
    auto [dropWindow, _] =
            findTouchedWindowAtLocked(displayId, x, y, isStylus, true);
    if (dropWindow) {
        vec2 local = dropWindow->getInfo()->transform.transform(x, y);
        sendDropWindowCommandLocked(dropWindow->getToken(), local.x, local.y);
    } else {
        ALOGW("No window found when drop.");
        sendDropWindowCommandLocked(nullptr, 0, 0);
    }
    mDragState.reset();
}
void InputDispatcher::addDragEventLocked(const MotionEntry& entry) {
    if (!mDragState || mDragState->dragWindow->getInfo()->displayId != entry.displayId) {
        return;
    }
    if (!mDragState->isStartDrag) {
        mDragState->isStartDrag = true;
        mDragState->isStylusButtonDownAtStart =
                (entry.buttonState & AMOTION_EVENT_BUTTON_STYLUS_PRIMARY) != 0;
    }
    int32_t pointerIndex = 0;
    for (; static_cast<uint32_t>(pointerIndex) < entry.pointerCount; pointerIndex++) {
        const PointerProperties& pointerProperties = entry.pointerProperties[pointerIndex];
        if (pointerProperties.id == mDragState->pointerId) {
            break;
        }
    }
    if (uint32_t(pointerIndex) == entry.pointerCount) {
        LOG_ALWAYS_FATAL("Should find a valid pointer index by id %d", mDragState->pointerId);
        sendDropWindowCommandLocked(nullptr, 0, 0);
        mDragState.reset();
        return;
    }
    const int32_t maskedAction = entry.action & AMOTION_EVENT_ACTION_MASK;
    const int32_t x = entry.pointerCoords[pointerIndex].getX();
    const int32_t y = entry.pointerCoords[pointerIndex].getY();
    switch (maskedAction) {
        case AMOTION_EVENT_ACTION_MOVE: {
            bool isStylusButtonDown =
                    (entry.buttonState & AMOTION_EVENT_BUTTON_STYLUS_PRIMARY) != 0;
            if (mDragState->isStylusButtonDownAtStart && !isStylusButtonDown) {
                finishDragAndDrop(entry.displayId, x, y);
                return;
            }
            constexpr bool isStylus = false;
            auto [hoverWindowHandle, _] = findTouchedWindowAtLocked(entry.displayId, x, y, isStylus,
                                                                                         true);
            if (hoverWindowHandle != mDragState->dragHoverWindowHandle &&
                !haveSameToken(hoverWindowHandle, mDragState->dragHoverWindowHandle)) {
                if (mDragState->dragHoverWindowHandle != nullptr) {
                    enqueueDragEventLocked(mDragState->dragHoverWindowHandle, true, x,
                                           y);
                }
                mDragState->dragHoverWindowHandle = hoverWindowHandle;
            }
            if (hoverWindowHandle != nullptr) {
                enqueueDragEventLocked(hoverWindowHandle, false, x, y);
            }
            break;
        }
        case AMOTION_EVENT_ACTION_POINTER_UP:
            if (getMotionEventActionPointerIndex(entry.action) != pointerIndex) {
                break;
            }
            [[fallthrough]];
        case AMOTION_EVENT_ACTION_UP:
            finishDragAndDrop(entry.displayId, x, y);
            break;
        case AMOTION_EVENT_ACTION_CANCEL: {
            ALOGD("Receiving cancel when drag and drop.");
            sendDropWindowCommandLocked(nullptr, 0, 0);
            mDragState.reset();
            break;
        }
    }
}
void InputDispatcher::addWindowTargetLocked(const sp<WindowInfoHandle>& windowHandle,
                                            ftl::Flags<InputTarget::Flags> targetFlags,
                                            std::bitset<MAX_POINTER_ID + 1> pointerIds,
                                            std::optional<nsecs_t> firstDownTimeInTarget,
                                            std::vector<InputTarget>& inputTargets) const {
    std::vector<InputTarget>::iterator it =
            std::find_if(inputTargets.begin(), inputTargets.end(),
                         [&windowHandle](const InputTarget& inputTarget) {
                             return inputTarget.inputChannel->getConnectionToken() ==
                                     windowHandle->getToken();
                         });
    const WindowInfo* windowInfo = windowHandle->getInfo();
    if (it == inputTargets.end()) {
        InputTarget inputTarget;
        std::shared_ptr<InputChannel> inputChannel =
                getInputChannelLocked(windowHandle->getToken());
        if (inputChannel == nullptr) {
            ALOGW("Window %s already unregistered input channel", windowHandle->getName().c_str());
            return;
        }
        inputTarget.inputChannel = inputChannel;
        inputTarget.flags = targetFlags;
        inputTarget.globalScaleFactor = windowInfo->globalScaleFactor;
        inputTarget.firstDownTimeInTarget = firstDownTimeInTarget;
        const auto& displayInfoIt = mDisplayInfos.find(windowInfo->displayId);
        if (displayInfoIt != mDisplayInfos.end()) {
            inputTarget.displayTransform = displayInfoIt->second.transform;
        } else {
        }
        inputTargets.push_back(inputTarget);
        it = inputTargets.end() - 1;
    }
    ALOG_ASSERT(it->flags == targetFlags);
    ALOG_ASSERT(it->globalScaleFactor == windowInfo->globalScaleFactor);
    it->addPointers(pointerIds, windowInfo->transform);
}
void InputDispatcher::addGlobalMonitoringTargetsLocked(std::vector<InputTarget>& inputTargets,
                                                       int32_t displayId) {
    auto monitorsIt = mGlobalMonitorsByDisplay.find(displayId);
    if (monitorsIt == mGlobalMonitorsByDisplay.end()) return;
    for (const Monitor& monitor : selectResponsiveMonitorsLocked(monitorsIt->second)) {
        InputTarget target;
        target.inputChannel = monitor.inputChannel;
        target.flags = InputTarget::Flags::DISPATCH_AS_IS;
        if (const auto& it = mDisplayInfos.find(displayId); it != mDisplayInfos.end()) {
            target.displayTransform = it->second.transform;
        }
        target.setDefaultPointerTransform(target.displayTransform);
        inputTargets.push_back(target);
    }
}
static bool canBeObscuredBy(const sp<WindowInfoHandle>& windowHandle,
                            const sp<WindowInfoHandle>& otherHandle) {
    if (haveSameToken(windowHandle, otherHandle)) {
        return false;
    }
    auto info = windowHandle->getInfo();
    auto otherInfo = otherHandle->getInfo();
    if (otherInfo->inputConfig.test(WindowInfo::InputConfig::NOT_VISIBLE)) {
        return false;
    } else if (otherInfo->alpha == 0 &&
               otherInfo->inputConfig.test(WindowInfo::InputConfig::NOT_TOUCHABLE)) {
        return false;
    } else if (info->ownerUid == otherInfo->ownerUid) {
        return false;
    } else if (otherInfo->inputConfig.test(gui::WindowInfo::InputConfig::TRUSTED_OVERLAY)) {
        return false;
    } else if (otherInfo->displayId != info->displayId) {
        return false;
    }
    return true;
}
InputDispatcher::TouchOcclusionInfo InputDispatcher::computeTouchOcclusionInfoLocked(
        const sp<WindowInfoHandle>& windowHandle, int32_t x, int32_t y) const {
    const WindowInfo* windowInfo = windowHandle->getInfo();
    int32_t displayId = windowInfo->displayId;
    const std::vector<sp<WindowInfoHandle>>& windowHandles = getWindowHandlesLocked(displayId);
    TouchOcclusionInfo info;
    info.hasBlockingOcclusion = false;
    info.obscuringOpacity = 0;
    info.obscuringUid = -1;
    std::map<int32_t, float> opacityByUid;
    for (const sp<WindowInfoHandle>& otherHandle : windowHandles) {
        if (windowHandle == otherHandle) {
            break;
        }
        const WindowInfo* otherInfo = otherHandle->getInfo();
        if (canBeObscuredBy(windowHandle, otherHandle) && otherInfo->frameContainsPoint(x, y) &&
            !haveSameApplicationToken(windowInfo, otherInfo)) {
            if (DEBUG_TOUCH_OCCLUSION) {
                info.debugInfo.push_back(
                        dumpWindowForTouchOcclusion(otherInfo, false));
            }
            if (otherInfo->touchOcclusionMode == TouchOcclusionMode::BLOCK_UNTRUSTED) {
                info.hasBlockingOcclusion = true;
                info.obscuringUid = otherInfo->ownerUid;
                info.obscuringPackage = otherInfo->packageName;
                break;
            }
            if (otherInfo->touchOcclusionMode == TouchOcclusionMode::USE_OPACITY) {
                uint32_t uid = otherInfo->ownerUid;
                float opacity =
                        (opacityByUid.find(uid) == opacityByUid.end()) ? 0 : opacityByUid[uid];
                opacity = 1 - (1 - opacity) * (1 - otherInfo->alpha);
                opacityByUid[uid] = opacity;
                if (opacity > info.obscuringOpacity) {
                    info.obscuringOpacity = opacity;
                    info.obscuringUid = uid;
                    info.obscuringPackage = otherInfo->packageName;
                }
            }
        }
    }
    if (DEBUG_TOUCH_OCCLUSION) {
        info.debugInfo.push_back(
                dumpWindowForTouchOcclusion(windowInfo, true));
    }
    return info;
}
std::string InputDispatcher::dumpWindowForTouchOcclusion(const WindowInfo* info,
                                                         bool isTouchedWindow) const {
    return StringPrintf(INDENT2 "* %spackage=%s/%" PRId32 ", id=%" PRId32 ", mode=%s, alpha=%.2f, "
                                "frame=[%" PRId32 ",%" PRId32 "][%" PRId32 ",%" PRId32
                                "], touchableRegion=%s, window={%s}, inputConfig={%s}, "
                                "hasToken=%s, applicationInfo.name=%s, applicationInfo.token=%s\n",
                        isTouchedWindow ? "[TOUCHED] " : "", info->packageName.c_str(),
                        info->ownerUid, info->id, toString(info->touchOcclusionMode).c_str(),
                        info->alpha, info->frameLeft, info->frameTop, info->frameRight,
                        info->frameBottom, dumpRegion(info->touchableRegion).c_str(),
                        info->name.c_str(), info->inputConfig.string().c_str(),
                        toString(info->token != nullptr), info->applicationInfo.name.c_str(),
                        toString(info->applicationInfo.token).c_str());
}
bool InputDispatcher::isTouchTrustedLocked(const TouchOcclusionInfo& occlusionInfo) const {
    if (occlusionInfo.hasBlockingOcclusion) {
        ALOGW("Untrusted touch due to occlusion by %s/%d", occlusionInfo.obscuringPackage.c_str(),
              occlusionInfo.obscuringUid);
        return false;
    }
    if (occlusionInfo.obscuringOpacity > mMaximumObscuringOpacityForTouch) {
        ALOGW("Untrusted touch due to occlusion by %s/%d (obscuring opacity = "
              "%.2f, maximum allowed = %.2f)",
              occlusionInfo.obscuringPackage.c_str(), occlusionInfo.obscuringUid,
              occlusionInfo.obscuringOpacity, mMaximumObscuringOpacityForTouch);
        return false;
    }
    return true;
}
bool InputDispatcher::isWindowObscuredAtPointLocked(const sp<WindowInfoHandle>& windowHandle,
                                                    int32_t x, int32_t y) const {
    int32_t displayId = windowHandle->getInfo()->displayId;
    const std::vector<sp<WindowInfoHandle>>& windowHandles = getWindowHandlesLocked(displayId);
    for (const sp<WindowInfoHandle>& otherHandle : windowHandles) {
        if (windowHandle == otherHandle) {
            break;
        }
        const WindowInfo* otherInfo = otherHandle->getInfo();
        if (canBeObscuredBy(windowHandle, otherHandle) &&
            otherInfo->frameContainsPoint(x, y)) {
            return true;
        }
    }
    return false;
}
bool InputDispatcher::isWindowObscuredLocked(const sp<WindowInfoHandle>& windowHandle) const {
    int32_t displayId = windowHandle->getInfo()->displayId;
    const std::vector<sp<WindowInfoHandle>>& windowHandles = getWindowHandlesLocked(displayId);
    const WindowInfo* windowInfo = windowHandle->getInfo();
    for (const sp<WindowInfoHandle>& otherHandle : windowHandles) {
        if (windowHandle == otherHandle) {
            break;
        }
        const WindowInfo* otherInfo = otherHandle->getInfo();
        if (canBeObscuredBy(windowHandle, otherHandle) &&
            otherInfo->overlaps(windowInfo)) {
            return true;
        }
    }
    return false;
}
std::string InputDispatcher::getApplicationWindowLabel(
        const InputApplicationHandle* applicationHandle, const sp<WindowInfoHandle>& windowHandle) {
    if (applicationHandle != nullptr) {
        if (windowHandle != nullptr) {
            return applicationHandle->getName() + " - " + windowHandle->getName();
        } else {
            return applicationHandle->getName();
        }
    } else if (windowHandle != nullptr) {
        return windowHandle->getInfo()->applicationInfo.name + " - " + windowHandle->getName();
    } else {
        return "<unknown application or window>";
    }
}
void InputDispatcher::pokeUserActivityLocked(const EventEntry& eventEntry) {
    if (!isUserActivityEvent(eventEntry)) {
        return;
    }
    int32_t displayId = getTargetDisplayId(eventEntry);
    sp<WindowInfoHandle> focusedWindowHandle = getFocusedWindowHandleLocked(displayId);
    if (focusedWindowHandle != nullptr) {
        const WindowInfo* info = focusedWindowHandle->getInfo();
        if (info->inputConfig.test(WindowInfo::InputConfig::DISABLE_USER_ACTIVITY)) {
            if (DEBUG_DISPATCH_CYCLE) {
                ALOGD("Not poking user activity: disabled by window '%s'.", info->name.c_str());
            }
            return;
        }
    }
    int32_t eventType = USER_ACTIVITY_EVENT_OTHER;
    switch (eventEntry.type) {
        case EventEntry::Type::MOTION: {
            const MotionEntry& motionEntry = static_cast<const MotionEntry&>(eventEntry);
            if (motionEntry.action == AMOTION_EVENT_ACTION_CANCEL) {
                return;
            }
            if (MotionEvent::isTouchEvent(motionEntry.source, motionEntry.action)) {
                eventType = USER_ACTIVITY_EVENT_TOUCH;
            }
            break;
        }
        case EventEntry::Type::KEY: {
            const KeyEntry& keyEntry = static_cast<const KeyEntry&>(eventEntry);
            if (keyEntry.flags & AKEY_EVENT_FLAG_CANCELED) {
                return;
            }
            eventType = USER_ACTIVITY_EVENT_BUTTON;
            break;
        }
        default: {
            LOG_ALWAYS_FATAL("%s events are not user activity",
                             ftl::enum_string(eventEntry.type).c_str());
            break;
        }
    }
    auto command = [this, eventTime = eventEntry.eventTime, eventType, displayId]()
                           REQUIRES(mLock) {
                               scoped_unlock unlock(mLock);
                               mPolicy->pokeUserActivity(eventTime, eventType, displayId);
                           };
    postCommandLocked(std::move(command));
}
void InputDispatcher::prepareDispatchCycleLocked(nsecs_t currentTime,
                                                 const sp<Connection>& connection,
                                                 std::shared_ptr<EventEntry> eventEntry,
                                                 const InputTarget& inputTarget) {
    if (ATRACE_ENABLED()) {
        std::string message =
                StringPrintf("prepareDispatchCycleLocked(inputChannel=%s, id=0x%" PRIx32 ")",
                             connection->getInputChannelName().c_str(), eventEntry->id);
        ATRACE_NAME(message.c_str());
    }
    if (DEBUG_DISPATCH_CYCLE) {
        ALOGD("channel '%s' ~ prepareDispatchCycle - flags=%s, "
              "globalScaleFactor=%f, pointerIds=%s %s",
              connection->getInputChannelName().c_str(), inputTarget.flags.string().c_str(),
              inputTarget.globalScaleFactor, bitsetToString(inputTarget.pointerIds).c_str(),
              inputTarget.getPointerInfoString().c_str());
    }
    if (connection->status != Connection::Status::NORMAL) {
        if (DEBUG_DISPATCH_CYCLE) {
            ALOGD("channel '%s' ~ Dropping event because the channel status is %s",
                  connection->getInputChannelName().c_str(),
                  ftl::enum_string(connection->status).c_str());
        }
        return;
    }
    if (inputTarget.flags.test(InputTarget::Flags::SPLIT)) {
        LOG_ALWAYS_FATAL_IF(eventEntry->type != EventEntry::Type::MOTION,
                            "Entry type %s should not have Flags::SPLIT",
                            ftl::enum_string(eventEntry->type).c_str());
        const MotionEntry& originalMotionEntry = static_cast<const MotionEntry&>(*eventEntry);
        if (inputTarget.pointerIds.count() != originalMotionEntry.pointerCount) {
            if (!inputTarget.firstDownTimeInTarget.has_value()) {
                logDispatchStateLocked();
                LOG(FATAL) << "Splitting motion events requires a down time to be set for the "
                              "target on connection "
                           << connection->getInputChannelName() << " for "
                           << originalMotionEntry.getDescription();
            }
            std::unique_ptr<MotionEntry> splitMotionEntry =
                    splitMotionEvent(originalMotionEntry, inputTarget.pointerIds,
                                     inputTarget.firstDownTimeInTarget.value());
            if (!splitMotionEntry) {
                return;
            }
            if (splitMotionEntry->action == AMOTION_EVENT_ACTION_CANCEL) {
                std::string reason = std::string("reason=pointer cancel on split window");
                android_log_event_list(LOGTAG_INPUT_CANCEL)
                        << connection->getInputChannelName().c_str() << reason << LOG_ID_EVENTS;
            }
            if (DEBUG_FOCUS) {
                ALOGD("channel '%s' ~ Split motion event.",
                      connection->getInputChannelName().c_str());
                logOutboundMotionDetails("  ", *splitMotionEntry);
            }
            enqueueDispatchEntriesLocked(currentTime, connection, std::move(splitMotionEntry),
                                         inputTarget);
            return;
        }
    }
    enqueueDispatchEntriesLocked(currentTime, connection, eventEntry, inputTarget);
}
void InputDispatcher::enqueueDispatchEntriesLocked(nsecs_t currentTime,
                                                   const sp<Connection>& connection,
                                                   std::shared_ptr<EventEntry> eventEntry,
                                                   const InputTarget& inputTarget) {
    if (ATRACE_ENABLED()) {
        std::string message =
                StringPrintf("enqueueDispatchEntriesLocked(inputChannel=%s, id=0x%" PRIx32 ")",
                             connection->getInputChannelName().c_str(), eventEntry->id);
        ATRACE_NAME(message.c_str());
    }
    LOG_ALWAYS_FATAL_IF(!inputTarget.flags.any(InputTarget::DISPATCH_MASK),
                        "No dispatch flags are set for %s", eventEntry->getDescription().c_str());
    const bool wasEmpty = connection->outboundQueue.empty();
    enqueueDispatchEntryLocked(connection, eventEntry, inputTarget,
                               InputTarget::Flags::DISPATCH_AS_HOVER_EXIT);
    enqueueDispatchEntryLocked(connection, eventEntry, inputTarget,
                               InputTarget::Flags::DISPATCH_AS_OUTSIDE);
    enqueueDispatchEntryLocked(connection, eventEntry, inputTarget,
                               InputTarget::Flags::DISPATCH_AS_HOVER_ENTER);
    enqueueDispatchEntryLocked(connection, eventEntry, inputTarget,
                               InputTarget::Flags::DISPATCH_AS_IS);
    enqueueDispatchEntryLocked(connection, eventEntry, inputTarget,
                               InputTarget::Flags::DISPATCH_AS_SLIPPERY_EXIT);
    enqueueDispatchEntryLocked(connection, eventEntry, inputTarget,
                               InputTarget::Flags::DISPATCH_AS_SLIPPERY_ENTER);
    if (wasEmpty && !connection->outboundQueue.empty()) {
        startDispatchCycleLocked(currentTime, connection);
    }
}
void InputDispatcher::enqueueDispatchEntryLocked(const sp<Connection>& connection,
                                                 std::shared_ptr<EventEntry> eventEntry,
                                                 const InputTarget& inputTarget,
                                                 ftl::Flags<InputTarget::Flags> dispatchMode) {
    if (ATRACE_ENABLED()) {
        std::string message = StringPrintf("enqueueDispatchEntry(inputChannel=%s, dispatchMode=%s)",
                                           connection->getInputChannelName().c_str(),
                                           dispatchMode.string().c_str());
        ATRACE_NAME(message.c_str());
    }
    ftl::Flags<InputTarget::Flags> inputTargetFlags = inputTarget.flags;
    if (!inputTargetFlags.any(dispatchMode)) {
        return;
    }
    inputTargetFlags.clear(InputTarget::DISPATCH_MASK);
    inputTargetFlags |= dispatchMode;
    std::unique_ptr<DispatchEntry> dispatchEntry =
            createDispatchEntry(inputTarget, eventEntry, inputTargetFlags);
    EventEntry& newEntry = *(dispatchEntry->eventEntry);
    switch (newEntry.type) {
        case EventEntry::Type::KEY: {
            const KeyEntry& keyEntry = static_cast<const KeyEntry&>(newEntry);
            dispatchEntry->resolvedEventId = keyEntry.id;
            dispatchEntry->resolvedAction = keyEntry.action;
            dispatchEntry->resolvedFlags = keyEntry.flags;
            if (!connection->inputState.trackKey(keyEntry, dispatchEntry->resolvedAction,
                                                 dispatchEntry->resolvedFlags)) {
                if (DEBUG_DISPATCH_CYCLE) {
                    ALOGD("channel '%s' ~ enqueueDispatchEntryLocked: skipping inconsistent key "
                          "event",
                          connection->getInputChannelName().c_str());
                }
                return;
            }
            break;
        }
        case EventEntry::Type::MOTION: {
            const MotionEntry& motionEntry = static_cast<const MotionEntry&>(newEntry);
            constexpr int32_t DEFAULT_RESOLVED_EVENT_ID =
                    static_cast<int32_t>(IdGenerator::Source::OTHER);
            dispatchEntry->resolvedEventId = DEFAULT_RESOLVED_EVENT_ID;
            if (dispatchMode.test(InputTarget::Flags::DISPATCH_AS_OUTSIDE)) {
                dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_OUTSIDE;
            } else if (dispatchMode.test(InputTarget::Flags::DISPATCH_AS_HOVER_EXIT)) {
                dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_HOVER_EXIT;
            } else if (dispatchMode.test(InputTarget::Flags::DISPATCH_AS_HOVER_ENTER)) {
                dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_HOVER_ENTER;
            } else if (dispatchMode.test(InputTarget::Flags::DISPATCH_AS_SLIPPERY_EXIT)) {
                dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_CANCEL;
            } else if (dispatchMode.test(InputTarget::Flags::DISPATCH_AS_SLIPPERY_ENTER)) {
                dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_DOWN;
            } else {
                dispatchEntry->resolvedAction = motionEntry.action;
                dispatchEntry->resolvedEventId = motionEntry.id;
            }
            if (dispatchEntry->resolvedAction == AMOTION_EVENT_ACTION_HOVER_MOVE &&
                !connection->inputState.isHovering(motionEntry.deviceId, motionEntry.source,
                                                   motionEntry.displayId)) {
                if (DEBUG_DISPATCH_CYCLE) {
                    ALOGD("channel '%s' ~ enqueueDispatchEntryLocked: filling in missing hover "
                          "enter event",
                          connection->getInputChannelName().c_str());
                }
                dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_HOVER_ENTER;
            }
            dispatchEntry->resolvedFlags = motionEntry.flags;
            if (dispatchEntry->resolvedAction == AMOTION_EVENT_ACTION_CANCEL) {
                dispatchEntry->resolvedFlags |= AMOTION_EVENT_FLAG_CANCELED;
            }
            if (dispatchEntry->targetFlags.test(InputTarget::Flags::WINDOW_IS_OBSCURED)) {
                dispatchEntry->resolvedFlags |= AMOTION_EVENT_FLAG_WINDOW_IS_OBSCURED;
            }
            if (dispatchEntry->targetFlags.test(InputTarget::Flags::WINDOW_IS_PARTIALLY_OBSCURED)) {
                dispatchEntry->resolvedFlags |= AMOTION_EVENT_FLAG_WINDOW_IS_PARTIALLY_OBSCURED;
            }
            if (!connection->inputState.trackMotion(motionEntry, dispatchEntry->resolvedAction,
                                                    dispatchEntry->resolvedFlags)) {
                if (DEBUG_DISPATCH_CYCLE) {
                    ALOGD("channel '%s' ~ enqueueDispatchEntryLocked: skipping inconsistent motion "
                          "event",
                          connection->getInputChannelName().c_str());
                }
                return;
            }
            dispatchEntry->resolvedEventId =
                    dispatchEntry->resolvedEventId == DEFAULT_RESOLVED_EVENT_ID
                    ? mIdGenerator.nextId()
                    : motionEntry.id;
            if (ATRACE_ENABLED() && dispatchEntry->resolvedEventId != motionEntry.id) {
                std::string message = StringPrintf("Transmute MotionEvent(id=0x%" PRIx32
                                                   ") to MotionEvent(id=0x%" PRIx32 ").",
                                                   motionEntry.id, dispatchEntry->resolvedEventId);
                ATRACE_NAME(message.c_str());
            }
            if ((motionEntry.flags & AMOTION_EVENT_FLAG_NO_FOCUS_CHANGE) &&
                (motionEntry.policyFlags & POLICY_FLAG_TRUSTED)) {
                break;
            }
            dispatchPointerDownOutsideFocus(motionEntry.source, dispatchEntry->resolvedAction,
                                            inputTarget.inputChannel->getConnectionToken());
            break;
        }
        case EventEntry::Type::FOCUS:
        case EventEntry::Type::TOUCH_MODE_CHANGED:
        case EventEntry::Type::POINTER_CAPTURE_CHANGED:
        case EventEntry::Type::DRAG: {
            break;
        }
        case EventEntry::Type::SENSOR: {
            LOG_ALWAYS_FATAL("SENSOR events should not go to apps via input channel");
            break;
        }
        case EventEntry::Type::CONFIGURATION_CHANGED:
        case EventEntry::Type::DEVICE_RESET: {
            LOG_ALWAYS_FATAL("%s events should not go to apps",
                             ftl::enum_string(newEntry.type).c_str());
            break;
        }
    }
    if (dispatchEntry->hasForegroundTarget()) {
        incrementPendingForegroundDispatches(newEntry);
    }
    connection->outboundQueue.push_back(dispatchEntry.release());
    traceOutboundQueueLength(*connection);
}
void InputDispatcher::updateInteractionTokensLocked(const EventEntry& entry,
                                                    const std::vector<InputTarget>& targets) {
    if (entry.type == EventEntry::Type::KEY) {
        const KeyEntry& keyEntry = static_cast<const KeyEntry&>(entry);
        if (keyEntry.action == AKEY_EVENT_ACTION_UP) {
            return;
        }
    } else if (entry.type == EventEntry::Type::MOTION) {
        const MotionEntry& motionEntry = static_cast<const MotionEntry&>(entry);
        if (motionEntry.action == AMOTION_EVENT_ACTION_UP ||
            motionEntry.action == AMOTION_EVENT_ACTION_CANCEL) {
            return;
        }
    } else {
        return;
    }
    std::unordered_set<sp<IBinder>, StrongPointerHash<IBinder>> newConnectionTokens;
    std::vector<sp<Connection>> newConnections;
    for (const InputTarget& target : targets) {
        if (target.flags.test(InputTarget::Flags::DISPATCH_AS_OUTSIDE)) {
            continue;
        }
        sp<IBinder> token = target.inputChannel->getConnectionToken();
        sp<Connection> connection = getConnectionLocked(token);
        if (connection == nullptr) {
            continue;
        }
        newConnectionTokens.insert(std::move(token));
        newConnections.emplace_back(connection);
    }
    if (newConnectionTokens == mInteractionConnectionTokens) {
        return;
    }
    mInteractionConnectionTokens = newConnectionTokens;
    std::string targetList;
    for (const sp<Connection>& connection : newConnections) {
        targetList += connection->getWindowName() + ", ";
    }
    std::string message = "Interaction with: " + targetList;
    if (targetList.empty()) {
        message += "<none>";
    }
    android_log_event_list(LOGTAG_INPUT_INTERACTION) << message << LOG_ID_EVENTS;
}
void InputDispatcher::dispatchPointerDownOutsideFocus(uint32_t source, int32_t action,
                                                      const sp<IBinder>& token) {
    int32_t maskedAction = action & AMOTION_EVENT_ACTION_MASK;
    uint32_t maskedSource = source & AINPUT_SOURCE_CLASS_MASK;
    if (maskedSource != AINPUT_SOURCE_CLASS_POINTER || maskedAction != AMOTION_EVENT_ACTION_DOWN) {
        return;
    }
    sp<IBinder> focusedToken = mFocusResolver.getFocusedWindowToken(mFocusedDisplayId);
    if (focusedToken == token) {
        return;
    }
    auto command = [this, token]() REQUIRES(mLock) {
        scoped_unlock unlock(mLock);
        mPolicy->onPointerDownOutsideFocus(token);
    };
    postCommandLocked(std::move(command));
}
status_t InputDispatcher::publishMotionEvent(Connection& connection,
                                             DispatchEntry& dispatchEntry) const {
    const EventEntry& eventEntry = *(dispatchEntry.eventEntry);
    const MotionEntry& motionEntry = static_cast<const MotionEntry&>(eventEntry);
    PointerCoords scaledCoords[MAX_POINTERS];
    const PointerCoords* usingCoords = motionEntry.pointerCoords;
    if ((motionEntry.source & AINPUT_SOURCE_CLASS_POINTER) &&
        !(dispatchEntry.targetFlags.test(InputTarget::Flags::ZERO_COORDS))) {
        float globalScaleFactor = dispatchEntry.globalScaleFactor;
        if (globalScaleFactor != 1.0f) {
            for (uint32_t i = 0; i < motionEntry.pointerCount; i++) {
                scaledCoords[i] = motionEntry.pointerCoords[i];
                scaledCoords[i].scale(globalScaleFactor, 1, 1);
            }
            usingCoords = scaledCoords;
        }
    } else if (dispatchEntry.targetFlags.test(InputTarget::Flags::ZERO_COORDS)) {
        for (uint32_t i = 0; i < motionEntry.pointerCount; i++) {
            scaledCoords[i].clear();
        }
        usingCoords = scaledCoords;
    }
    std::array<uint8_t, 32> hmac = getSignature(motionEntry, dispatchEntry);
    return connection.inputPublisher
            .publishMotionEvent(dispatchEntry.seq, dispatchEntry.resolvedEventId,
                                motionEntry.deviceId, motionEntry.source, motionEntry.displayId,
                                std::move(hmac), dispatchEntry.resolvedAction,
                                motionEntry.actionButton, dispatchEntry.resolvedFlags,
                                motionEntry.edgeFlags, motionEntry.metaState,
                                motionEntry.buttonState, motionEntry.classification,
                                dispatchEntry.transform, motionEntry.xPrecision,
                                motionEntry.yPrecision, motionEntry.xCursorPosition,
                                motionEntry.yCursorPosition, dispatchEntry.rawTransform,
                                motionEntry.downTime, motionEntry.eventTime,
                                motionEntry.pointerCount, motionEntry.pointerProperties,
                                usingCoords);
}
void InputDispatcher::startDispatchCycleLocked(nsecs_t currentTime,
                                               const sp<Connection>& connection) {
    if (ATRACE_ENABLED()) {
        std::string message = StringPrintf("startDispatchCycleLocked(inputChannel=%s)",
                                           connection->getInputChannelName().c_str());
        ATRACE_NAME(message.c_str());
    }
    if (DEBUG_DISPATCH_CYCLE) {
        ALOGD("channel '%s' ~ startDispatchCycle", connection->getInputChannelName().c_str());
    }
    while (connection->status == Connection::Status::NORMAL && !connection->outboundQueue.empty()) {
        DispatchEntry* dispatchEntry = connection->outboundQueue.front();
        dispatchEntry->deliveryTime = currentTime;
        const std::chrono::nanoseconds timeout = getDispatchingTimeoutLocked(connection);
        dispatchEntry->timeoutTime = currentTime + timeout.count();
        status_t status;
        const EventEntry& eventEntry = *(dispatchEntry->eventEntry);
        switch (eventEntry.type) {
            case EventEntry::Type::KEY: {
                const KeyEntry& keyEntry = static_cast<const KeyEntry&>(eventEntry);
                std::array<uint8_t, 32> hmac = getSignature(keyEntry, *dispatchEntry);
                if (DEBUG_OUTBOUND_EVENT_DETAILS) {
                    LOG(DEBUG) << "Publishing " << *dispatchEntry << " to "
                               << connection->getInputChannelName();
                }
                status = connection->inputPublisher
                                 .publishKeyEvent(dispatchEntry->seq,
                                                  dispatchEntry->resolvedEventId, keyEntry.deviceId,
                                                  keyEntry.source, keyEntry.displayId,
                                                  std::move(hmac), dispatchEntry->resolvedAction,
                                                  dispatchEntry->resolvedFlags, keyEntry.keyCode,
                                                  keyEntry.scanCode, keyEntry.metaState,
                                                  keyEntry.repeatCount, keyEntry.downTime,
                                                  keyEntry.eventTime);
                break;
            }
            case EventEntry::Type::MOTION: {
                if (DEBUG_OUTBOUND_EVENT_DETAILS) {
                    LOG(DEBUG) << "Publishing " << *dispatchEntry << " to "
                               << connection->getInputChannelName();
                }
                status = publishMotionEvent(*connection, *dispatchEntry);
                break;
            }
            case EventEntry::Type::FOCUS: {
                const FocusEntry& focusEntry = static_cast<const FocusEntry&>(eventEntry);
                status = connection->inputPublisher.publishFocusEvent(dispatchEntry->seq,
                                                                      focusEntry.id,
                                                                      focusEntry.hasFocus);
                break;
            }
            case EventEntry::Type::TOUCH_MODE_CHANGED: {
                const TouchModeEntry& touchModeEntry =
                        static_cast<const TouchModeEntry&>(eventEntry);
                status = connection->inputPublisher
                                 .publishTouchModeEvent(dispatchEntry->seq, touchModeEntry.id,
                                                        touchModeEntry.inTouchMode);
                break;
            }
            case EventEntry::Type::POINTER_CAPTURE_CHANGED: {
                const auto& captureEntry =
                        static_cast<const PointerCaptureChangedEntry&>(eventEntry);
                status = connection->inputPublisher
                                 .publishCaptureEvent(dispatchEntry->seq, captureEntry.id,
                                                      captureEntry.pointerCaptureRequest.enable);
                break;
            }
            case EventEntry::Type::DRAG: {
                const DragEntry& dragEntry = static_cast<const DragEntry&>(eventEntry);
                status = connection->inputPublisher.publishDragEvent(dispatchEntry->seq,
                                                                     dragEntry.id, dragEntry.x,
                                                                     dragEntry.y,
                                                                     dragEntry.isExiting);
                break;
            }
            case EventEntry::Type::CONFIGURATION_CHANGED:
            case EventEntry::Type::DEVICE_RESET:
            case EventEntry::Type::SENSOR: {
                LOG_ALWAYS_FATAL("Should never start dispatch cycles for %s events",
                                 ftl::enum_string(eventEntry.type).c_str());
                return;
            }
        }
        if (status) {
            if (status == WOULD_BLOCK) {
                if (connection->waitQueue.empty()) {
                    ALOGE("channel '%s' ~ Could not publish event because the pipe is full. "
                          "This is unexpected because the wait queue is empty, so the pipe "
                          "should be empty and we shouldn't have any problems writing an "
                          "event to it, status=%s(%d)",
                          connection->getInputChannelName().c_str(), statusToString(status).c_str(),
                          status);
                    abortBrokenDispatchCycleLocked(currentTime, connection, true);
                } else {
                    if (DEBUG_DISPATCH_CYCLE) {
                        ALOGD("channel '%s' ~ Could not publish event because the pipe is full, "
                              "waiting for the application to catch up",
                              connection->getInputChannelName().c_str());
                    }
                }
            } else {
                ALOGE("channel '%s' ~ Could not publish event due to an unexpected error, "
                      "status=%s(%d)",
                      connection->getInputChannelName().c_str(), statusToString(status).c_str(),
                      status);
                abortBrokenDispatchCycleLocked(currentTime, connection, true);
            }
            return;
        }
        connection->outboundQueue.erase(std::remove(connection->outboundQueue.begin(),
                                                    connection->outboundQueue.end(),
                                                    dispatchEntry));
        traceOutboundQueueLength(*connection);
        connection->waitQueue.push_back(dispatchEntry);
        if (connection->responsive) {
            mAnrTracker.insert(dispatchEntry->timeoutTime,
                               connection->inputChannel->getConnectionToken());
        }
        traceWaitQueueLength(*connection);
    }
}
std::array<uint8_t, 32> InputDispatcher::sign(const VerifiedInputEvent& event) const {
    size_t size;
    switch (event.type) {
        case VerifiedInputEvent::Type::KEY: {
            size = sizeof(VerifiedKeyEvent);
            break;
        }
        case VerifiedInputEvent::Type::MOTION: {
            size = sizeof(VerifiedMotionEvent);
            break;
        }
    }
    const uint8_t* start = reinterpret_cast<const uint8_t*>(&event);
    return mHmacKeyManager.sign(start, size);
}
const std::array<uint8_t, 32> InputDispatcher::getSignature(
        const MotionEntry& motionEntry, const DispatchEntry& dispatchEntry) const {
    const int32_t actionMasked = dispatchEntry.resolvedAction & AMOTION_EVENT_ACTION_MASK;
    if (actionMasked != AMOTION_EVENT_ACTION_UP && actionMasked != AMOTION_EVENT_ACTION_DOWN) {
        return INVALID_HMAC;
    }
    VerifiedMotionEvent verifiedEvent =
            verifiedMotionEventFromMotionEntry(motionEntry, dispatchEntry.rawTransform);
    verifiedEvent.actionMasked = actionMasked;
    verifiedEvent.flags = dispatchEntry.resolvedFlags & VERIFIED_MOTION_EVENT_FLAGS;
    return sign(verifiedEvent);
}
const std::array<uint8_t, 32> InputDispatcher::getSignature(
        const KeyEntry& keyEntry, const DispatchEntry& dispatchEntry) const {
    VerifiedKeyEvent verifiedEvent = verifiedKeyEventFromKeyEntry(keyEntry);
    verifiedEvent.flags = dispatchEntry.resolvedFlags & VERIFIED_KEY_EVENT_FLAGS;
    verifiedEvent.action = dispatchEntry.resolvedAction;
    return sign(verifiedEvent);
}
void InputDispatcher::finishDispatchCycleLocked(nsecs_t currentTime,
                                                const sp<Connection>& connection, uint32_t seq,
                                                bool handled, nsecs_t consumeTime) {
    if (DEBUG_DISPATCH_CYCLE) {
        ALOGD("channel '%s' ~ finishDispatchCycle - seq=%u, handled=%s",
              connection->getInputChannelName().c_str(), seq, toString(handled));
    }
    if (connection->status == Connection::Status::BROKEN ||
        connection->status == Connection::Status::ZOMBIE) {
        return;
    }
    auto command = [this, currentTime, connection, seq, handled, consumeTime]() REQUIRES(mLock) {
        doDispatchCycleFinishedCommand(currentTime, connection, seq, handled, consumeTime);
    };
    postCommandLocked(std::move(command));
}
void InputDispatcher::abortBrokenDispatchCycleLocked(nsecs_t currentTime,
                                                     const sp<Connection>& connection,
                                                     bool notify) {
    if (DEBUG_DISPATCH_CYCLE) {
        ALOGD("channel '%s' ~ abortBrokenDispatchCycle - notify=%s",
              connection->getInputChannelName().c_str(), toString(notify));
    }
    drainDispatchQueue(connection->outboundQueue);
    traceOutboundQueueLength(*connection);
    drainDispatchQueue(connection->waitQueue);
    traceWaitQueueLength(*connection);
    if (connection->status == Connection::Status::NORMAL) {
        connection->status = Connection::Status::BROKEN;
        if (notify) {
            ALOGE("channel '%s' ~ Channel is unrecoverably broken and will be disposed!",
                  connection->getInputChannelName().c_str());
            auto command = [this, connection]() REQUIRES(mLock) {
                scoped_unlock unlock(mLock);
                mPolicy->notifyInputChannelBroken(connection->inputChannel->getConnectionToken());
            };
            postCommandLocked(std::move(command));
        }
    }
}
void InputDispatcher::drainDispatchQueue(std::deque<DispatchEntry*>& queue) {
    while (!queue.empty()) {
        DispatchEntry* dispatchEntry = queue.front();
        queue.pop_front();
        releaseDispatchEntry(dispatchEntry);
    }
}
void InputDispatcher::releaseDispatchEntry(DispatchEntry* dispatchEntry) {
    if (dispatchEntry->hasForegroundTarget()) {
        decrementPendingForegroundDispatches(*(dispatchEntry->eventEntry));
    }
    delete dispatchEntry;
}
int InputDispatcher::handleReceiveCallback(int events, sp<IBinder> connectionToken) {
    std::scoped_lock _l(mLock);
    sp<Connection> connection = getConnectionLocked(connectionToken);
    if (connection == nullptr) {
        ALOGW("Received looper callback for unknown input channel token %p.  events=0x%x",
              connectionToken.get(), events);
        return 0;
    }
    bool notify;
    if (!(events & (ALOOPER_EVENT_ERROR | ALOOPER_EVENT_HANGUP))) {
        if (!(events & ALOOPER_EVENT_INPUT)) {
            ALOGW("channel '%s' ~ Received spurious callback for unhandled poll event.  "
                  "events=0x%x",
                  connection->getInputChannelName().c_str(), events);
            return 1;
        }
        nsecs_t currentTime = now();
        bool gotOne = false;
        status_t status = OK;
        for (;;) {
            Result<InputPublisher::ConsumerResponse> result =
                    connection->inputPublisher.receiveConsumerResponse();
            if (!result.ok()) {
                status = result.error().code();
                break;
            }
            if (std::holds_alternative<InputPublisher::Finished>(*result)) {
                const InputPublisher::Finished& finish =
                        std::get<InputPublisher::Finished>(*result);
                finishDispatchCycleLocked(currentTime, connection, finish.seq, finish.handled,
                                          finish.consumeTime);
            } else if (std::holds_alternative<InputPublisher::Timeline>(*result)) {
                if (shouldReportMetricsForConnection(*connection)) {
                    const InputPublisher::Timeline& timeline =
                            std::get<InputPublisher::Timeline>(*result);
                    mLatencyTracker
                            .trackGraphicsLatency(timeline.inputEventId,
                                                  connection->inputChannel->getConnectionToken(),
                                                  std::move(timeline.graphicsTimeline));
                }
            }
            gotOne = true;
        }
        if (gotOne) {
            runCommandsLockedInterruptable();
            if (status == WOULD_BLOCK) {
                return 1;
            }
        }
        notify = status != DEAD_OBJECT || !connection->monitor;
        if (notify) {
            ALOGE("channel '%s' ~ Failed to receive finished signal.  status=%s(%d)",
                  connection->getInputChannelName().c_str(), statusToString(status).c_str(),
                  status);
        }
    } else {
        const bool stillHaveWindowHandle =
                getWindowHandleLocked(connection->inputChannel->getConnectionToken()) != nullptr;
        notify = !connection->monitor && stillHaveWindowHandle;
        if (notify) {
            ALOGW("channel '%s' ~ Consumer closed input channel or an error occurred.  events=0x%x",
                  connection->getInputChannelName().c_str(), events);
        }
    }
    removeInputChannelLocked(connection->inputChannel->getConnectionToken(), notify);
    return 0;
}
void InputDispatcher::synthesizeCancelationEventsForAllConnectionsLocked(
        const CancelationOptions& options) {
    for (const auto& [token, connection] : mConnectionsByToken) {
        synthesizeCancelationEventsForConnectionLocked(connection, options);
    }
}
void InputDispatcher::synthesizeCancelationEventsForMonitorsLocked(
        const CancelationOptions& options) {
    for (const auto& [_, monitors] : mGlobalMonitorsByDisplay) {
        for (const Monitor& monitor : monitors) {
            synthesizeCancelationEventsForInputChannelLocked(monitor.inputChannel, options);
        }
    }
}
void InputDispatcher::synthesizeCancelationEventsForInputChannelLocked(
        const std::shared_ptr<InputChannel>& channel, const CancelationOptions& options) {
    sp<Connection> connection = getConnectionLocked(channel->getConnectionToken());
    if (connection == nullptr) {
        return;
    }
    synthesizeCancelationEventsForConnectionLocked(connection, options);
}
void InputDispatcher::synthesizeCancelationEventsForConnectionLocked(
        const sp<Connection>& connection, const CancelationOptions& options) {
    if (connection->status == Connection::Status::BROKEN) {
        return;
    }
    nsecs_t currentTime = now();
    std::vector<std::unique_ptr<EventEntry>> cancelationEvents =
            connection->inputState.synthesizeCancelationEvents(currentTime, options);
    if (cancelationEvents.empty()) {
        return;
    }
    if (DEBUG_OUTBOUND_EVENT_DETAILS) {
        ALOGD("channel '%s' ~ Synthesized %zu cancelation events to bring channel back in sync "
              "with reality: %s, mode=%s.",
              connection->getInputChannelName().c_str(), cancelationEvents.size(), options.reason,
              ftl::enum_string(options.mode).c_str());
    }
    std::string reason = std::string("reason=").append(options.reason);
    android_log_event_list(LOGTAG_INPUT_CANCEL)
            << connection->getInputChannelName().c_str() << reason << LOG_ID_EVENTS;
    InputTarget target;
    sp<WindowInfoHandle> windowHandle =
            getWindowHandleLocked(connection->inputChannel->getConnectionToken());
    if (windowHandle != nullptr) {
        const WindowInfo* windowInfo = windowHandle->getInfo();
        target.setDefaultPointerTransform(windowInfo->transform);
        target.globalScaleFactor = windowInfo->globalScaleFactor;
    }
    target.inputChannel = connection->inputChannel;
    target.flags = InputTarget::Flags::DISPATCH_AS_IS;
    const bool wasEmpty = connection->outboundQueue.empty();
    for (size_t i = 0; i < cancelationEvents.size(); i++) {
        std::unique_ptr<EventEntry> cancelationEventEntry = std::move(cancelationEvents[i]);
        switch (cancelationEventEntry->type) {
            case EventEntry::Type::KEY: {
                logOutboundKeyDetails("cancel - ",
                                      static_cast<const KeyEntry&>(*cancelationEventEntry));
                break;
            }
            case EventEntry::Type::MOTION: {
                logOutboundMotionDetails("cancel - ",
                                         static_cast<const MotionEntry&>(*cancelationEventEntry));
                break;
            }
            case EventEntry::Type::FOCUS:
            case EventEntry::Type::TOUCH_MODE_CHANGED:
            case EventEntry::Type::POINTER_CAPTURE_CHANGED:
            case EventEntry::Type::DRAG: {
                LOG_ALWAYS_FATAL("Canceling %s events is not supported",
                                 ftl::enum_string(cancelationEventEntry->type).c_str());
                break;
            }
            case EventEntry::Type::CONFIGURATION_CHANGED:
            case EventEntry::Type::DEVICE_RESET:
            case EventEntry::Type::SENSOR: {
                LOG_ALWAYS_FATAL("%s event should not be found inside Connections's queue",
                                 ftl::enum_string(cancelationEventEntry->type).c_str());
                break;
            }
        }
        enqueueDispatchEntryLocked(connection, std::move(cancelationEventEntry), target,
                                   InputTarget::Flags::DISPATCH_AS_IS);
    }
    if (wasEmpty && !connection->outboundQueue.empty()) {
        startDispatchCycleLocked(currentTime, connection);
    }
}
void InputDispatcher::synthesizePointerDownEventsForConnectionLocked(
        const nsecs_t downTime, const sp<Connection>& connection,
        ftl::Flags<InputTarget::Flags> targetFlags) {
    if (connection->status == Connection::Status::BROKEN) {
        return;
    }
    std::vector<std::unique_ptr<EventEntry>> downEvents =
            connection->inputState.synthesizePointerDownEvents(downTime);
    if (downEvents.empty()) {
        return;
    }
    if (DEBUG_OUTBOUND_EVENT_DETAILS) {
        ALOGD("channel '%s' ~ Synthesized %zu down events to ensure consistent event stream.",
              connection->getInputChannelName().c_str(), downEvents.size());
    }
    InputTarget target;
    sp<WindowInfoHandle> windowHandle =
            getWindowHandleLocked(connection->inputChannel->getConnectionToken());
    if (windowHandle != nullptr) {
        const WindowInfo* windowInfo = windowHandle->getInfo();
        target.setDefaultPointerTransform(windowInfo->transform);
        target.globalScaleFactor = windowInfo->globalScaleFactor;
    }
    target.inputChannel = connection->inputChannel;
    target.flags = targetFlags;
    const bool wasEmpty = connection->outboundQueue.empty();
    for (std::unique_ptr<EventEntry>& downEventEntry : downEvents) {
        switch (downEventEntry->type) {
            case EventEntry::Type::MOTION: {
                logOutboundMotionDetails("down - ",
                        static_cast<const MotionEntry&>(*downEventEntry));
                break;
            }
            case EventEntry::Type::KEY:
            case EventEntry::Type::FOCUS:
            case EventEntry::Type::TOUCH_MODE_CHANGED:
            case EventEntry::Type::CONFIGURATION_CHANGED:
            case EventEntry::Type::DEVICE_RESET:
            case EventEntry::Type::POINTER_CAPTURE_CHANGED:
            case EventEntry::Type::SENSOR:
            case EventEntry::Type::DRAG: {
                LOG_ALWAYS_FATAL("%s event should not be found inside Connections's queue",
                                 ftl::enum_string(downEventEntry->type).c_str());
                break;
            }
        }
        enqueueDispatchEntryLocked(connection, std::move(downEventEntry), target,
                                   InputTarget::Flags::DISPATCH_AS_IS);
    }
    if (wasEmpty && !connection->outboundQueue.empty()) {
        startDispatchCycleLocked(downTime, connection);
    }
}
void InputDispatcher::synthesizeCancelationEventsForWindowLocked(
        const sp<WindowInfoHandle>& windowHandle, const CancelationOptions& options) {
    if (windowHandle != nullptr) {
        sp<Connection> wallpaperConnection = getConnectionLocked(windowHandle->getToken());
        if (wallpaperConnection != nullptr) {
            synthesizeCancelationEventsForConnectionLocked(wallpaperConnection, options);
        }
    }
}
std::unique_ptr<MotionEntry> InputDispatcher::splitMotionEvent(
        const MotionEntry& originalMotionEntry, std::bitset<MAX_POINTER_ID + 1> pointerIds,
        nsecs_t splitDownTime) {
    ALOG_ASSERT(pointerIds.any());
    uint32_t splitPointerIndexMap[MAX_POINTERS];
    PointerProperties splitPointerProperties[MAX_POINTERS];
    PointerCoords splitPointerCoords[MAX_POINTERS];
    uint32_t originalPointerCount = originalMotionEntry.pointerCount;
    uint32_t splitPointerCount = 0;
    for (uint32_t originalPointerIndex = 0; originalPointerIndex < originalPointerCount;
         originalPointerIndex++) {
        const PointerProperties& pointerProperties =
                originalMotionEntry.pointerProperties[originalPointerIndex];
        uint32_t pointerId = uint32_t(pointerProperties.id);
        if (pointerIds.test(pointerId)) {
            splitPointerIndexMap[splitPointerCount] = originalPointerIndex;
            splitPointerProperties[splitPointerCount].copyFrom(pointerProperties);
            splitPointerCoords[splitPointerCount].copyFrom(
                    originalMotionEntry.pointerCoords[originalPointerIndex]);
            splitPointerCount += 1;
        }
    }
    if (splitPointerCount != pointerIds.count()) {
        ALOGW("Dropping split motion event because the pointer count is %d but "
              "we expected there to be %zu pointers.  This probably means we received "
              "a broken sequence of pointer ids from the input device: %s",
              splitPointerCount, pointerIds.count(), originalMotionEntry.getDescription().c_str());
        return nullptr;
    }
    int32_t action = originalMotionEntry.action;
    int32_t maskedAction = action & AMOTION_EVENT_ACTION_MASK;
    if (maskedAction == AMOTION_EVENT_ACTION_POINTER_DOWN ||
        maskedAction == AMOTION_EVENT_ACTION_POINTER_UP) {
        int32_t originalPointerIndex = getMotionEventActionPointerIndex(action);
        const PointerProperties& pointerProperties =
                originalMotionEntry.pointerProperties[originalPointerIndex];
        uint32_t pointerId = uint32_t(pointerProperties.id);
        if (pointerIds.test(pointerId)) {
            if (pointerIds.count() == 1) {
                action = maskedAction == AMOTION_EVENT_ACTION_POINTER_DOWN
                        ? AMOTION_EVENT_ACTION_DOWN
                        : (originalMotionEntry.flags & AMOTION_EVENT_FLAG_CANCELED) != 0
                                ? AMOTION_EVENT_ACTION_CANCEL
                                : AMOTION_EVENT_ACTION_UP;
            } else {
                uint32_t splitPointerIndex = 0;
                while (pointerId != uint32_t(splitPointerProperties[splitPointerIndex].id)) {
                    splitPointerIndex += 1;
                }
                action = maskedAction |
                        (splitPointerIndex << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
            }
        } else {
            action = AMOTION_EVENT_ACTION_MOVE;
        }
    }
    if (action == AMOTION_EVENT_ACTION_DOWN) {
        LOG_ALWAYS_FATAL_IF(splitDownTime != originalMotionEntry.eventTime,
                            "Split motion event has mismatching downTime and eventTime for "
                            "ACTION_DOWN, motionEntry=%s, splitDownTime=%" PRId64,
                            originalMotionEntry.getDescription().c_str(), splitDownTime);
    }
    int32_t newId = mIdGenerator.nextId();
    if (ATRACE_ENABLED()) {
        std::string message = StringPrintf("Split MotionEvent(id=0x%" PRIx32
                                           ") to MotionEvent(id=0x%" PRIx32 ").",
                                           originalMotionEntry.id, newId);
        ATRACE_NAME(message.c_str());
    }
    std::unique_ptr<MotionEntry> splitMotionEntry =
            std::make_unique<MotionEntry>(newId, originalMotionEntry.eventTime,
                                          originalMotionEntry.deviceId, originalMotionEntry.source,
                                          originalMotionEntry.displayId,
                                          originalMotionEntry.policyFlags, action,
                                          originalMotionEntry.actionButton,
                                          originalMotionEntry.flags, originalMotionEntry.metaState,
                                          originalMotionEntry.buttonState,
                                          originalMotionEntry.classification,
                                          originalMotionEntry.edgeFlags,
                                          originalMotionEntry.xPrecision,
                                          originalMotionEntry.yPrecision,
                                          originalMotionEntry.xCursorPosition,
                                          originalMotionEntry.yCursorPosition, splitDownTime,
                                          splitPointerCount, splitPointerProperties,
                                          splitPointerCoords);
    if (originalMotionEntry.injectionState) {
        splitMotionEntry->injectionState = originalMotionEntry.injectionState;
        splitMotionEntry->injectionState->refCount += 1;
    }
    return splitMotionEntry;
}
void InputDispatcher::notifyConfigurationChanged(const NotifyConfigurationChangedArgs* args) {
    if (debugInboundEventDetails()) {
        ALOGD("notifyConfigurationChanged - eventTime=%" PRId64, args->eventTime);
    }
    bool needWake = false;
    {
        std::scoped_lock _l(mLock);
        std::unique_ptr<ConfigurationChangedEntry> newEntry =
                std::make_unique<ConfigurationChangedEntry>(args->id, args->eventTime);
        needWake = enqueueInboundEventLocked(std::move(newEntry));
    }
    if (needWake) {
        mLooper->wake();
    }
}
void InputDispatcher::accelerateMetaShortcuts(const int32_t deviceId, const int32_t action,
                                              int32_t& keyCode, int32_t& metaState) {
    if (metaState & AMETA_META_ON && action == AKEY_EVENT_ACTION_DOWN) {
        int32_t newKeyCode = AKEYCODE_UNKNOWN;
        if (keyCode == AKEYCODE_DEL || keyCode == AKEYCODE_GRAVE || keyCode == AKEYCODE_DPAD_LEFT) {
            newKeyCode = AKEYCODE_BACK;
        }
        if (newKeyCode != AKEYCODE_UNKNOWN) {
            std::scoped_lock _l(mLock);
            struct KeyReplacement replacement = {keyCode, deviceId};
            mReplacedKeys[replacement] = newKeyCode;
            keyCode = newKeyCode;
            metaState &= ~(AMETA_META_ON | AMETA_META_LEFT_ON | AMETA_META_RIGHT_ON);
        }
    } else if (action == AKEY_EVENT_ACTION_UP) {
        std::scoped_lock _l(mLock);
        struct KeyReplacement replacement = {keyCode, deviceId};
        auto replacementIt = mReplacedKeys.find(replacement);
        if (replacementIt != mReplacedKeys.end()) {
            keyCode = replacementIt->second;
            mReplacedKeys.erase(replacementIt);
            metaState &= ~(AMETA_META_ON | AMETA_META_LEFT_ON | AMETA_META_RIGHT_ON);
        }
    }
}
void InputDispatcher::notifyKey(const NotifyKeyArgs* args) {
    ALOGD_IF(debugInboundEventDetails(),
             "notifyKey - id=%" PRIx32 ", eventTime=%" PRId64
             ", deviceId=%d, source=%s, displayId=%" PRId32
             "policyFlags=0x%x, action=%s, flags=0x%x, keyCode=%s, scanCode=0x%x, metaState=0x%x, "
             "downTime=%" PRId64,
             args->id, args->eventTime, args->deviceId,
             inputEventSourceToString(args->source).c_str(), args->displayId, args->policyFlags,
             KeyEvent::actionToString(args->action), args->flags, KeyEvent::getLabel(args->keyCode),
             args->scanCode, args->metaState, args->downTime);
    if (!validateKeyEvent(args->action)) {
        return;
    }
    uint32_t policyFlags = args->policyFlags;
    int32_t flags = args->flags;
    int32_t metaState = args->metaState;
    constexpr int32_t repeatCount = 0;
    if ((policyFlags & POLICY_FLAG_VIRTUAL) || (flags & AKEY_EVENT_FLAG_VIRTUAL_HARD_KEY)) {
        policyFlags |= POLICY_FLAG_VIRTUAL;
        flags |= AKEY_EVENT_FLAG_VIRTUAL_HARD_KEY;
    }
    if (policyFlags & POLICY_FLAG_FUNCTION) {
        metaState |= AMETA_FUNCTION_ON;
    }
    policyFlags |= POLICY_FLAG_TRUSTED;
    int32_t keyCode = args->keyCode;
    accelerateMetaShortcuts(args->deviceId, args->action, keyCode, metaState);
    KeyEvent event;
    event.initialize(args->id, args->deviceId, args->source, args->displayId, INVALID_HMAC,
                     args->action, flags, keyCode, args->scanCode, metaState, repeatCount,
                     args->downTime, args->eventTime);
    android::base::Timer t;
    mPolicy->interceptKeyBeforeQueueing(&event, policyFlags);
    if (t.duration() > SLOW_INTERCEPTION_THRESHOLD) {
        ALOGW("Excessive delay in interceptKeyBeforeQueueing; took %s ms",
              std::to_string(t.duration().count()).c_str());
    }
    bool needWake = false;
    {
        mLock.lock();
        if (shouldSendKeyToInputFilterLocked(args)) {
            mLock.unlock();
            policyFlags |= POLICY_FLAG_FILTERED;
            if (!mPolicy->filterInputEvent(&event, policyFlags)) {
                return;
            }
            mLock.lock();
        }
        std::unique_ptr<KeyEntry> newEntry =
                std::make_unique<KeyEntry>(args->id, args->eventTime, args->deviceId, args->source,
                                           args->displayId, policyFlags, args->action, flags,
                                           keyCode, args->scanCode, metaState, repeatCount,
                                           args->downTime);
        needWake = enqueueInboundEventLocked(std::move(newEntry));
        mLock.unlock();
    }
    if (needWake) {
        mLooper->wake();
    }
}
bool InputDispatcher::shouldSendKeyToInputFilterLocked(const NotifyKeyArgs* args) {
    return mInputFilterEnabled;
}
void InputDispatcher::notifyMotion(const NotifyMotionArgs* args) {
    if (debugInboundEventDetails()) {
        ALOGD("notifyMotion - id=%" PRIx32 " eventTime=%" PRId64 ", deviceId=%d, source=%s, "
              "displayId=%" PRId32 ", policyFlags=0x%x, "
              "action=%s, actionButton=0x%x, flags=0x%x, metaState=0x%x, buttonState=0x%x, "
              "edgeFlags=0x%x, xPrecision=%f, yPrecision=%f, xCursorPosition=%f, "
              "yCursorPosition=%f, downTime=%" PRId64,
              args->id, args->eventTime, args->deviceId,
              inputEventSourceToString(args->source).c_str(), args->displayId, args->policyFlags,
              MotionEvent::actionToString(args->action).c_str(), args->actionButton, args->flags,
              args->metaState, args->buttonState, args->edgeFlags, args->xPrecision,
              args->yPrecision, args->xCursorPosition, args->yCursorPosition, args->downTime);
        for (uint32_t i = 0; i < args->pointerCount; i++) {
            ALOGD("  Pointer %d: id=%d, toolType=%s, x=%f, y=%f, pressure=%f, size=%f, "
                  "touchMajor=%f, touchMinor=%f, toolMajor=%f, toolMinor=%f, orientation=%f",
                  i, args->pointerProperties[i].id,
                  motionToolTypeToString(args->pointerProperties[i].toolType),
                  args->pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_X),
                  args->pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_Y),
                  args->pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_PRESSURE),
                  args->pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_SIZE),
                  args->pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_TOUCH_MAJOR),
                  args->pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_TOUCH_MINOR),
                  args->pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_TOOL_MAJOR),
                  args->pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_TOOL_MINOR),
                  args->pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_ORIENTATION));
        }
    }
    if (!validateMotionEvent(args->action, args->actionButton, args->pointerCount,
                             args->pointerProperties)) {
        LOG(ERROR) << "Invalid event: " << args->dump();
        return;
    }
    uint32_t policyFlags = args->policyFlags;
    policyFlags |= POLICY_FLAG_TRUSTED;
    android::base::Timer t;
    mPolicy->interceptMotionBeforeQueueing(args->displayId, args->eventTime, policyFlags);
    if (t.duration() > SLOW_INTERCEPTION_THRESHOLD) {
        ALOGW("Excessive delay in interceptMotionBeforeQueueing; took %s ms",
              std::to_string(t.duration().count()).c_str());
    }
    bool needWake = false;
    {
        mLock.lock();
        if (!(policyFlags & POLICY_FLAG_PASS_TO_USER)) {
            const auto touchStateIt = mTouchStatesByDisplay.find(args->displayId);
            if (touchStateIt != mTouchStatesByDisplay.end()) {
                const TouchState& touchState = touchStateIt->second;
                if (touchState.deviceId == args->deviceId && touchState.isDown()) {
                    policyFlags |= POLICY_FLAG_PASS_TO_USER;
                }
            }
        }
        if (shouldSendMotionToInputFilterLocked(args)) {
            ui::Transform displayTransform;
            if (const auto it = mDisplayInfos.find(args->displayId); it != mDisplayInfos.end()) {
                displayTransform = it->second.transform;
            }
            mLock.unlock();
            MotionEvent event;
            event.initialize(args->id, args->deviceId, args->source, args->displayId, INVALID_HMAC,
                             args->action, args->actionButton, args->flags, args->edgeFlags,
                             args->metaState, args->buttonState, args->classification,
                             displayTransform, args->xPrecision, args->yPrecision,
                             args->xCursorPosition, args->yCursorPosition, displayTransform,
                             args->downTime, args->eventTime, args->pointerCount,
                             args->pointerProperties, args->pointerCoords);
            policyFlags |= POLICY_FLAG_FILTERED;
            if (!mPolicy->filterInputEvent(&event, policyFlags)) {
                return;
            }
            mLock.lock();
        }
        std::unique_ptr<MotionEntry> newEntry =
                std::make_unique<MotionEntry>(args->id, args->eventTime, args->deviceId,
                                              args->source, args->displayId, policyFlags,
                                              args->action, args->actionButton, args->flags,
                                              args->metaState, args->buttonState,
                                              args->classification, args->edgeFlags,
                                              args->xPrecision, args->yPrecision,
                                              args->xCursorPosition, args->yCursorPosition,
                                              args->downTime, args->pointerCount,
                                              args->pointerProperties, args->pointerCoords);
        if (args->id != android::os::IInputConstants::INVALID_INPUT_EVENT_ID &&
            IdGenerator::getSource(args->id) == IdGenerator::Source::INPUT_READER &&
            !mInputFilterEnabled) {
            const bool isDown = args->action == AMOTION_EVENT_ACTION_DOWN;
            mLatencyTracker.trackListener(args->id, isDown, args->eventTime, args->readTime);
        }
        needWake = enqueueInboundEventLocked(std::move(newEntry));
        mLock.unlock();
    }
    if (needWake) {
        mLooper->wake();
    }
}
void InputDispatcher::notifySensor(const NotifySensorArgs* args) {
    if (debugInboundEventDetails()) {
        ALOGD("notifySensor - id=%" PRIx32 " eventTime=%" PRId64 ", deviceId=%d, source=0x%x, "
              " sensorType=%s",
              args->id, args->eventTime, args->deviceId, args->source,
              ftl::enum_string(args->sensorType).c_str());
    }
    bool needWake = false;
    {
        mLock.lock();
        std::unique_ptr<SensorEntry> newEntry =
                std::make_unique<SensorEntry>(args->id, args->eventTime, args->deviceId,
                                              args->source, 0, args->hwTimestamp,
                                              args->sensorType, args->accuracy,
                                              args->accuracyChanged, args->values);
        needWake = enqueueInboundEventLocked(std::move(newEntry));
        mLock.unlock();
    }
    if (needWake) {
        mLooper->wake();
    }
}
void InputDispatcher::notifyVibratorState(const NotifyVibratorStateArgs* args) {
    if (debugInboundEventDetails()) {
        ALOGD("notifyVibratorState - eventTime=%" PRId64 ", device=%d,  isOn=%d", args->eventTime,
              args->deviceId, args->isOn);
    }
    mPolicy->notifyVibratorState(args->deviceId, args->isOn);
}
bool InputDispatcher::shouldSendMotionToInputFilterLocked(const NotifyMotionArgs* args) {
    return mInputFilterEnabled;
}
void InputDispatcher::notifySwitch(const NotifySwitchArgs* args) {
    if (debugInboundEventDetails()) {
        ALOGD("notifySwitch - eventTime=%" PRId64 ", policyFlags=0x%x, switchValues=0x%08x, "
              "switchMask=0x%08x",
              args->eventTime, args->policyFlags, args->switchValues, args->switchMask);
    }
    uint32_t policyFlags = args->policyFlags;
    policyFlags |= POLICY_FLAG_TRUSTED;
    mPolicy->notifySwitch(args->eventTime, args->switchValues, args->switchMask, policyFlags);
}
void InputDispatcher::notifyDeviceReset(const NotifyDeviceResetArgs* args) {
    if (debugInboundEventDetails()) {
        ALOGD("notifyDeviceReset - eventTime=%" PRId64 ", deviceId=%d", args->eventTime,
              args->deviceId);
    }
    bool needWake = false;
    {
        std::scoped_lock _l(mLock);
        std::unique_ptr<DeviceResetEntry> newEntry =
                std::make_unique<DeviceResetEntry>(args->id, args->eventTime, args->deviceId);
        needWake = enqueueInboundEventLocked(std::move(newEntry));
    }
    if (needWake) {
        mLooper->wake();
    }
}
void InputDispatcher::notifyPointerCaptureChanged(const NotifyPointerCaptureChangedArgs* args) {
    if (debugInboundEventDetails()) {
        ALOGD("notifyPointerCaptureChanged - eventTime=%" PRId64 ", enabled=%s", args->eventTime,
              args->request.enable ? "true" : "false");
    }
    bool needWake = false;
    {
        std::scoped_lock _l(mLock);
        auto entry = std::make_unique<PointerCaptureChangedEntry>(args->id, args->eventTime,
                                                                  args->request);
        needWake = enqueueInboundEventLocked(std::move(entry));
    }
    if (needWake) {
        mLooper->wake();
    }
}
InputEventInjectionResult InputDispatcher::injectInputEvent(const InputEvent* event,
                                                            std::optional<int32_t> targetUid,
                                                            InputEventInjectionSync syncMode,
                                                            std::chrono::milliseconds timeout,
                                                            uint32_t policyFlags) {
    if (debugInboundEventDetails()) {
        ALOGD("injectInputEvent - eventType=%d, targetUid=%s, syncMode=%d, timeout=%lld, "
              "policyFlags=0x%08x",
              event->getType(), targetUid ? std::to_string(*targetUid).c_str() : "none", syncMode,
              timeout.count(), policyFlags);
    }
    nsecs_t endTime = now() + std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();
    policyFlags |= POLICY_FLAG_INJECTED | POLICY_FLAG_TRUSTED;
    int32_t resolvedDeviceId = VIRTUAL_KEYBOARD_ID;
    if (policyFlags & POLICY_FLAG_FILTERED) {
        resolvedDeviceId = event->getDeviceId();
    }
    std::queue<std::unique_ptr<EventEntry>> injectedEntries;
    switch (event->getType()) {
        case AINPUT_EVENT_TYPE_KEY: {
            const KeyEvent& incomingKey = static_cast<const KeyEvent&>(*event);
            int32_t action = incomingKey.getAction();
            if (!validateKeyEvent(action)) {
                return InputEventInjectionResult::FAILED;
            }
            int32_t flags = incomingKey.getFlags();
            if (policyFlags & POLICY_FLAG_INJECTED_FROM_ACCESSIBILITY) {
                flags |= AKEY_EVENT_FLAG_IS_ACCESSIBILITY_EVENT;
            }
            int32_t keyCode = incomingKey.getKeyCode();
            int32_t metaState = incomingKey.getMetaState();
            accelerateMetaShortcuts(resolvedDeviceId, action,
                                              keyCode, metaState);
            KeyEvent keyEvent;
            keyEvent.initialize(incomingKey.getId(), resolvedDeviceId, incomingKey.getSource(),
                                incomingKey.getDisplayId(), INVALID_HMAC, action, flags, keyCode,
                                incomingKey.getScanCode(), metaState, incomingKey.getRepeatCount(),
                                incomingKey.getDownTime(), incomingKey.getEventTime());
            if (flags & AKEY_EVENT_FLAG_VIRTUAL_HARD_KEY) {
                policyFlags |= POLICY_FLAG_VIRTUAL;
            }
            if (!(policyFlags & POLICY_FLAG_FILTERED)) {
                android::base::Timer t;
                mPolicy->interceptKeyBeforeQueueing(&keyEvent, policyFlags);
                if (t.duration() > SLOW_INTERCEPTION_THRESHOLD) {
                    ALOGW("Excessive delay in interceptKeyBeforeQueueing; took %s ms",
                          std::to_string(t.duration().count()).c_str());
                }
            }
            mLock.lock();
            std::unique_ptr<KeyEntry> injectedEntry =
                    std::make_unique<KeyEntry>(incomingKey.getId(), incomingKey.getEventTime(),
                                               resolvedDeviceId, incomingKey.getSource(),
                                               incomingKey.getDisplayId(), policyFlags, action,
                                               flags, keyCode, incomingKey.getScanCode(), metaState,
                                               incomingKey.getRepeatCount(),
                                               incomingKey.getDownTime());
            injectedEntries.push(std::move(injectedEntry));
            break;
        }
        case AINPUT_EVENT_TYPE_MOTION: {
            const MotionEvent& motionEvent = static_cast<const MotionEvent&>(*event);
            const int32_t action = motionEvent.getAction();
            const bool isPointerEvent =
                    isFromSource(event->getSource(), AINPUT_SOURCE_CLASS_POINTER);
            const uint32_t displayId = isPointerEvent && (event->getDisplayId() == ADISPLAY_ID_NONE)
                    ? ADISPLAY_ID_DEFAULT
                    : event->getDisplayId();
            const size_t pointerCount = motionEvent.getPointerCount();
            const PointerProperties* pointerProperties = motionEvent.getPointerProperties();
            const int32_t actionButton = motionEvent.getActionButton();
            int32_t flags = motionEvent.getFlags();
            if (!validateMotionEvent(action, actionButton, pointerCount, pointerProperties)) {
                return InputEventInjectionResult::FAILED;
            }
            if (!(policyFlags & POLICY_FLAG_FILTERED)) {
                nsecs_t eventTime = motionEvent.getEventTime();
                android::base::Timer t;
                mPolicy->interceptMotionBeforeQueueing(displayId, eventTime, policyFlags);
                if (t.duration() > SLOW_INTERCEPTION_THRESHOLD) {
                    ALOGW("Excessive delay in interceptMotionBeforeQueueing; took %s ms",
                          std::to_string(t.duration().count()).c_str());
                }
            }
            if (policyFlags & POLICY_FLAG_INJECTED_FROM_ACCESSIBILITY) {
                flags |= AMOTION_EVENT_FLAG_IS_ACCESSIBILITY_EVENT;
            }
            mLock.lock();
            const nsecs_t* sampleEventTimes = motionEvent.getSampleEventTimes();
            const PointerCoords* samplePointerCoords = motionEvent.getSamplePointerCoords();
            std::unique_ptr<MotionEntry> injectedEntry =
                    std::make_unique<MotionEntry>(motionEvent.getId(), *sampleEventTimes,
                                                  resolvedDeviceId, motionEvent.getSource(),
                                                  displayId, policyFlags, action, actionButton,
                                                  flags, motionEvent.getMetaState(),
                                                  motionEvent.getButtonState(),
                                                  motionEvent.getClassification(),
                                                  motionEvent.getEdgeFlags(),
                                                  motionEvent.getXPrecision(),
                                                  motionEvent.getYPrecision(),
                                                  motionEvent.getRawXCursorPosition(),
                                                  motionEvent.getRawYCursorPosition(),
                                                  motionEvent.getDownTime(), uint32_t(pointerCount),
                                                  pointerProperties, samplePointerCoords);
            transformMotionEntryForInjectionLocked(*injectedEntry, motionEvent.getTransform());
            injectedEntries.push(std::move(injectedEntry));
            for (size_t i = motionEvent.getHistorySize(); i > 0; i--) {
                sampleEventTimes += 1;
                samplePointerCoords += pointerCount;
                std::unique_ptr<MotionEntry> nextInjectedEntry =
                        std::make_unique<MotionEntry>(motionEvent.getId(), *sampleEventTimes,
                                                      resolvedDeviceId, motionEvent.getSource(),
                                                      displayId, policyFlags, action, actionButton,
                                                      flags, motionEvent.getMetaState(),
                                                      motionEvent.getButtonState(),
                                                      motionEvent.getClassification(),
                                                      motionEvent.getEdgeFlags(),
                                                      motionEvent.getXPrecision(),
                                                      motionEvent.getYPrecision(),
                                                      motionEvent.getRawXCursorPosition(),
                                                      motionEvent.getRawYCursorPosition(),
                                                      motionEvent.getDownTime(),
                                                      uint32_t(pointerCount), pointerProperties,
                                                      samplePointerCoords);
                transformMotionEntryForInjectionLocked(*nextInjectedEntry,
                                                       motionEvent.getTransform());
                injectedEntries.push(std::move(nextInjectedEntry));
            }
            break;
        }
        default:
            ALOGW("Cannot inject %s events", inputEventTypeToString(event->getType()));
            return InputEventInjectionResult::FAILED;
    }
    InjectionState* injectionState = new InjectionState(targetUid);
    if (syncMode == InputEventInjectionSync::NONE) {
        injectionState->injectionIsAsync = true;
    }
    injectionState->refCount += 1;
    injectedEntries.back()->injectionState = injectionState;
    bool needWake = false;
    while (!injectedEntries.empty()) {
        if (DEBUG_INJECTION) {
            LOG(DEBUG) << "Injecting " << injectedEntries.front()->getDescription();
        }
        needWake |= enqueueInboundEventLocked(std::move(injectedEntries.front()));
        injectedEntries.pop();
    }
    mLock.unlock();
    if (needWake) {
        mLooper->wake();
    }
    InputEventInjectionResult injectionResult;
    {
        std::unique_lock _l(mLock);
        if (syncMode == InputEventInjectionSync::NONE) {
            injectionResult = InputEventInjectionResult::SUCCEEDED;
        } else {
            for (;;) {
                injectionResult = injectionState->injectionResult;
                if (injectionResult != InputEventInjectionResult::PENDING) {
                    break;
                }
                nsecs_t remainingTimeout = endTime - now();
                if (remainingTimeout <= 0) {
                    if (DEBUG_INJECTION) {
                        ALOGD("injectInputEvent - Timed out waiting for injection result "
                              "to become available.");
                    }
                    injectionResult = InputEventInjectionResult::TIMED_OUT;
                    break;
                }
                mInjectionResultAvailable.wait_for(_l, std::chrono::nanoseconds(remainingTimeout));
            }
            if (injectionResult == InputEventInjectionResult::SUCCEEDED &&
                syncMode == InputEventInjectionSync::WAIT_FOR_FINISHED) {
                while (injectionState->pendingForegroundDispatches != 0) {
                    if (DEBUG_INJECTION) {
                        ALOGD("injectInputEvent - Waiting for %d pending foreground dispatches.",
                              injectionState->pendingForegroundDispatches);
                    }
                    nsecs_t remainingTimeout = endTime - now();
                    if (remainingTimeout <= 0) {
                        if (DEBUG_INJECTION) {
                            ALOGD("injectInputEvent - Timed out waiting for pending foreground "
                                  "dispatches to finish.");
                        }
                        injectionResult = InputEventInjectionResult::TIMED_OUT;
                        break;
                    }
                    mInjectionSyncFinished.wait_for(_l, std::chrono::nanoseconds(remainingTimeout));
                }
            }
        }
        injectionState->release();
    }
    if (DEBUG_INJECTION) {
        LOG(DEBUG) << "injectInputEvent - Finished with result "
                   << ftl::enum_string(injectionResult);
    }
    return injectionResult;
}
std::unique_ptr<VerifiedInputEvent> InputDispatcher::verifyInputEvent(const InputEvent& event) {
    std::array<uint8_t, 32> calculatedHmac;
    std::unique_ptr<VerifiedInputEvent> result;
    switch (event.getType()) {
        case AINPUT_EVENT_TYPE_KEY: {
            const KeyEvent& keyEvent = static_cast<const KeyEvent&>(event);
            VerifiedKeyEvent verifiedKeyEvent = verifiedKeyEventFromKeyEvent(keyEvent);
            result = std::make_unique<VerifiedKeyEvent>(verifiedKeyEvent);
            calculatedHmac = sign(verifiedKeyEvent);
            break;
        }
        case AINPUT_EVENT_TYPE_MOTION: {
            const MotionEvent& motionEvent = static_cast<const MotionEvent&>(event);
            VerifiedMotionEvent verifiedMotionEvent =
                    verifiedMotionEventFromMotionEvent(motionEvent);
            result = std::make_unique<VerifiedMotionEvent>(verifiedMotionEvent);
            calculatedHmac = sign(verifiedMotionEvent);
            break;
        }
        default: {
            ALOGE("Cannot verify events of type %" PRId32, event.getType());
            return nullptr;
        }
    }
    if (calculatedHmac == INVALID_HMAC) {
        return nullptr;
    }
    if (calculatedHmac != event.getHmac()) {
        return nullptr;
    }
    return result;
}
void InputDispatcher::setInjectionResult(EventEntry& entry,
                                         InputEventInjectionResult injectionResult) {
    InjectionState* injectionState = entry.injectionState;
    if (injectionState) {
        if (DEBUG_INJECTION) {
            LOG(DEBUG) << "Setting input event injection result to "
                       << ftl::enum_string(injectionResult);
        }
        if (injectionState->injectionIsAsync && !(entry.policyFlags & POLICY_FLAG_FILTERED)) {
            switch (injectionResult) {
                case InputEventInjectionResult::SUCCEEDED:
                    ALOGV("Asynchronous input event injection succeeded.");
                    break;
                case InputEventInjectionResult::TARGET_MISMATCH:
                    ALOGV("Asynchronous input event injection target mismatch.");
                    break;
                case InputEventInjectionResult::FAILED:
                    ALOGW("Asynchronous input event injection failed.");
                    break;
                case InputEventInjectionResult::TIMED_OUT:
                    ALOGW("Asynchronous input event injection timed out.");
                    break;
                case InputEventInjectionResult::PENDING:
                    ALOGE("Setting result to 'PENDING' for asynchronous injection");
                    break;
            }
        }
        injectionState->injectionResult = injectionResult;
        mInjectionResultAvailable.notify_all();
    }
}
void InputDispatcher::transformMotionEntryForInjectionLocked(
        MotionEntry& entry, const ui::Transform& injectedTransform) const {
    const auto it = mDisplayInfos.find(entry.displayId);
    if (it == mDisplayInfos.end()) return;
    const auto& transformToDisplay = it->second.transform.inverse() * injectedTransform;
    if (entry.xCursorPosition != AMOTION_EVENT_INVALID_CURSOR_POSITION &&
        entry.yCursorPosition != AMOTION_EVENT_INVALID_CURSOR_POSITION) {
        const vec2 cursor =
                MotionEvent::calculateTransformedXY(entry.source, transformToDisplay,
                                                    {entry.xCursorPosition, entry.yCursorPosition});
        entry.xCursorPosition = cursor.x;
        entry.yCursorPosition = cursor.y;
    }
    for (uint32_t i = 0; i < entry.pointerCount; i++) {
        entry.pointerCoords[i] =
                MotionEvent::calculateTransformedCoords(entry.source, transformToDisplay,
                                                        entry.pointerCoords[i]);
    }
}
void InputDispatcher::incrementPendingForegroundDispatches(EventEntry& entry) {
    InjectionState* injectionState = entry.injectionState;
    if (injectionState) {
        injectionState->pendingForegroundDispatches += 1;
    }
}
void InputDispatcher::decrementPendingForegroundDispatches(EventEntry& entry) {
    InjectionState* injectionState = entry.injectionState;
    if (injectionState) {
        injectionState->pendingForegroundDispatches -= 1;
        if (injectionState->pendingForegroundDispatches == 0) {
            mInjectionSyncFinished.notify_all();
        }
    }
}
const std::vector<sp<WindowInfoHandle>>& InputDispatcher::getWindowHandlesLocked(
        int32_t displayId) const {
    static const std::vector<sp<WindowInfoHandle>> EMPTY_WINDOW_HANDLES;
    auto it = mWindowHandlesByDisplay.find(displayId);
    return it != mWindowHandlesByDisplay.end() ? it->second : EMPTY_WINDOW_HANDLES;
}
sp<WindowInfoHandle> InputDispatcher::getWindowHandleLocked(
        const sp<IBinder>& windowHandleToken) const {
    if (windowHandleToken == nullptr) {
        return nullptr;
    }
    for (auto& it : mWindowHandlesByDisplay) {
        const std::vector<sp<WindowInfoHandle>>& windowHandles = it.second;
        for (const sp<WindowInfoHandle>& windowHandle : windowHandles) {
            if (windowHandle->getToken() == windowHandleToken) {
                return windowHandle;
            }
        }
    }
    return nullptr;
}
sp<WindowInfoHandle> InputDispatcher::getWindowHandleLocked(const sp<IBinder>& windowHandleToken,
                                                            int displayId) const {
    if (windowHandleToken == nullptr) {
        return nullptr;
    }
    for (const sp<WindowInfoHandle>& windowHandle : getWindowHandlesLocked(displayId)) {
        if (windowHandle->getToken() == windowHandleToken) {
            return windowHandle;
        }
    }
    return nullptr;
}
sp<WindowInfoHandle> InputDispatcher::getWindowHandleLocked(
        const sp<WindowInfoHandle>& windowHandle) const {
    for (auto& it : mWindowHandlesByDisplay) {
        const std::vector<sp<WindowInfoHandle>>& windowHandles = it.second;
        for (const sp<WindowInfoHandle>& handle : windowHandles) {
            if (handle->getId() == windowHandle->getId() &&
                handle->getToken() == windowHandle->getToken()) {
                if (windowHandle->getInfo()->displayId != it.first) {
                    ALOGE("Found window %s in display %" PRId32
                          ", but it should belong to display %" PRId32,
                          windowHandle->getName().c_str(), it.first,
                          windowHandle->getInfo()->displayId);
                }
                return handle;
            }
        }
    }
    return nullptr;
}
sp<WindowInfoHandle> InputDispatcher::getFocusedWindowHandleLocked(int displayId) const {
    sp<IBinder> focusedToken = mFocusResolver.getFocusedWindowToken(displayId);
    return getWindowHandleLocked(focusedToken, displayId);
}
ui::Transform InputDispatcher::getTransformLocked(int32_t displayId) const {
    auto displayInfoIt = mDisplayInfos.find(displayId);
    return displayInfoIt != mDisplayInfos.end() ? displayInfoIt->second.transform
                                                : kIdentityTransform;
}
bool InputDispatcher::canWindowReceiveMotionLocked(const sp<WindowInfoHandle>& window,
                                                   const MotionEntry& motionEntry) const {
    const WindowInfo& info = *window->getInfo();
    if (const auto err = verifyTargetedInjection(window, motionEntry); err) {
        return false;
    }
    if (info.inputConfig.test(WindowInfo::InputConfig::PAUSE_DISPATCHING)) {
        ALOGI("Not sending touch event to %s because it is paused", window->getName().c_str());
        return false;
    }
    if (info.inputConfig.test(WindowInfo::InputConfig::NO_INPUT_CHANNEL)) {
        ALOGW("Not sending touch gesture to %s because it has config NO_INPUT_CHANNEL",
              window->getName().c_str());
        return false;
    }
    sp<Connection> connection = getConnectionLocked(window->getToken());
    if (connection == nullptr) {
        ALOGW("Not sending touch to %s because there's no corresponding connection",
              window->getName().c_str());
        return false;
    }
    if (!connection->responsive) {
        ALOGW("Not sending touch to %s because it is not responsive", window->getName().c_str());
        return false;
    }
    const auto [x, y] = resolveTouchedPosition(motionEntry);
    TouchOcclusionInfo occlusionInfo = computeTouchOcclusionInfoLocked(window, x, y);
    if (!isTouchTrustedLocked(occlusionInfo)) {
        if (DEBUG_TOUCH_OCCLUSION) {
            ALOGD("Stack of obscuring windows during untrusted touch (%.1f, %.1f):", x, y);
            for (const auto& log : occlusionInfo.debugInfo) {
                ALOGD("%s", log.c_str());
            }
        }
        ALOGW("Dropping untrusted touch event due to %s/%d", occlusionInfo.obscuringPackage.c_str(),
              occlusionInfo.obscuringUid);
        return false;
    }
    if (shouldDropInput(motionEntry, window)) {
        return false;
    }
    return true;
}
std::shared_ptr<InputChannel> InputDispatcher::getInputChannelLocked(
        const sp<IBinder>& token) const {
    auto connectionIt = mConnectionsByToken.find(token);
    if (connectionIt == mConnectionsByToken.end()) {
        return nullptr;
    }
    return connectionIt->second->inputChannel;
}
void InputDispatcher::updateWindowHandlesForDisplayLocked(
        const std::vector<sp<WindowInfoHandle>>& windowInfoHandles, int32_t displayId) {
    if (windowInfoHandles.empty()) {
        mWindowHandlesByDisplay.erase(displayId);
        return;
    }
    const std::vector<sp<WindowInfoHandle>>& oldHandles = getWindowHandlesLocked(displayId);
    std::unordered_map<int32_t , sp<WindowInfoHandle>> oldHandlesById;
    for (const sp<WindowInfoHandle>& handle : oldHandles) {
        oldHandlesById[handle->getId()] = handle;
    }
    std::vector<sp<WindowInfoHandle>> newHandles;
    for (const sp<WindowInfoHandle>& handle : windowInfoHandles) {
        const WindowInfo* info = handle->getInfo();
        if (getInputChannelLocked(handle->getToken()) == nullptr) {
            const bool noInputChannel =
                    info->inputConfig.test(WindowInfo::InputConfig::NO_INPUT_CHANNEL);
            const bool canReceiveInput =
                    !info->inputConfig.test(WindowInfo::InputConfig::NOT_TOUCHABLE) ||
                    !info->inputConfig.test(WindowInfo::InputConfig::NOT_FOCUSABLE);
            if (canReceiveInput && !noInputChannel) {
                ALOGV("Window handle %s has no registered input channel",
                      handle->getName().c_str());
                continue;
            }
        }
        if (info->displayId != displayId) {
            ALOGE("Window %s updated by wrong display %d, should belong to display %d",
                  handle->getName().c_str(), displayId, info->displayId);
            continue;
        }
        if ((oldHandlesById.find(handle->getId()) != oldHandlesById.end()) &&
                (oldHandlesById.at(handle->getId())->getToken() == handle->getToken())) {
            const sp<WindowInfoHandle>& oldHandle = oldHandlesById.at(handle->getId());
            oldHandle->updateFrom(handle);
            newHandles.push_back(oldHandle);
        } else {
            newHandles.push_back(handle);
        }
    }
    mWindowHandlesByDisplay[displayId] = newHandles;
}
void InputDispatcher::setInputWindows(
        const std::unordered_map<int32_t, std::vector<sp<WindowInfoHandle>>>& handlesPerDisplay) {
    {
        std::scoped_lock _l(mLock);
        for (const auto& [displayId, handles] : handlesPerDisplay) {
            setInputWindowsLocked(handles, displayId);
        }
    }
    mLooper->wake();
}
void InputDispatcher::setInputWindowsLocked(
        const std::vector<sp<WindowInfoHandle>>& windowInfoHandles, int32_t displayId) {
    if (DEBUG_FOCUS) {
        std::string windowList;
        for (const sp<WindowInfoHandle>& iwh : windowInfoHandles) {
            windowList += iwh->getName() + " ";
        }
        ALOGD("setInputWindows displayId=%" PRId32 " %s", displayId, windowList.c_str());
    }
    for (const sp<WindowInfoHandle>& window : windowInfoHandles) {
        const WindowInfo& info = *window->getInfo();
        const bool noInputWindow = info.inputConfig.test(WindowInfo::InputConfig::NO_INPUT_CHANNEL);
        if (noInputWindow && window->getToken() != nullptr) {
            ALOGE("%s has feature NO_INPUT_WINDOW, but a non-null token. Clearing",
                  window->getName().c_str());
            window->releaseChannel();
        }
        LOG_ALWAYS_FATAL_IF(info.isSpy() &&
                                    !info.inputConfig.test(
                                            WindowInfo::InputConfig::TRUSTED_OVERLAY),
                            "%s has feature SPY, but is not a trusted overlay.",
                            window->getName().c_str());
        LOG_ALWAYS_FATAL_IF(info.interceptsStylus() &&
                                    !info.inputConfig.test(
                                            WindowInfo::InputConfig::TRUSTED_OVERLAY),
                            "%s has feature INTERCEPTS_STYLUS, but is not a trusted overlay.",
                            window->getName().c_str());
    }
    const std::vector<sp<WindowInfoHandle>> oldWindowHandles = getWindowHandlesLocked(displayId);
    std::unordered_map<int32_t, uint32_t> oldWindowOrientations;
    for (const sp<WindowInfoHandle>& handle : oldWindowHandles) {
        oldWindowOrientations.emplace(handle->getId(),
                                      handle->getInfo()->transform.getOrientation());
    }
    updateWindowHandlesForDisplayLocked(windowInfoHandles, displayId);
    const std::vector<sp<WindowInfoHandle>>& windowHandles = getWindowHandlesLocked(displayId);
    std::optional<FocusResolver::FocusChanges> changes =
            mFocusResolver.setInputWindows(displayId, windowHandles);
    if (changes) {
        onFocusChangedLocked(*changes);
    }
    std::unordered_map<int32_t, TouchState>::iterator stateIt =
            mTouchStatesByDisplay.find(displayId);
    if (stateIt != mTouchStatesByDisplay.end()) {
        TouchState& state = stateIt->second;
        for (size_t i = 0; i < state.windows.size();) {
            TouchedWindow& touchedWindow = state.windows[i];
            if (getWindowHandleLocked(touchedWindow.windowHandle) == nullptr) {
                if (DEBUG_FOCUS) {
                    ALOGD("Touched window was removed: %s in display %" PRId32,
                          touchedWindow.windowHandle->getName().c_str(), displayId);
                }
                std::shared_ptr<InputChannel> touchedInputChannel =
                        getInputChannelLocked(touchedWindow.windowHandle->getToken());
                if (touchedInputChannel != nullptr) {
                    CancelationOptions options(CancelationOptions::Mode::CANCEL_POINTER_EVENTS,
                                               "touched window was removed");
                    synthesizeCancelationEventsForInputChannelLocked(touchedInputChannel, options);
                    if (touchedWindow.targetFlags.test(InputTarget::Flags::FOREGROUND) &&
                        touchedWindow.windowHandle->getInfo()->inputConfig.test(
                                gui::WindowInfo::InputConfig::DUPLICATE_TOUCH_TO_WALLPAPER)) {
                        sp<WindowInfoHandle> wallpaper = state.getWallpaperWindow();
                        synthesizeCancelationEventsForWindowLocked(wallpaper, options);
                    }
                }
                state.windows.erase(state.windows.begin() + i);
            } else {
                ++i;
            }
        }
        if (mDragState && mDragState->dragWindow->getInfo()->displayId == displayId &&
            std::find(windowHandles.begin(), windowHandles.end(), mDragState->dragWindow) ==
                    windowHandles.end()) {
            ALOGI("Drag window went away: %s", mDragState->dragWindow->getName().c_str());
            sendDropWindowCommandLocked(nullptr, 0, 0);
            mDragState.reset();
        }
    }
    for (const sp<WindowInfoHandle>& oldWindowHandle : oldWindowHandles) {
        const sp<WindowInfoHandle> newWindowHandle = getWindowHandleLocked(oldWindowHandle);
        if (newWindowHandle != nullptr &&
            newWindowHandle->getInfo()->transform.getOrientation() !=
                    oldWindowOrientations[oldWindowHandle->getId()]) {
            std::shared_ptr<InputChannel> inputChannel =
                    getInputChannelLocked(newWindowHandle->getToken());
            if (inputChannel != nullptr) {
                CancelationOptions options(CancelationOptions::Mode::CANCEL_POINTER_EVENTS,
                                           "touched window's orientation changed");
                synthesizeCancelationEventsForInputChannelLocked(inputChannel, options);
            }
        }
    }
    for (const sp<WindowInfoHandle>& oldWindowHandle : oldWindowHandles) {
        if (getWindowHandleLocked(oldWindowHandle) == nullptr) {
            if (DEBUG_FOCUS) {
                ALOGD("Window went away: %s", oldWindowHandle->getName().c_str());
            }
            oldWindowHandle->releaseChannel();
        }
    }
}
void InputDispatcher::setFocusedApplication(
        int32_t displayId, const std::shared_ptr<InputApplicationHandle>& inputApplicationHandle) {
    if (DEBUG_FOCUS) {
        ALOGD("setFocusedApplication displayId=%" PRId32 " %s", displayId,
              inputApplicationHandle ? inputApplicationHandle->getName().c_str() : "<nullptr>");
    }
    {
        std::scoped_lock _l(mLock);
        setFocusedApplicationLocked(displayId, inputApplicationHandle);
    }
    mLooper->wake();
}
void InputDispatcher::setFocusedApplicationLocked(
        int32_t displayId, const std::shared_ptr<InputApplicationHandle>& inputApplicationHandle) {
    std::shared_ptr<InputApplicationHandle> oldFocusedApplicationHandle =
            getValueByKey(mFocusedApplicationHandlesByDisplay, displayId);
    if (sharedPointersEqual(oldFocusedApplicationHandle, inputApplicationHandle)) {
        return;
    }
    if (inputApplicationHandle != nullptr) {
        mFocusedApplicationHandlesByDisplay[displayId] = inputApplicationHandle;
    } else {
        mFocusedApplicationHandlesByDisplay.erase(displayId);
    }
    resetNoFocusedWindowTimeoutLocked();
}
void InputDispatcher::setFocusedDisplay(int32_t displayId) {
    if (DEBUG_FOCUS) {
        ALOGD("setFocusedDisplay displayId=%" PRId32, displayId);
    }
    {
        std::scoped_lock _l(mLock);
        if (mFocusedDisplayId != displayId) {
            sp<IBinder> oldFocusedWindowToken =
                    mFocusResolver.getFocusedWindowToken(mFocusedDisplayId);
            if (oldFocusedWindowToken != nullptr) {
                std::shared_ptr<InputChannel> inputChannel =
                        getInputChannelLocked(oldFocusedWindowToken);
                if (inputChannel != nullptr) {
                    CancelationOptions
                            options(CancelationOptions::Mode::CANCEL_NON_POINTER_EVENTS,
                                    "The display which contains this window no longer has focus.");
                    options.displayId = ADISPLAY_ID_NONE;
                    synthesizeCancelationEventsForInputChannelLocked(inputChannel, options);
                }
            }
            mFocusedDisplayId = displayId;
            sp<IBinder> newFocusedWindowToken = mFocusResolver.getFocusedWindowToken(displayId);
            sendFocusChangedCommandLocked(oldFocusedWindowToken, newFocusedWindowToken);
            if (newFocusedWindowToken == nullptr) {
                ALOGW("Focused display #%" PRId32 " does not have a focused window.", displayId);
                if (mFocusResolver.hasFocusedWindowTokens()) {
                    ALOGE("But another display has a focused window\n%s",
                          mFocusResolver.dumpFocusedWindows().c_str());
                }
            }
        }
    }
    mLooper->wake();
}
void InputDispatcher::setInputDispatchMode(bool enabled, bool frozen) {
    if (DEBUG_FOCUS) {
        ALOGD("setInputDispatchMode: enabled=%d, frozen=%d", enabled, frozen);
    }
    bool changed;
    {
        std::scoped_lock _l(mLock);
        if (mDispatchEnabled != enabled || mDispatchFrozen != frozen) {
            if (mDispatchFrozen && !frozen) {
                resetNoFocusedWindowTimeoutLocked();
            }
            if (mDispatchEnabled && !enabled) {
                resetAndDropEverythingLocked("dispatcher is being disabled");
            }
            mDispatchEnabled = enabled;
            mDispatchFrozen = frozen;
            changed = true;
        } else {
            changed = false;
        }
    }
    if (changed) {
        mLooper->wake();
    }
}
void InputDispatcher::setInputFilterEnabled(bool enabled) {
    if (DEBUG_FOCUS) {
        ALOGD("setInputFilterEnabled: enabled=%d", enabled);
    }
    {
        std::scoped_lock _l(mLock);
        if (mInputFilterEnabled == enabled) {
            return;
        }
        mInputFilterEnabled = enabled;
        resetAndDropEverythingLocked("input filter is being enabled or disabled");
    }
    mLooper->wake();
}
bool InputDispatcher::setInTouchMode(bool inTouchMode, int32_t pid, int32_t uid, bool hasPermission,
                                     int32_t displayId) {
    bool needWake = false;
    {
        std::scoped_lock lock(mLock);
        ALOGD_IF(DEBUG_TOUCH_MODE,
                 "Request to change touch mode to %s (calling pid=%d, uid=%d, "
                 "hasPermission=%s, target displayId=%d, mTouchModePerDisplay[displayId]=%s)",
                 toString(inTouchMode), pid, uid, toString(hasPermission), displayId,
                 mTouchModePerDisplay.count(displayId) == 0
                         ? "not set"
                         : std::to_string(mTouchModePerDisplay[displayId]).c_str());
        auto touchModeIt = mTouchModePerDisplay.find(displayId);
        if (touchModeIt != mTouchModePerDisplay.end() && touchModeIt->second == inTouchMode) {
            return false;
        }
        if (!hasPermission) {
            if (!focusedWindowIsOwnedByLocked(pid, uid) &&
                !recentWindowsAreOwnedByLocked(pid, uid)) {
                ALOGD("Touch mode switch rejected, caller (pid=%d, uid=%d) doesn't own the focused "
                      "window nor none of the previously interacted window",
                      pid, uid);
                return false;
            }
        }
        mTouchModePerDisplay[displayId] = inTouchMode;
        auto entry = std::make_unique<TouchModeEntry>(mIdGenerator.nextId(), now(), inTouchMode,
                                                      displayId);
        needWake = enqueueInboundEventLocked(std::move(entry));
    }
    if (needWake) {
        mLooper->wake();
    }
    return true;
}
bool InputDispatcher::focusedWindowIsOwnedByLocked(int32_t pid, int32_t uid) {
    const sp<IBinder> focusedToken = mFocusResolver.getFocusedWindowToken(mFocusedDisplayId);
    if (focusedToken == nullptr) {
        return false;
    }
    sp<WindowInfoHandle> windowHandle = getWindowHandleLocked(focusedToken);
    return isWindowOwnedBy(windowHandle, pid, uid);
}
bool InputDispatcher::recentWindowsAreOwnedByLocked(int32_t pid, int32_t uid) {
    return std::find_if(mInteractionConnectionTokens.begin(), mInteractionConnectionTokens.end(),
                        [&](const sp<IBinder>& connectionToken) REQUIRES(mLock) {
                            const sp<WindowInfoHandle> windowHandle =
                                    getWindowHandleLocked(connectionToken);
                            return isWindowOwnedBy(windowHandle, pid, uid);
                        }) != mInteractionConnectionTokens.end();
}
void InputDispatcher::setMaximumObscuringOpacityForTouch(float opacity) {
    if (opacity < 0 || opacity > 1) {
        LOG_ALWAYS_FATAL("Maximum obscuring opacity for touch should be >= 0 and <= 1");
        return;
    }
    std::scoped_lock lock(mLock);
    mMaximumObscuringOpacityForTouch = opacity;
}
std::tuple<TouchState*, TouchedWindow*, int32_t >
InputDispatcher::findTouchStateWindowAndDisplayLocked(const sp<IBinder>& token) {
    for (auto& [displayId, state] : mTouchStatesByDisplay) {
        for (TouchedWindow& w : state.windows) {
            if (w.windowHandle->getToken() == token) {
                return std::make_tuple(&state, &w, displayId);
            }
        }
    }
    return std::make_tuple(nullptr, nullptr, ADISPLAY_ID_DEFAULT);
}
bool InputDispatcher::transferTouchFocus(const sp<IBinder>& fromToken, const sp<IBinder>& toToken,
                                         bool isDragDrop) {
    if (fromToken == toToken) {
        if (DEBUG_FOCUS) {
            ALOGD("Trivial transfer to same window.");
        }
        return true;
    }
    {
        std::scoped_lock _l(mLock);
        auto [state, touchedWindow, displayId] = findTouchStateWindowAndDisplayLocked(fromToken);
        if (state == nullptr || touchedWindow == nullptr) {
            ALOGD("Focus transfer failed because from window is not being touched.");
            return false;
        }
        sp<WindowInfoHandle> toWindowHandle = getWindowHandleLocked(toToken, displayId);
        if (toWindowHandle == nullptr) {
            ALOGW("Cannot transfer focus because to window not found.");
            return false;
        }
        if (DEBUG_FOCUS) {
            ALOGD("transferTouchFocus: fromWindowHandle=%s, toWindowHandle=%s",
                  touchedWindow->windowHandle->getName().c_str(),
                  toWindowHandle->getName().c_str());
        }
        ftl::Flags<InputTarget::Flags> oldTargetFlags = touchedWindow->targetFlags;
        std::bitset<MAX_POINTER_ID + 1> pointerIds = touchedWindow->pointerIds;
        sp<WindowInfoHandle> fromWindowHandle = touchedWindow->windowHandle;
        state->removeWindowByToken(fromToken);
        nsecs_t downTimeInTarget = now();
        ftl::Flags<InputTarget::Flags> newTargetFlags =
                oldTargetFlags & (InputTarget::Flags::SPLIT | InputTarget::Flags::DISPATCH_AS_IS);
        if (canReceiveForegroundTouches(*toWindowHandle->getInfo())) {
            newTargetFlags |= InputTarget::Flags::FOREGROUND;
        }
        state->addOrUpdateWindow(toWindowHandle, newTargetFlags, pointerIds, downTimeInTarget);
        if (isDragDrop) {
            if (pointerIds.count() != 1) {
                ALOGW("The drag and drop cannot be started when there is no pointer or more than 1"
                      " pointer on the window.");
                return false;
            }
            const size_t id = firstMarkedBit(pointerIds);
            mDragState = std::make_unique<DragState>(toWindowHandle, id);
        }
        sp<Connection> fromConnection = getConnectionLocked(fromToken);
        sp<Connection> toConnection = getConnectionLocked(toToken);
        if (fromConnection != nullptr && toConnection != nullptr) {
            fromConnection->inputState.mergePointerStateTo(toConnection->inputState);
            CancelationOptions
                    options(CancelationOptions::Mode::CANCEL_POINTER_EVENTS,
                            "transferring touch focus from this window to another window");
            synthesizeCancelationEventsForConnectionLocked(fromConnection, options);
            synthesizePointerDownEventsForConnectionLocked(downTimeInTarget, toConnection,
                                                           newTargetFlags);
            transferWallpaperTouch(oldTargetFlags, newTargetFlags, fromWindowHandle, toWindowHandle,
                                   *state, pointerIds);
        }
    }
    mLooper->wake();
    return true;
}
sp<WindowInfoHandle> InputDispatcher::findTouchedForegroundWindowLocked(int32_t displayId) const {
    auto stateIt = mTouchStatesByDisplay.find(displayId);
    if (stateIt == mTouchStatesByDisplay.end()) {
        ALOGI("No touch state on display %" PRId32, displayId);
        return nullptr;
    }
    const TouchState& state = stateIt->second;
    sp<WindowInfoHandle> touchedForegroundWindow;
    for (const TouchedWindow& window : state.windows) {
        if (window.targetFlags.test(InputTarget::Flags::FOREGROUND)) {
            if (touchedForegroundWindow != nullptr) {
                ALOGI("Two or more foreground windows: %s and %s",
                      touchedForegroundWindow->getName().c_str(),
                      window.windowHandle->getName().c_str());
                return nullptr;
            }
            touchedForegroundWindow = window.windowHandle;
        }
    }
    return touchedForegroundWindow;
}
bool InputDispatcher::transferTouch(const sp<IBinder>& destChannelToken, int32_t displayId) {
    sp<IBinder> fromToken;
    {
        std::scoped_lock _l(mLock);
        sp<WindowInfoHandle> toWindowHandle = getWindowHandleLocked(destChannelToken, displayId);
        if (toWindowHandle == nullptr) {
            ALOGW("Could not find window associated with token=%p on display %" PRId32,
                  destChannelToken.get(), displayId);
            return false;
        }
        sp<WindowInfoHandle> from = findTouchedForegroundWindowLocked(displayId);
        if (from == nullptr) {
            ALOGE("Could not find a source window in %s for %p", __func__, destChannelToken.get());
            return false;
        }
        fromToken = from->getToken();
    }
    return transferTouchFocus(fromToken, destChannelToken);
}
void InputDispatcher::resetAndDropEverythingLocked(const char* reason) {
    if (DEBUG_FOCUS) {
        ALOGD("Resetting and dropping all events (%s).", reason);
    }
    CancelationOptions options(CancelationOptions::Mode::CANCEL_ALL_EVENTS, reason);
    synthesizeCancelationEventsForAllConnectionsLocked(options);
    resetKeyRepeatLocked();
    releasePendingEventLocked();
    drainInboundQueueLocked();
    resetNoFocusedWindowTimeoutLocked();
    mAnrTracker.clear();
    mTouchStatesByDisplay.clear();
    mReplacedKeys.clear();
}
void InputDispatcher::logDispatchStateLocked() {
    std::string dump;
    dumpDispatchStateLocked(dump);
    std::istringstream stream(dump);
    std::string line;
    while (std::getline(stream, line, '\n')) {
        ALOGD("%s", line.c_str());
    }
}
std::string InputDispatcher::dumpPointerCaptureStateLocked() {
    std::string dump;
    dump += StringPrintf(INDENT "Pointer Capture Requested: %s\n",
                         toString(mCurrentPointerCaptureRequest.enable));
    std::string windowName = "None";
    if (mWindowTokenWithPointerCapture) {
        const sp<WindowInfoHandle> captureWindowHandle =
                getWindowHandleLocked(mWindowTokenWithPointerCapture);
        windowName = captureWindowHandle ? captureWindowHandle->getName().c_str()
                                         : "token has capture without window";
    }
    dump += StringPrintf(INDENT "Current Window with Pointer Capture: %s\n", windowName.c_str());
    return dump;
}
void InputDispatcher::dumpDispatchStateLocked(std::string& dump) {
    dump += StringPrintf(INDENT "DispatchEnabled: %s\n", toString(mDispatchEnabled));
    dump += StringPrintf(INDENT "DispatchFrozen: %s\n", toString(mDispatchFrozen));
    dump += StringPrintf(INDENT "InputFilterEnabled: %s\n", toString(mInputFilterEnabled));
    dump += StringPrintf(INDENT "FocusedDisplayId: %" PRId32 "\n", mFocusedDisplayId);
    if (!mFocusedApplicationHandlesByDisplay.empty()) {
        dump += StringPrintf(INDENT "FocusedApplications:\n");
        for (auto& it : mFocusedApplicationHandlesByDisplay) {
            const int32_t displayId = it.first;
            const std::shared_ptr<InputApplicationHandle>& applicationHandle = it.second;
            const std::chrono::duration timeout =
                    applicationHandle->getDispatchingTimeout(DEFAULT_INPUT_DISPATCHING_TIMEOUT);
            dump += StringPrintf(INDENT2 "displayId=%" PRId32
                                         ", name='%s', dispatchingTimeout=%" PRId64 "ms\n",
                                 displayId, applicationHandle->getName().c_str(), millis(timeout));
        }
    } else {
        dump += StringPrintf(INDENT "FocusedApplications: <none>\n");
    }
    dump += mFocusResolver.dump();
    dump += dumpPointerCaptureStateLocked();
    if (!mTouchStatesByDisplay.empty()) {
        dump += StringPrintf(INDENT "TouchStatesByDisplay:\n");
        for (const auto& [displayId, state] : mTouchStatesByDisplay) {
            std::string touchStateDump = addLinePrefix(state.dump(), INDENT2);
            dump += INDENT2 + std::to_string(displayId) + " : " + touchStateDump;
        }
    } else {
        dump += INDENT "TouchStates: <no displays touched>\n";
    }
    if (mDragState) {
        dump += StringPrintf(INDENT "DragState:\n");
        mDragState->dump(dump, INDENT2);
    }
    if (!mWindowHandlesByDisplay.empty()) {
        for (const auto& [displayId, windowHandles] : mWindowHandlesByDisplay) {
            dump += StringPrintf(INDENT "Display: %" PRId32 "\n", displayId);
            if (const auto& it = mDisplayInfos.find(displayId); it != mDisplayInfos.end()) {
                const auto& displayInfo = it->second;
                dump += StringPrintf(INDENT2 "logicalSize=%dx%d\n", displayInfo.logicalWidth,
                                     displayInfo.logicalHeight);
                displayInfo.transform.dump(dump, "transform", INDENT4);
            } else {
                dump += INDENT2 "No DisplayInfo found!\n";
            }
            if (!windowHandles.empty()) {
                dump += INDENT2 "Windows:\n";
                for (size_t i = 0; i < windowHandles.size(); i++) {
                    const sp<WindowInfoHandle>& windowHandle = windowHandles[i];
                    const WindowInfo* windowInfo = windowHandle->getInfo();
                    dump += StringPrintf(INDENT3 "%zu: name='%s', id=%" PRId32 ", displayId=%d, "
                                                 "inputConfig=%s, alpha=%.2f, "
                                                 "frame=[%d,%d][%d,%d], globalScale=%f, "
                                                 "applicationInfo.name=%s, "
                                                 "applicationInfo.token=%s, "
                                                 "touchableRegion=",
                                         i, windowInfo->name.c_str(), windowInfo->id,
                                         windowInfo->displayId,
                                         windowInfo->inputConfig.string().c_str(),
                                         windowInfo->alpha, windowInfo->frameLeft,
                                         windowInfo->frameTop, windowInfo->frameRight,
                                         windowInfo->frameBottom, windowInfo->globalScaleFactor,
                                         windowInfo->applicationInfo.name.c_str(),
                                         toString(windowInfo->applicationInfo.token).c_str());
                    dump += dumpRegion(windowInfo->touchableRegion);
                    dump += StringPrintf(", ownerPid=%d, ownerUid=%d, dispatchingTimeout=%" PRId64
                                         "ms, hasToken=%s, "
                                         "touchOcclusionMode=%s\n",
                                         windowInfo->ownerPid, windowInfo->ownerUid,
                                         millis(windowInfo->dispatchingTimeout),
                                         toString(windowInfo->token != nullptr),
                                         toString(windowInfo->touchOcclusionMode).c_str());
                    windowInfo->transform.dump(dump, "transform", INDENT4);
                }
            } else {
                dump += INDENT2 "Windows: <none>\n";
            }
        }
    } else {
        dump += INDENT "Displays: <none>\n";
    }
    if (!mGlobalMonitorsByDisplay.empty()) {
        for (const auto& [displayId, monitors] : mGlobalMonitorsByDisplay) {
            dump += StringPrintf(INDENT "Global monitors on display %d:\n", displayId);
            dumpMonitors(dump, monitors);
        }
    } else {
        dump += INDENT "Global Monitors: <none>\n";
    }
    const nsecs_t currentTime = now();
    if (!mRecentQueue.empty()) {
        dump += StringPrintf(INDENT "RecentQueue: length=%zu\n", mRecentQueue.size());
        for (std::shared_ptr<EventEntry>& entry : mRecentQueue) {
            dump += INDENT2;
            dump += entry->getDescription();
            dump += StringPrintf(", age=%" PRId64 "ms\n", ns2ms(currentTime - entry->eventTime));
        }
    } else {
        dump += INDENT "RecentQueue: <empty>\n";
    }
    if (mPendingEvent) {
        dump += INDENT "PendingEvent:\n";
        dump += INDENT2;
        dump += mPendingEvent->getDescription();
        dump += StringPrintf(", age=%" PRId64 "ms\n",
                             ns2ms(currentTime - mPendingEvent->eventTime));
    } else {
        dump += INDENT "PendingEvent: <none>\n";
    }
    if (!mInboundQueue.empty()) {
        dump += StringPrintf(INDENT "InboundQueue: length=%zu\n", mInboundQueue.size());
        for (std::shared_ptr<EventEntry>& entry : mInboundQueue) {
            dump += INDENT2;
            dump += entry->getDescription();
            dump += StringPrintf(", age=%" PRId64 "ms\n", ns2ms(currentTime - entry->eventTime));
        }
    } else {
        dump += INDENT "InboundQueue: <empty>\n";
    }
    if (!mReplacedKeys.empty()) {
        dump += INDENT "ReplacedKeys:\n";
        for (const auto& [replacement, newKeyCode] : mReplacedKeys) {
            dump += StringPrintf(INDENT2 "originalKeyCode=%d, deviceId=%d -> newKeyCode=%d\n",
                                 replacement.keyCode, replacement.deviceId, newKeyCode);
        }
    } else {
        dump += INDENT "ReplacedKeys: <empty>\n";
    }
    if (!mCommandQueue.empty()) {
        dump += StringPrintf(INDENT "CommandQueue: size=%zu\n", mCommandQueue.size());
    } else {
        dump += INDENT "CommandQueue: <empty>\n";
    }
    if (!mConnectionsByToken.empty()) {
        dump += INDENT "Connections:\n";
        for (const auto& [token, connection] : mConnectionsByToken) {
            dump += StringPrintf(INDENT2 "%i: channelName='%s', windowName='%s', "
                                         "status=%s, monitor=%s, responsive=%s\n",
                                 connection->inputChannel->getFd().get(),
                                 connection->getInputChannelName().c_str(),
                                 connection->getWindowName().c_str(),
                                 ftl::enum_string(connection->status).c_str(),
                                 toString(connection->monitor), toString(connection->responsive));
            if (!connection->outboundQueue.empty()) {
                dump += StringPrintf(INDENT3 "OutboundQueue: length=%zu\n",
                                     connection->outboundQueue.size());
                dump += dumpQueue(connection->outboundQueue, currentTime);
            } else {
                dump += INDENT3 "OutboundQueue: <empty>\n";
            }
            if (!connection->waitQueue.empty()) {
                dump += StringPrintf(INDENT3 "WaitQueue: length=%zu\n",
                                     connection->waitQueue.size());
                dump += dumpQueue(connection->waitQueue, currentTime);
            } else {
                dump += INDENT3 "WaitQueue: <empty>\n";
            }
        }
    } else {
        dump += INDENT "Connections: <none>\n";
    }
    if (isAppSwitchPendingLocked()) {
        dump += StringPrintf(INDENT "AppSwitch: pending, due in %" PRId64 "ms\n",
                             ns2ms(mAppSwitchDueTime - now()));
    } else {
        dump += INDENT "AppSwitch: not pending\n";
    }
    if (!mTouchModePerDisplay.empty()) {
        dump += INDENT "TouchModePerDisplay:\n";
        for (const auto& [displayId, touchMode] : mTouchModePerDisplay) {
            dump += StringPrintf(INDENT2 "Display: %" PRId32 " TouchMode: %s\n", displayId,
                                 std::to_string(touchMode).c_str());
        }
    } else {
        dump += INDENT "TouchModePerDisplay: <none>\n";
    }
    dump += INDENT "Configuration:\n";
    dump += StringPrintf(INDENT2 "KeyRepeatDelay: %" PRId64 "ms\n", ns2ms(mConfig.keyRepeatDelay));
    dump += StringPrintf(INDENT2 "KeyRepeatTimeout: %" PRId64 "ms\n",
                         ns2ms(mConfig.keyRepeatTimeout));
    dump += mLatencyTracker.dump(INDENT2);
    dump += mLatencyAggregator.dump(INDENT2);
}
void InputDispatcher::dumpMonitors(std::string& dump, const std::vector<Monitor>& monitors) {
    const size_t numMonitors = monitors.size();
    for (size_t i = 0; i < numMonitors; i++) {
        const Monitor& monitor = monitors[i];
        const std::shared_ptr<InputChannel>& channel = monitor.inputChannel;
        dump += StringPrintf(INDENT2 "%zu: '%s', ", i, channel->getName().c_str());
        dump += "\n";
    }
}
class LooperEventCallback : public LooperCallback {
public:
    LooperEventCallback(std::function<int(int events)> callback) : mCallback(callback) {}
    int handleEvent(int , int events, void* ) override { return mCallback(events); }
private:
    std::function<int(int events)> mCallback;
};
Result<std::unique_ptr<InputChannel>> InputDispatcher::createInputChannel(const std::string& name) {
    if (DEBUG_CHANNEL_CREATION) {
        ALOGD("channel '%s' ~ createInputChannel", name.c_str());
    }
    std::unique_ptr<InputChannel> serverChannel;
    std::unique_ptr<InputChannel> clientChannel;
    status_t result = InputChannel::openInputChannelPair(name, serverChannel, clientChannel);
    if (result) {
        return base::Error(result) << "Failed to open input channel pair with name " << name;
    }
    {
        std::scoped_lock _l(mLock);
        const sp<IBinder>& token = serverChannel->getConnectionToken();
        int fd = serverChannel->getFd();
        sp<Connection> connection =
                sp<Connection>::make(std::move(serverChannel), false, mIdGenerator);
        if (mConnectionsByToken.find(token) != mConnectionsByToken.end()) {
            ALOGE("Created a new connection, but the token %p is already known", token.get());
        }
        mConnectionsByToken.emplace(token, connection);
        std::function<int(int events)> callback = std::bind(&InputDispatcher::handleReceiveCallback,
                                                            this, std::placeholders::_1, token);
        mLooper->addFd(fd, 0, ALOOPER_EVENT_INPUT, sp<LooperEventCallback>::make(callback),
                       nullptr);
    }
    mLooper->wake();
    return clientChannel;
}
Result<std::unique_ptr<InputChannel>> InputDispatcher::createInputMonitor(int32_t displayId,
                                                                          const std::string& name,
                                                                          int32_t pid) {
    std::shared_ptr<InputChannel> serverChannel;
    std::unique_ptr<InputChannel> clientChannel;
    status_t result = openInputChannelPair(name, serverChannel, clientChannel);
    if (result) {
        return base::Error(result) << "Failed to open input channel pair with name " << name;
    }
    {
        std::scoped_lock _l(mLock);
        if (displayId < 0) {
            return base::Error(BAD_VALUE) << "Attempted to create input monitor with name " << name
                                          << " without a specified display.";
        }
        sp<Connection> connection =
                sp<Connection>::make(serverChannel, true, mIdGenerator);
        const sp<IBinder>& token = serverChannel->getConnectionToken();
        const int fd = serverChannel->getFd();
        if (mConnectionsByToken.find(token) != mConnectionsByToken.end()) {
            ALOGE("Created a new connection, but the token %p is already known", token.get());
        }
        mConnectionsByToken.emplace(token, connection);
        std::function<int(int events)> callback = std::bind(&InputDispatcher::handleReceiveCallback,
                                                            this, std::placeholders::_1, token);
        mGlobalMonitorsByDisplay[displayId].emplace_back(serverChannel, pid);
        mLooper->addFd(fd, 0, ALOOPER_EVENT_INPUT, sp<LooperEventCallback>::make(callback),
                       nullptr);
    }
    mLooper->wake();
    return clientChannel;
}
status_t InputDispatcher::removeInputChannel(const sp<IBinder>& connectionToken) {
    {
        std::scoped_lock _l(mLock);
        status_t status = removeInputChannelLocked(connectionToken, false);
        if (status) {
            return status;
        }
    }
    mLooper->wake();
    return OK;
}
status_t InputDispatcher::removeInputChannelLocked(const sp<IBinder>& connectionToken,
                                                   bool notify) {
    sp<Connection> connection = getConnectionLocked(connectionToken);
    if (connection == nullptr) {
        return BAD_VALUE;
    }
    removeConnectionLocked(connection);
    if (connection->monitor) {
        removeMonitorChannelLocked(connectionToken);
    }
    mLooper->removeFd(connection->inputChannel->getFd());
    nsecs_t currentTime = now();
    abortBrokenDispatchCycleLocked(currentTime, connection, notify);
    connection->status = Connection::Status::ZOMBIE;
    return OK;
}
void InputDispatcher::removeMonitorChannelLocked(const sp<IBinder>& connectionToken) {
    for (auto it = mGlobalMonitorsByDisplay.begin(); it != mGlobalMonitorsByDisplay.end();) {
        auto& [displayId, monitors] = *it;
        std::erase_if(monitors, [connectionToken](const Monitor& monitor) {
            return monitor.inputChannel->getConnectionToken() == connectionToken;
        });
        if (monitors.empty()) {
            it = mGlobalMonitorsByDisplay.erase(it);
        } else {
            ++it;
        }
    }
}
status_t InputDispatcher::pilferPointers(const sp<IBinder>& token) {
    std::scoped_lock _l(mLock);
    return pilferPointersLocked(token);
}
status_t InputDispatcher::pilferPointersLocked(const sp<IBinder>& token) {
    const std::shared_ptr<InputChannel> requestingChannel = getInputChannelLocked(token);
    if (!requestingChannel) {
        ALOGW("Attempted to pilfer pointers from an un-registered channel or invalid token");
        return BAD_VALUE;
    }
    auto [statePtr, windowPtr, displayId] = findTouchStateWindowAndDisplayLocked(token);
    if (statePtr == nullptr || windowPtr == nullptr || windowPtr->pointerIds.none()) {
        ALOGW("Attempted to pilfer points from a channel without any on-going pointer streams."
              " Ignoring.");
        return BAD_VALUE;
    }
    TouchState& state = *statePtr;
    TouchedWindow& window = *windowPtr;
    CancelationOptions options(CancelationOptions::Mode::CANCEL_POINTER_EVENTS,
                               "input channel stole pointer stream");
    options.deviceId = state.deviceId;
    options.displayId = displayId;
    options.pointerIds = window.pointerIds;
    std::string canceledWindows;
    for (const TouchedWindow& w : state.windows) {
        const std::shared_ptr<InputChannel> channel =
                getInputChannelLocked(w.windowHandle->getToken());
        if (channel != nullptr && channel->getConnectionToken() != token) {
            synthesizeCancelationEventsForInputChannelLocked(channel, options);
            canceledWindows += canceledWindows.empty() ? "[" : ", ";
            canceledWindows += channel->getName();
        }
    }
    canceledWindows += canceledWindows.empty() ? "[]" : "]";
    ALOGI("Channel %s is stealing touch from %s", requestingChannel->getName().c_str(),
          canceledWindows.c_str());
    window.pilferedPointerIds |= window.pointerIds;
    state.cancelPointersForWindowsExcept(window.pointerIds, token);
    return OK;
}
void InputDispatcher::requestPointerCapture(const sp<IBinder>& windowToken, bool enabled) {
    {
        std::scoped_lock _l(mLock);
        if (DEBUG_FOCUS) {
            const sp<WindowInfoHandle> windowHandle = getWindowHandleLocked(windowToken);
            ALOGI("Request to %s Pointer Capture from: %s.", enabled ? "enable" : "disable",
                  windowHandle != nullptr ? windowHandle->getName().c_str()
                                          : "token without window");
        }
        const sp<IBinder> focusedToken = mFocusResolver.getFocusedWindowToken(mFocusedDisplayId);
        if (focusedToken != windowToken) {
            ALOGW("Ignoring request to %s Pointer Capture: window does not have focus.",
                  enabled ? "enable" : "disable");
            return;
        }
        if (enabled == mCurrentPointerCaptureRequest.enable) {
            ALOGW("Ignoring request to %s Pointer Capture: "
                  "window has %s requested pointer capture.",
                  enabled ? "enable" : "disable", enabled ? "already" : "not");
            return;
        }
        if (enabled) {
            if (std::find(mIneligibleDisplaysForPointerCapture.begin(),
                          mIneligibleDisplaysForPointerCapture.end(),
                          mFocusedDisplayId) != mIneligibleDisplaysForPointerCapture.end()) {
                ALOGW("Ignoring request to enable Pointer Capture: display is not eligible");
                return;
            }
        }
        setPointerCaptureLocked(enabled);
    }
    mLooper->wake();
}
void InputDispatcher::setDisplayEligibilityForPointerCapture(int32_t displayId, bool isEligible) {
    {
        std::scoped_lock _l(mLock);
        std::erase(mIneligibleDisplaysForPointerCapture, displayId);
        if (!isEligible) {
            mIneligibleDisplaysForPointerCapture.push_back(displayId);
        }
    }
}
std::optional<int32_t> InputDispatcher::findMonitorPidByTokenLocked(const sp<IBinder>& token) {
    for (const auto& [_, monitors] : mGlobalMonitorsByDisplay) {
        for (const Monitor& monitor : monitors) {
            if (monitor.inputChannel->getConnectionToken() == token) {
                return monitor.pid;
            }
        }
    }
    return std::nullopt;
}
sp<Connection> InputDispatcher::getConnectionLocked(const sp<IBinder>& inputConnectionToken) const {
    if (inputConnectionToken == nullptr) {
        return nullptr;
    }
    for (const auto& [token, connection] : mConnectionsByToken) {
        if (token == inputConnectionToken) {
            return connection;
        }
    }
    return nullptr;
}
std::string InputDispatcher::getConnectionNameLocked(const sp<IBinder>& connectionToken) const {
    sp<Connection> connection = getConnectionLocked(connectionToken);
    if (connection == nullptr) {
        return "<nullptr>";
    }
    return connection->getInputChannelName();
}
void InputDispatcher::removeConnectionLocked(const sp<Connection>& connection) {
    mAnrTracker.eraseToken(connection->inputChannel->getConnectionToken());
    mConnectionsByToken.erase(connection->inputChannel->getConnectionToken());
}
void InputDispatcher::doDispatchCycleFinishedCommand(nsecs_t finishTime,
                                                     const sp<Connection>& connection, uint32_t seq,
                                                     bool handled, nsecs_t consumeTime) {
    std::deque<DispatchEntry*>::iterator dispatchEntryIt = connection->findWaitQueueEntry(seq);
    if (dispatchEntryIt == connection->waitQueue.end()) {
        return;
    }
    DispatchEntry* dispatchEntry = *dispatchEntryIt;
    const nsecs_t eventDuration = finishTime - dispatchEntry->deliveryTime;
    if (eventDuration > SLOW_EVENT_PROCESSING_WARNING_TIMEOUT) {
        ALOGI("%s spent %" PRId64 "ms processing %s", connection->getWindowName().c_str(),
              ns2ms(eventDuration), dispatchEntry->eventEntry->getDescription().c_str());
    }
    if (shouldReportFinishedEvent(*dispatchEntry, *connection)) {
        mLatencyTracker.trackFinishedEvent(dispatchEntry->eventEntry->id,
                                           connection->inputChannel->getConnectionToken(),
                                           dispatchEntry->deliveryTime, consumeTime, finishTime);
    }
    bool restartEvent;
    if (dispatchEntry->eventEntry->type == EventEntry::Type::KEY) {
        KeyEntry& keyEntry = static_cast<KeyEntry&>(*(dispatchEntry->eventEntry));
        restartEvent =
                afterKeyEventLockedInterruptable(connection, dispatchEntry, keyEntry, handled);
    } else if (dispatchEntry->eventEntry->type == EventEntry::Type::MOTION) {
        MotionEntry& motionEntry = static_cast<MotionEntry&>(*(dispatchEntry->eventEntry));
        restartEvent = afterMotionEventLockedInterruptable(connection, dispatchEntry, motionEntry,
                                                           handled);
    } else {
        restartEvent = false;
    }
    dispatchEntryIt = connection->findWaitQueueEntry(seq);
    if (dispatchEntryIt != connection->waitQueue.end()) {
        dispatchEntry = *dispatchEntryIt;
        connection->waitQueue.erase(dispatchEntryIt);
        const sp<IBinder>& connectionToken = connection->inputChannel->getConnectionToken();
        mAnrTracker.erase(dispatchEntry->timeoutTime, connectionToken);
        if (!connection->responsive) {
            connection->responsive = isConnectionResponsive(*connection);
            if (connection->responsive) {
                processConnectionResponsiveLocked(*connection);
            }
        }
        traceWaitQueueLength(*connection);
        if (restartEvent && connection->status == Connection::Status::NORMAL) {
            connection->outboundQueue.push_front(dispatchEntry);
            traceOutboundQueueLength(*connection);
        } else {
            releaseDispatchEntry(dispatchEntry);
        }
    }
    startDispatchCycleLocked(now(), connection);
}
void InputDispatcher::sendFocusChangedCommandLocked(const sp<IBinder>& oldToken,
                                                    const sp<IBinder>& newToken) {
    auto command = [this, oldToken, newToken]() REQUIRES(mLock) {
        scoped_unlock unlock(mLock);
        mPolicy->notifyFocusChanged(oldToken, newToken);
    };
    postCommandLocked(std::move(command));
}
void InputDispatcher::sendDropWindowCommandLocked(const sp<IBinder>& token, float x, float y) {
    auto command = [this, token, x, y]() REQUIRES(mLock) {
        scoped_unlock unlock(mLock);
        mPolicy->notifyDropWindow(token, x, y);
    };
    postCommandLocked(std::move(command));
}
void InputDispatcher::onAnrLocked(const sp<Connection>& connection) {
    if (connection == nullptr) {
        LOG_ALWAYS_FATAL("Caller must check for nullness");
    }
    if (connection->waitQueue.empty()) {
        ALOGI("Not raising ANR because the connection %s has recovered",
              connection->inputChannel->getName().c_str());
        return;
    }
    DispatchEntry* oldestEntry = *connection->waitQueue.begin();
    const nsecs_t currentWait = now() - oldestEntry->deliveryTime;
    std::string reason =
            android::base::StringPrintf("%s is not responding. Waited %" PRId64 "ms for %s",
                                        connection->inputChannel->getName().c_str(),
                                        ns2ms(currentWait),
                                        oldestEntry->eventEntry->getDescription().c_str());
    sp<IBinder> connectionToken = connection->inputChannel->getConnectionToken();
    updateLastAnrStateLocked(getWindowHandleLocked(connectionToken), reason);
    processConnectionUnresponsiveLocked(*connection, std::move(reason));
    cancelEventsForAnrLocked(connection);
}
void InputDispatcher::onAnrLocked(std::shared_ptr<InputApplicationHandle> application) {
    std::string reason =
            StringPrintf("%s does not have a focused window", application->getName().c_str());
    updateLastAnrStateLocked(*application, reason);
    auto command = [this, application = std::move(application)]() REQUIRES(mLock) {
        scoped_unlock unlock(mLock);
        mPolicy->notifyNoFocusedWindowAnr(application);
    };
    postCommandLocked(std::move(command));
}
void InputDispatcher::updateLastAnrStateLocked(const sp<WindowInfoHandle>& window,
                                               const std::string& reason) {
    const std::string windowLabel = getApplicationWindowLabel(nullptr, window);
    updateLastAnrStateLocked(windowLabel, reason);
}
void InputDispatcher::updateLastAnrStateLocked(const InputApplicationHandle& application,
                                               const std::string& reason) {
    const std::string windowLabel = getApplicationWindowLabel(&application, nullptr);
    updateLastAnrStateLocked(windowLabel, reason);
}
void InputDispatcher::updateLastAnrStateLocked(const std::string& windowLabel,
                                               const std::string& reason) {
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%F %T", &tm);
    mLastAnrState.clear();
    mLastAnrState += INDENT "ANR:\n";
    mLastAnrState += StringPrintf(INDENT2 "Time: %s\n", timestr);
    mLastAnrState += StringPrintf(INDENT2 "Reason: %s\n", reason.c_str());
    mLastAnrState += StringPrintf(INDENT2 "Window: %s\n", windowLabel.c_str());
    dumpDispatchStateLocked(mLastAnrState);
}
void InputDispatcher::doInterceptKeyBeforeDispatchingCommand(const sp<IBinder>& focusedWindowToken,
                                                             KeyEntry& entry) {
    const KeyEvent event = createKeyEvent(entry);
    nsecs_t delay = 0;
    {
        scoped_unlock unlock(mLock);
        android::base::Timer t;
        delay = mPolicy->interceptKeyBeforeDispatching(focusedWindowToken, &event,
                                                       entry.policyFlags);
        if (t.duration() > SLOW_INTERCEPTION_THRESHOLD) {
            ALOGW("Excessive delay in interceptKeyBeforeDispatching; took %s ms",
                  std::to_string(t.duration().count()).c_str());
        }
    }
    if (delay < 0) {
        entry.interceptKeyResult = KeyEntry::InterceptKeyResult::SKIP;
    } else if (delay == 0) {
        entry.interceptKeyResult = KeyEntry::InterceptKeyResult::CONTINUE;
    } else {
        entry.interceptKeyResult = KeyEntry::InterceptKeyResult::TRY_AGAIN_LATER;
        entry.interceptKeyWakeupTime = now() + delay;
    }
}
void InputDispatcher::sendWindowUnresponsiveCommandLocked(const sp<IBinder>& token,
                                                          std::optional<int32_t> pid,
                                                          std::string reason) {
    auto command = [this, token, pid, reason = std::move(reason)]() REQUIRES(mLock) {
        scoped_unlock unlock(mLock);
        mPolicy->notifyWindowUnresponsive(token, pid, reason);
    };
    postCommandLocked(std::move(command));
}
void InputDispatcher::sendWindowResponsiveCommandLocked(const sp<IBinder>& token,
                                                        std::optional<int32_t> pid) {
    auto command = [this, token, pid]() REQUIRES(mLock) {
        scoped_unlock unlock(mLock);
        mPolicy->notifyWindowResponsive(token, pid);
    };
    postCommandLocked(std::move(command));
}
void InputDispatcher::processConnectionUnresponsiveLocked(const Connection& connection,
                                                          std::string reason) {
    const sp<IBinder>& connectionToken = connection.inputChannel->getConnectionToken();
    std::optional<int32_t> pid;
    if (connection.monitor) {
        ALOGW("Monitor %s is unresponsive: %s", connection.inputChannel->getName().c_str(),
              reason.c_str());
        pid = findMonitorPidByTokenLocked(connectionToken);
    } else {
        ALOGW("Window %s is unresponsive: %s", connection.inputChannel->getName().c_str(),
              reason.c_str());
        const sp<WindowInfoHandle> handle = getWindowHandleLocked(connectionToken);
        if (handle != nullptr) {
            pid = handle->getInfo()->ownerPid;
        }
    }
    sendWindowUnresponsiveCommandLocked(connectionToken, pid, std::move(reason));
}
void InputDispatcher::processConnectionResponsiveLocked(const Connection& connection) {
    const sp<IBinder>& connectionToken = connection.inputChannel->getConnectionToken();
    std::optional<int32_t> pid;
    if (connection.monitor) {
        pid = findMonitorPidByTokenLocked(connectionToken);
    } else {
        const sp<WindowInfoHandle> handle = getWindowHandleLocked(connectionToken);
        if (handle != nullptr) {
            pid = handle->getInfo()->ownerPid;
        }
    }
    sendWindowResponsiveCommandLocked(connectionToken, pid);
}
bool InputDispatcher::afterKeyEventLockedInterruptable(const sp<Connection>& connection,
                                                       DispatchEntry* dispatchEntry,
                                                       KeyEntry& keyEntry, bool handled) {
    if (keyEntry.flags & AKEY_EVENT_FLAG_FALLBACK) {
        if (!handled) {
            mReporter->reportUnhandledKey(keyEntry.id);
        }
        return false;
    }
    int32_t originalKeyCode = keyEntry.keyCode;
    int32_t fallbackKeyCode = connection->inputState.getFallbackKey(originalKeyCode);
    if (keyEntry.action == AKEY_EVENT_ACTION_UP) {
        connection->inputState.removeFallbackKey(originalKeyCode);
    }
    if (handled || !dispatchEntry->hasForegroundTarget()) {
        if (fallbackKeyCode != -1) {
            if (DEBUG_OUTBOUND_EVENT_DETAILS) {
                ALOGD("Unhandled key event: Asking policy to cancel fallback action.  "
                      "keyCode=%d, action=%d, repeatCount=%d, policyFlags=0x%08x",
                      keyEntry.keyCode, keyEntry.action, keyEntry.repeatCount,
                      keyEntry.policyFlags);
            }
            KeyEvent event = createKeyEvent(keyEntry);
            event.setFlags(event.getFlags() | AKEY_EVENT_FLAG_CANCELED);
            mLock.unlock();
            mPolicy->dispatchUnhandledKey(connection->inputChannel->getConnectionToken(), &event,
                                          keyEntry.policyFlags, &event);
            mLock.lock();
            if (fallbackKeyCode != AKEYCODE_UNKNOWN) {
                CancelationOptions options(CancelationOptions::Mode::CANCEL_FALLBACK_EVENTS,
                                           "application handled the original non-fallback key "
                                           "or is no longer a foreground target, "
                                           "canceling previously dispatched fallback key");
                options.keyCode = fallbackKeyCode;
                synthesizeCancelationEventsForConnectionLocked(connection, options);
            }
            connection->inputState.removeFallbackKey(originalKeyCode);
        }
    } else {
        bool initialDown = keyEntry.action == AKEY_EVENT_ACTION_DOWN && keyEntry.repeatCount == 0;
        if (fallbackKeyCode == -1 && !initialDown) {
            if (DEBUG_OUTBOUND_EVENT_DETAILS) {
                ALOGD("Unhandled key event: Skipping unhandled key event processing "
                      "since this is not an initial down.  "
                      "keyCode=%d, action=%d, repeatCount=%d, policyFlags=0x%08x",
                      originalKeyCode, keyEntry.action, keyEntry.repeatCount, keyEntry.policyFlags);
            }
            return false;
        }
        if (DEBUG_OUTBOUND_EVENT_DETAILS) {
            ALOGD("Unhandled key event: Asking policy to perform fallback action.  "
                  "keyCode=%d, action=%d, repeatCount=%d, policyFlags=0x%08x",
                  keyEntry.keyCode, keyEntry.action, keyEntry.repeatCount, keyEntry.policyFlags);
        }
        KeyEvent event = createKeyEvent(keyEntry);
        mLock.unlock();
        bool fallback =
                mPolicy->dispatchUnhandledKey(connection->inputChannel->getConnectionToken(),
                                              &event, keyEntry.policyFlags, &event);
        mLock.lock();
        if (connection->status != Connection::Status::NORMAL) {
            connection->inputState.removeFallbackKey(originalKeyCode);
            return false;
        }
        if (initialDown) {
            if (fallback) {
                fallbackKeyCode = event.getKeyCode();
            } else {
                fallbackKeyCode = AKEYCODE_UNKNOWN;
            }
            connection->inputState.setFallbackKey(originalKeyCode, fallbackKeyCode);
        }
        ALOG_ASSERT(fallbackKeyCode != -1);
        if (fallbackKeyCode != AKEYCODE_UNKNOWN &&
            (!fallback || fallbackKeyCode != event.getKeyCode())) {
            if (DEBUG_OUTBOUND_EVENT_DETAILS) {
                if (fallback) {
                    ALOGD("Unhandled key event: Policy requested to send key %d"
                          "as a fallback for %d, but on the DOWN it had requested "
                          "to send %d instead.  Fallback canceled.",
                          event.getKeyCode(), originalKeyCode, fallbackKeyCode);
                } else {
                    ALOGD("Unhandled key event: Policy did not request fallback for %d, "
                          "but on the DOWN it had requested to send %d.  "
                          "Fallback canceled.",
                          originalKeyCode, fallbackKeyCode);
                }
            }
            CancelationOptions options(CancelationOptions::Mode::CANCEL_FALLBACK_EVENTS,
                                       "canceling fallback, policy no longer desires it");
            options.keyCode = fallbackKeyCode;
            synthesizeCancelationEventsForConnectionLocked(connection, options);
            fallback = false;
            fallbackKeyCode = AKEYCODE_UNKNOWN;
            if (keyEntry.action != AKEY_EVENT_ACTION_UP) {
                connection->inputState.setFallbackKey(originalKeyCode, fallbackKeyCode);
            }
        }
        if (DEBUG_OUTBOUND_EVENT_DETAILS) {
            {
                std::string msg;
                const KeyedVector<int32_t, int32_t>& fallbackKeys =
                        connection->inputState.getFallbackKeys();
                for (size_t i = 0; i < fallbackKeys.size(); i++) {
                    msg += StringPrintf(", %d->%d", fallbackKeys.keyAt(i), fallbackKeys.valueAt(i));
                }
                ALOGD("Unhandled key event: %zu currently tracked fallback keys%s.",
                      fallbackKeys.size(), msg.c_str());
            }
        }
        if (fallback) {
            keyEntry.eventTime = event.getEventTime();
            keyEntry.deviceId = event.getDeviceId();
            keyEntry.source = event.getSource();
            keyEntry.displayId = event.getDisplayId();
            keyEntry.flags = event.getFlags() | AKEY_EVENT_FLAG_FALLBACK;
            keyEntry.keyCode = fallbackKeyCode;
            keyEntry.scanCode = event.getScanCode();
            keyEntry.metaState = event.getMetaState();
            keyEntry.repeatCount = event.getRepeatCount();
            keyEntry.downTime = event.getDownTime();
            keyEntry.syntheticRepeat = false;
            if (DEBUG_OUTBOUND_EVENT_DETAILS) {
                ALOGD("Unhandled key event: Dispatching fallback key.  "
                      "originalKeyCode=%d, fallbackKeyCode=%d, fallbackMetaState=%08x",
                      originalKeyCode, fallbackKeyCode, keyEntry.metaState);
            }
            return true;
        } else {
            if (DEBUG_OUTBOUND_EVENT_DETAILS) {
                ALOGD("Unhandled key event: No fallback key.");
            }
            mReporter->reportUnhandledKey(keyEntry.id);
        }
    }
    return false;
}
bool InputDispatcher::afterMotionEventLockedInterruptable(const sp<Connection>& connection,
                                                          DispatchEntry* dispatchEntry,
                                                          MotionEntry& motionEntry, bool handled) {
    return false;
}
void InputDispatcher::traceInboundQueueLengthLocked() {
    if (ATRACE_ENABLED()) {
        ATRACE_INT("iq", mInboundQueue.size());
    }
}
void InputDispatcher::traceOutboundQueueLength(const Connection& connection) {
    if (ATRACE_ENABLED()) {
        char counterName[40];
        snprintf(counterName, sizeof(counterName), "oq:%s", connection.getWindowName().c_str());
        ATRACE_INT(counterName, connection.outboundQueue.size());
    }
}
void InputDispatcher::traceWaitQueueLength(const Connection& connection) {
    if (ATRACE_ENABLED()) {
        char counterName[40];
        snprintf(counterName, sizeof(counterName), "wq:%s", connection.getWindowName().c_str());
        ATRACE_INT(counterName, connection.waitQueue.size());
    }
}
void InputDispatcher::dump(std::string& dump) {
    std::scoped_lock _l(mLock);
    dump += "Input Dispatcher State:\n";
    dumpDispatchStateLocked(dump);
    if (!mLastAnrState.empty()) {
        dump += "\nInput Dispatcher State at time of last ANR:\n";
        dump += mLastAnrState;
    }
}
void InputDispatcher::monitor() {
    std::unique_lock _l(mLock);
    mLooper->wake();
    mDispatcherIsAlive.wait(_l);
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
void InputDispatcher::setFocusedWindow(const FocusRequest& request) {
    {
        std::scoped_lock _l(mLock);
        std::optional<FocusResolver::FocusChanges> changes =
                mFocusResolver.setFocusedWindow(request, getWindowHandlesLocked(request.displayId));
        if (changes) {
            onFocusChangedLocked(*changes);
        }
    }
    mLooper->wake();
}
void InputDispatcher::onFocusChangedLocked(const FocusResolver::FocusChanges& changes) {
    if (changes.oldFocus) {
        std::shared_ptr<InputChannel> focusedInputChannel = getInputChannelLocked(changes.oldFocus);
        if (focusedInputChannel) {
            CancelationOptions options(CancelationOptions::Mode::CANCEL_NON_POINTER_EVENTS,
                                       "focus left window");
            synthesizeCancelationEventsForInputChannelLocked(focusedInputChannel, options);
            enqueueFocusEventLocked(changes.oldFocus, false, changes.reason);
        }
    }
    if (changes.newFocus) {
        enqueueFocusEventLocked(changes.newFocus, true, changes.reason);
    }
    disablePointerCaptureForcedLocked();
    if (mFocusedDisplayId == changes.displayId) {
        sendFocusChangedCommandLocked(changes.oldFocus, changes.newFocus);
    }
}
void InputDispatcher::disablePointerCaptureForcedLocked() {
    if (!mCurrentPointerCaptureRequest.enable && !mWindowTokenWithPointerCapture) {
        return;
    }
    ALOGD_IF(DEBUG_FOCUS, "Disabling Pointer Capture because the window lost focus.");
    if (mCurrentPointerCaptureRequest.enable) {
        setPointerCaptureLocked(false);
    }
    if (!mWindowTokenWithPointerCapture) {
        return;
    }
    if (mPendingEvent != nullptr) {
        mInboundQueue.push_front(mPendingEvent);
        mPendingEvent = nullptr;
    }
    auto entry = std::make_unique<PointerCaptureChangedEntry>(mIdGenerator.nextId(), now(),
                                                              mCurrentPointerCaptureRequest);
    mInboundQueue.push_front(std::move(entry));
}
void InputDispatcher::setPointerCaptureLocked(bool enable) {
    mCurrentPointerCaptureRequest.enable = enable;
    mCurrentPointerCaptureRequest.seq++;
    auto command = [this, request = mCurrentPointerCaptureRequest]() REQUIRES(mLock) {
        scoped_unlock unlock(mLock);
        mPolicy->setPointerCapture(request);
    };
    postCommandLocked(std::move(command));
}
void InputDispatcher::displayRemoved(int32_t displayId) {
    {
        std::scoped_lock _l(mLock);
        setInputWindowsLocked( {}, displayId);
        setFocusedApplicationLocked(displayId, nullptr);
        mFocusResolver.displayRemoved(displayId);
        std::erase(mIneligibleDisplaysForPointerCapture, displayId);
        mTouchModePerDisplay.erase(displayId);
    }
    mLooper->wake();
}
void InputDispatcher::onWindowInfosChanged(const std::vector<WindowInfo>& windowInfos,
                                           const std::vector<DisplayInfo>& displayInfos) {
    std::unordered_map<int32_t, std::vector<sp<WindowInfoHandle>>> handlesPerDisplay;
    for (const auto& info : windowInfos) {
        handlesPerDisplay.emplace(info.displayId, std::vector<sp<WindowInfoHandle>>());
        handlesPerDisplay[info.displayId].push_back(sp<WindowInfoHandle>::make(info));
    }
    {
        std::scoped_lock _l(mLock);
        for (const auto& [displayId, _] : mWindowHandlesByDisplay) {
            handlesPerDisplay[displayId];
        }
        mDisplayInfos.clear();
        for (const auto& displayInfo : displayInfos) {
            mDisplayInfos.emplace(displayInfo.displayId, displayInfo);
        }
        for (const auto& [displayId, handles] : handlesPerDisplay) {
            setInputWindowsLocked(handles, displayId);
        }
    }
    mLooper->wake();
}
bool InputDispatcher::shouldDropInput(
        const EventEntry& entry, const sp<android::gui::WindowInfoHandle>& windowHandle) const {
    if (windowHandle->getInfo()->inputConfig.test(WindowInfo::InputConfig::DROP_INPUT) ||
        (windowHandle->getInfo()->inputConfig.test(
                 WindowInfo::InputConfig::DROP_INPUT_IF_OBSCURED) &&
         isWindowObscuredLocked(windowHandle))) {
        ALOGW("Dropping %s event targeting %s as requested by the input configuration {%s} on "
              "display %" PRId32 ".",
              ftl::enum_string(entry.type).c_str(), windowHandle->getName().c_str(),
              windowHandle->getInfo()->inputConfig.string().c_str(),
              windowHandle->getInfo()->displayId);
        return true;
    }
    return false;
}
void InputDispatcher::DispatcherWindowListener::onWindowInfosChanged(
        const std::vector<gui::WindowInfo>& windowInfos,
        const std::vector<DisplayInfo>& displayInfos) {
    mDispatcher.onWindowInfosChanged(windowInfos, displayInfos);
}
void InputDispatcher::cancelCurrentTouch() {
    {
        std::scoped_lock _l(mLock);
        ALOGD("Canceling all ongoing pointer gestures on all displays.");
        CancelationOptions options(CancelationOptions::Mode::CANCEL_POINTER_EVENTS,
                                   "cancel current touch");
        synthesizeCancelationEventsForAllConnectionsLocked(options);
        mTouchStatesByDisplay.clear();
    }
    mLooper->wake();
}
void InputDispatcher::setMonitorDispatchingTimeoutForTest(std::chrono::nanoseconds timeout) {
    std::scoped_lock _l(mLock);
    mMonitorDispatchingTimeout = timeout;
}
void InputDispatcher::slipWallpaperTouch(ftl::Flags<InputTarget::Flags> targetFlags,
                                         const sp<WindowInfoHandle>& oldWindowHandle,
                                         const sp<WindowInfoHandle>& newWindowHandle,
                                         TouchState& state, int32_t pointerId,
                                         std::vector<InputTarget>& targets) {
    std::bitset<MAX_POINTER_ID + 1> pointerIds;
    pointerIds.set(pointerId);
    const bool oldHasWallpaper = oldWindowHandle->getInfo()->inputConfig.test(
            gui::WindowInfo::InputConfig::DUPLICATE_TOUCH_TO_WALLPAPER);
    const bool newHasWallpaper = targetFlags.test(InputTarget::Flags::FOREGROUND) &&
            newWindowHandle->getInfo()->inputConfig.test(
                    gui::WindowInfo::InputConfig::DUPLICATE_TOUCH_TO_WALLPAPER);
    const sp<WindowInfoHandle> oldWallpaper =
            oldHasWallpaper ? state.getWallpaperWindow() : nullptr;
    const sp<WindowInfoHandle> newWallpaper =
            newHasWallpaper ? findWallpaperWindowBelow(newWindowHandle) : nullptr;
    if (oldWallpaper == newWallpaper) {
        return;
    }
    if (oldWallpaper != nullptr) {
        const TouchedWindow& oldTouchedWindow = state.getTouchedWindow(oldWallpaper);
        addWindowTargetLocked(oldWallpaper,
                              oldTouchedWindow.targetFlags |
                                      InputTarget::Flags::DISPATCH_AS_SLIPPERY_EXIT,
                              pointerIds, oldTouchedWindow.firstDownTimeInTarget, targets);
        state.removeTouchedPointerFromWindow(pointerId, oldWallpaper);
    }
    if (newWallpaper != nullptr) {
        state.addOrUpdateWindow(newWallpaper,
                                InputTarget::Flags::DISPATCH_AS_SLIPPERY_ENTER |
                                        InputTarget::Flags::WINDOW_IS_OBSCURED |
                                        InputTarget::Flags::WINDOW_IS_PARTIALLY_OBSCURED,
                                pointerIds);
    }
}
void InputDispatcher::transferWallpaperTouch(ftl::Flags<InputTarget::Flags> oldTargetFlags,
                                             ftl::Flags<InputTarget::Flags> newTargetFlags,
                                             const sp<WindowInfoHandle> fromWindowHandle,
                                             const sp<WindowInfoHandle> toWindowHandle,
                                             TouchState& state,
                                             std::bitset<MAX_POINTER_ID + 1> pointerIds) {
    const bool oldHasWallpaper = oldTargetFlags.test(InputTarget::Flags::FOREGROUND) &&
            fromWindowHandle->getInfo()->inputConfig.test(
                    gui::WindowInfo::InputConfig::DUPLICATE_TOUCH_TO_WALLPAPER);
    const bool newHasWallpaper = newTargetFlags.test(InputTarget::Flags::FOREGROUND) &&
            toWindowHandle->getInfo()->inputConfig.test(
                    gui::WindowInfo::InputConfig::DUPLICATE_TOUCH_TO_WALLPAPER);
    const sp<WindowInfoHandle> oldWallpaper =
            oldHasWallpaper ? state.getWallpaperWindow() : nullptr;
    const sp<WindowInfoHandle> newWallpaper =
            newHasWallpaper ? findWallpaperWindowBelow(toWindowHandle) : nullptr;
    if (oldWallpaper == newWallpaper) {
        return;
    }
    if (oldWallpaper != nullptr) {
        CancelationOptions options(CancelationOptions::Mode::CANCEL_POINTER_EVENTS,
                                   "transferring touch focus to another window");
        state.removeWindowByToken(oldWallpaper->getToken());
        synthesizeCancelationEventsForWindowLocked(oldWallpaper, options);
    }
    if (newWallpaper != nullptr) {
        nsecs_t downTimeInTarget = now();
        ftl::Flags<InputTarget::Flags> wallpaperFlags =
                oldTargetFlags & (InputTarget::Flags::SPLIT | InputTarget::Flags::DISPATCH_AS_IS);
        wallpaperFlags |= InputTarget::Flags::WINDOW_IS_OBSCURED |
                InputTarget::Flags::WINDOW_IS_PARTIALLY_OBSCURED;
        state.addOrUpdateWindow(newWallpaper, wallpaperFlags, pointerIds, downTimeInTarget);
        sp<Connection> wallpaperConnection = getConnectionLocked(newWallpaper->getToken());
        if (wallpaperConnection != nullptr) {
            sp<Connection> toConnection = getConnectionLocked(toWindowHandle->getToken());
            toConnection->inputState.mergePointerStateTo(wallpaperConnection->inputState);
            synthesizePointerDownEventsForConnectionLocked(downTimeInTarget, wallpaperConnection,
                                                           wallpaperFlags);
        }
    }
}
sp<WindowInfoHandle> InputDispatcher::findWallpaperWindowBelow(
        const sp<WindowInfoHandle>& windowHandle) const {
    const std::vector<sp<WindowInfoHandle>>& windowHandles =
            getWindowHandlesLocked(windowHandle->getInfo()->displayId);
    bool foundWindow = false;
    for (const sp<WindowInfoHandle>& otherHandle : windowHandles) {
        if (!foundWindow && otherHandle != windowHandle) {
            continue;
        }
        if (windowHandle == otherHandle) {
            foundWindow = true;
            continue;
        }
        if (otherHandle->getInfo()->inputConfig.test(WindowInfo::InputConfig::IS_WALLPAPER)) {
            return otherHandle;
        }
    }
    return nullptr;
}
}
