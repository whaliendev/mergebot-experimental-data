#define LOG_TAG "InputDispatcher"
#define ATRACE_TAG ATRACE_TAG_INPUT
#define LOG_NDEBUG 1
#define DEBUG_INBOUND_EVENT_DETAILS 0
#define DEBUG_OUTBOUND_EVENT_DETAILS 0
#define DEBUG_DISPATCH_CYCLE 0
#define DEBUG_REGISTRATION 0
#define DEBUG_INJECTION 0
static constexpr bool DEBUG_FOCUS = false;
#define DEBUG_APP_SWITCH 0
#define DEBUG_HOVER 0
#define INDENT "  "
#define INDENT2 "    "
#define INDENT3 "      "
#define INDENT4 "        "
using android::base::StringPrintf;
namespace android::inputdispatcher {
constexpr nsecs_t DEFAULT_INPUT_DISPATCHING_TIMEOUT = 5000 * 1000000LL;
constexpr nsecs_t APP_SWITCH_TIMEOUT = 500 * 1000000LL;
constexpr nsecs_t STALE_EVENT_TIMEOUT = 10000 * 1000000LL;
constexpr nsecs_t STREAM_AHEAD_EVENT_TIMEOUT = 500 * 1000000LL;
constexpr nsecs_t SLOW_EVENT_PROCESSING_WARNING_TIMEOUT = 2000 * 1000000LL;
constexpr std::chrono::milliseconds SLOW_INTERCEPTION_THRESHOLD = 50ms;
constexpr size_t RECENT_QUEUE_MAX_SIZE = 10;
static inline nsecs_t now() {
    return systemTime(SYSTEM_TIME_MONOTONIC);
}
static inline const char* toString(bool value) {
    return value ? "true" : "false";
}
static inline int32_t getMotionEventActionPointerIndex(int32_t action) {
    return (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
}
static bool isValidKeyAction(int32_t action) {
    switch (action) {
        case AKEY_EVENT_ACTION_DOWN:
        case AKEY_EVENT_ACTION_UP:
            return true;
        default:
            return false;
    }
}
static bool validateKeyEvent(int32_t action) {
    if (!isValidKeyAction(action)) {
        ALOGE("Key event has invalid action code 0x%x", action);
        return false;
    }
    return true;
}
static bool isValidMotionAction(int32_t action, int32_t actionButton, int32_t pointerCount) {
    switch (action & AMOTION_EVENT_ACTION_MASK) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
        case AMOTION_EVENT_ACTION_MOVE:
        case AMOTION_EVENT_ACTION_OUTSIDE:
        case AMOTION_EVENT_ACTION_HOVER_ENTER:
        case AMOTION_EVENT_ACTION_HOVER_MOVE:
        case AMOTION_EVENT_ACTION_HOVER_EXIT:
        case AMOTION_EVENT_ACTION_SCROLL:
            return true;
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_UP: {
            int32_t index = getMotionEventActionPointerIndex(action);
            return index >= 0 && index < pointerCount;
        }
        case AMOTION_EVENT_ACTION_BUTTON_PRESS:
        case AMOTION_EVENT_ACTION_BUTTON_RELEASE:
            return actionButton != 0;
        default:
            return false;
    }
}
static bool validateMotionEvent(int32_t action, int32_t actionButton, size_t pointerCount,
                                const PointerProperties* pointerProperties) {
    if (!isValidMotionAction(action, actionButton, pointerCount)) {
        ALOGE("Motion event has invalid action code 0x%x", action);
        return false;
    }
    if (pointerCount < 1 || pointerCount > MAX_POINTERS) {
        ALOGE("Motion event has invalid pointer count %zu; value must be between 1 and %d.",
              pointerCount, MAX_POINTERS);
        return false;
    }
    BitSet32 pointerIdBits;
    for (size_t i = 0; i < pointerCount; i++) {
        int32_t id = pointerProperties[i].id;
        if (id < 0 || id > MAX_POINTER_ID) {
            ALOGE("Motion event has invalid pointer id %d; value must be between 0 and %d", id,
                  MAX_POINTER_ID);
            return false;
        }
        if (pointerIdBits.hasBit(id)) {
            ALOGE("Motion event has duplicate pointer id %d", id);
            return false;
        }
        pointerIdBits.markBit(id);
    }
    return true;
}
static void dumpRegion(std::string& dump, const Region& region) {
    if (region.isEmpty()) {
        dump += "<empty>";
        return;
    }
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
}
template <typename K, typename V>
static V getValueByKey(const std::unordered_map<K, V>& map, K key) {
    auto it = map.find(key);
    return it != map.end() ? it->second : V{};
}
template <typename K, typename V>
static bool removeByValue(std::unordered_map<K, V>& map, const V& value) {
    bool removed = false;
    for (auto it = map.begin(); it != map.end();) {
        if (it->second == value) {
            it = map.erase(it);
            removed = true;
        } else {
            it++;
        }
    }
    return removed;
}
static bool haveSameToken(const sp<InputWindowHandle>& first, const sp<InputWindowHandle>& second) {
    if (first == second) {
        return true;
    }
    if (first == nullptr || second == nullptr) {
        return false;
    }
    return first->getToken() == second->getToken();
}
static bool isStaleEvent(nsecs_t currentTime, const EventEntry& entry) {
    return currentTime - entry.eventTime >= STALE_EVENT_TIMEOUT;
}
static std::unique_ptr<DispatchEntry> createDispatchEntry(const InputTarget& inputTarget,
                                                          EventEntry* eventEntry,
                                                          int32_t inputTargetFlags) {
    if (inputTarget.useDefaultPointerInfo()) {
        const PointerInfo& pointerInfo = inputTarget.getDefaultPointerInfo();
        return std::make_unique<DispatchEntry>(eventEntry,
                                               inputTargetFlags, pointerInfo.xOffset,
                                               pointerInfo.yOffset, inputTarget.globalScaleFactor,
                                               pointerInfo.windowXScale, pointerInfo.windowYScale);
    }
    ALOG_ASSERT(eventEntry->type == EventEntry::Type::MOTION);
    const MotionEntry& motionEntry = static_cast<const MotionEntry&>(*eventEntry);
    PointerCoords pointerCoords[motionEntry.pointerCount];
    const PointerInfo& firstPointerInfo =
            inputTarget.pointerInfos[inputTarget.pointerIds.firstMarkedBit()];
    for (uint32_t pointerIndex = 0; pointerIndex < motionEntry.pointerCount; pointerIndex++) {
        const PointerProperties& pointerProperties = motionEntry.pointerProperties[pointerIndex];
        uint32_t pointerId = uint32_t(pointerProperties.id);
        const PointerInfo& currPointerInfo = inputTarget.pointerInfos[pointerId];
        float scaleXDiff = currPointerInfo.windowXScale / firstPointerInfo.windowXScale;
        float scaleYDiff = currPointerInfo.windowYScale / firstPointerInfo.windowYScale;
        pointerCoords[pointerIndex].copyFrom(motionEntry.pointerCoords[pointerIndex]);
        pointerCoords[pointerIndex].applyOffset(currPointerInfo.xOffset, currPointerInfo.yOffset);
        pointerCoords[pointerIndex].scale(1, scaleXDiff, scaleYDiff);
        pointerCoords[pointerIndex].applyOffset(-firstPointerInfo.xOffset,
                                                -firstPointerInfo.yOffset);
    }
    MotionEntry* combinedMotionEntry =
            new MotionEntry(motionEntry.id, motionEntry.eventTime, motionEntry.deviceId,
                            motionEntry.source, motionEntry.displayId, motionEntry.policyFlags,
                            motionEntry.action, motionEntry.actionButton, motionEntry.flags,
                            motionEntry.metaState, motionEntry.buttonState,
                            motionEntry.classification, motionEntry.edgeFlags,
                            motionEntry.xPrecision, motionEntry.yPrecision,
                            motionEntry.xCursorPosition, motionEntry.yCursorPosition,
                            motionEntry.downTime, motionEntry.pointerCount,
                            motionEntry.pointerProperties, pointerCoords, 0 ,
                            0 );
    if (motionEntry.injectionState) {
        combinedMotionEntry->injectionState = motionEntry.injectionState;
        combinedMotionEntry->injectionState->refCount += 1;
    }
    std::unique_ptr<DispatchEntry> dispatchEntry =
            std::make_unique<DispatchEntry>(combinedMotionEntry,
                                            inputTargetFlags, firstPointerInfo.xOffset,
                                            firstPointerInfo.yOffset, inputTarget.globalScaleFactor,
                                            firstPointerInfo.windowXScale,
                                            firstPointerInfo.windowYScale);
    combinedMotionEntry->release();
    return dispatchEntry;
}
static std::array<uint8_t, 128> getRandomKey() {
    std::array<uint8_t, 128> key;
    if (RAND_bytes(key.data(), key.size()) != 1) {
        LOG_ALWAYS_FATAL("Can't generate HMAC key");
    }
    return key;
}
HmacKeyManager::HmacKeyManager() : mHmacKey(getRandomKey()) {}
std::array<uint8_t, 32> HmacKeyManager::sign(const VerifiedInputEvent& event) const {
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
    std::vector<uint8_t> data;
    const uint8_t* start = reinterpret_cast<const uint8_t*>(&event);
    data.assign(start, start + size);
    return sign(data);
}
std::array<uint8_t, 32> HmacKeyManager::sign(const std::vector<uint8_t>& data) const {
    std::array<uint8_t, 32> hash;
    unsigned int hashLen = 0;
    uint8_t* result = HMAC(EVP_sha256(), mHmacKey.data(), mHmacKey.size(), data.data(), data.size(),
                           hash.data(), &hashLen);
    if (result == nullptr) {
        ALOGE("Could not sign the data using HMAC");
        return INVALID_HMAC;
    }
    if (hashLen != hash.size()) {
        ALOGE("HMAC-SHA256 has unexpected length");
        return INVALID_HMAC;
    }
    return hash;
}
InputDispatcher::InputDispatcher(const sp<InputDispatcherPolicyInterface>& policy)
      : mPolicy(policy),
        mPendingEvent(nullptr),
        mLastDropReason(DropReason::NOT_DROPPED),
        mIdGenerator(IdGenerator::Source::INPUT_DISPATCHER),
        mAppSwitchSawKeyDown(false),
        mAppSwitchDueTime(LONG_LONG_MAX),
        mNextUnblockedEvent(nullptr),
        mDispatchEnabled(false),
        mDispatchFrozen(false),
        mInputFilterEnabled(false),
        mInTouchMode(true),
        mFocusedDisplayId(ADISPLAY_ID_DEFAULT),
        mInputTargetWaitCause(INPUT_TARGET_WAIT_CAUSE_NONE) {
    mLooper = new Looper(false);
    mReporter = createInputReporter();
    mKeyRepeatState.lastKeyEntry = nullptr;
    policy->getDispatcherConfiguration(&mConfig);
}
InputDispatcher::~InputDispatcher() {
    {
        std::scoped_lock _l(mLock);
        resetKeyRepeatLocked();
        releasePendingEventLocked();
        drainInboundQueueLocked();
    }
    while (!mConnectionsByFd.empty()) {
        sp<Connection> connection = mConnectionsByFd.begin()->second;
        unregisterInputChannel(connection->inputChannel);
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
    nsecs_t nextWakeupTime = LONG_LONG_MAX;
    {
        std::scoped_lock _l(mLock);
        mDispatcherIsAlive.notify_all();
        if (!haveCommandsLocked()) {
            dispatchOnceInnerLocked(&nextWakeupTime);
        }
        if (runCommandsLockedInterruptible()) {
            nextWakeupTime = LONG_LONG_MIN;
        }
        if (nextWakeupTime == LONG_LONG_MAX) {
            mDispatcherEnteredIdle.notify_all();
        }
    }
    nsecs_t currentTime = now();
    int timeoutMillis = toMillisecondTimeoutDelay(currentTime, nextWakeupTime);
    mLooper->pollOnce(timeoutMillis);
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
        resetANRTimeoutsLocked();
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
            ConfigurationChangedEntry* typedEntry =
                    static_cast<ConfigurationChangedEntry*>(mPendingEvent);
            done = dispatchConfigurationChangedLocked(currentTime, typedEntry);
            dropReason = DropReason::NOT_DROPPED;
            break;
        }
        case EventEntry::Type::DEVICE_RESET: {
            DeviceResetEntry* typedEntry = static_cast<DeviceResetEntry*>(mPendingEvent);
            done = dispatchDeviceResetLocked(currentTime, typedEntry);
            dropReason = DropReason::NOT_DROPPED;
            break;
        }
        case EventEntry::Type::FOCUS: {
            FocusEntry* typedEntry = static_cast<FocusEntry*>(mPendingEvent);
            dispatchFocusLocked(currentTime, typedEntry);
            done = true;
            dropReason = DropReason::NOT_DROPPED;
            break;
        }
        case EventEntry::Type::KEY: {
            KeyEntry* typedEntry = static_cast<KeyEntry*>(mPendingEvent);
            if (isAppSwitchDue) {
                if (isAppSwitchKeyEvent(*typedEntry)) {
                    resetPendingAppSwitchLocked(true);
                    isAppSwitchDue = false;
                } else if (dropReason == DropReason::NOT_DROPPED) {
                    dropReason = DropReason::APP_SWITCH;
                }
            }
            if (dropReason == DropReason::NOT_DROPPED && isStaleEvent(currentTime, *typedEntry)) {
                dropReason = DropReason::STALE;
            }
            if (dropReason == DropReason::NOT_DROPPED && mNextUnblockedEvent) {
                dropReason = DropReason::BLOCKED;
            }
            done = dispatchKeyLocked(currentTime, typedEntry, &dropReason, nextWakeupTime);
            break;
        }
        case EventEntry::Type::MOTION: {
            MotionEntry* typedEntry = static_cast<MotionEntry*>(mPendingEvent);
            if (dropReason == DropReason::NOT_DROPPED && isAppSwitchDue) {
                dropReason = DropReason::APP_SWITCH;
            }
            if (dropReason == DropReason::NOT_DROPPED && isStaleEvent(currentTime, *typedEntry)) {
                dropReason = DropReason::STALE;
            }
            if (dropReason == DropReason::NOT_DROPPED && mNextUnblockedEvent) {
                dropReason = DropReason::BLOCKED;
            }
            done = dispatchMotionLocked(currentTime, typedEntry, &dropReason, nextWakeupTime);
            break;
        }
    }
    if (done) {
        if (dropReason != DropReason::NOT_DROPPED) {
            dropInboundEventLocked(*mPendingEvent, dropReason);
        }
        mLastDropReason = dropReason;
        releasePendingEventLocked();
        *nextWakeupTime = LONG_LONG_MIN;
    }
}
bool InputDispatcher::enqueueInboundEventLocked(EventEntry* entry) {
    bool needWake = mInboundQueue.empty();
    mInboundQueue.push_back(entry);
    traceInboundQueueLengthLocked();
    switch (entry->type) {
        case EventEntry::Type::KEY: {
            const KeyEntry& keyEntry = static_cast<const KeyEntry&>(*entry);
            if (isAppSwitchKeyEvent(keyEntry)) {
                if (keyEntry.action == AKEY_EVENT_ACTION_DOWN) {
                    mAppSwitchSawKeyDown = true;
                } else if (keyEntry.action == AKEY_EVENT_ACTION_UP) {
                    if (mAppSwitchSawKeyDown) {
#if DEBUG_APP_SWITCH
                        ALOGD("App switch is pending!");
#endif
                        mAppSwitchDueTime = keyEntry.eventTime + APP_SWITCH_TIMEOUT;
                        mAppSwitchSawKeyDown = false;
                        needWake = true;
                    }
                }
            }
            break;
        }
        case EventEntry::Type::MOTION: {
            MotionEntry* motionEntry = static_cast<MotionEntry*>(entry);
            if (motionEntry->action == AMOTION_EVENT_ACTION_DOWN &&
                (motionEntry->source & AINPUT_SOURCE_CLASS_POINTER) &&
                mInputTargetWaitCause == INPUT_TARGET_WAIT_CAUSE_APPLICATION_NOT_READY &&
                mInputTargetWaitApplicationToken != nullptr) {
                int32_t displayId = motionEntry->displayId;
                int32_t x =
                        int32_t(motionEntry->pointerCoords[0].getAxisValue(AMOTION_EVENT_AXIS_X));
                int32_t y =
                        int32_t(motionEntry->pointerCoords[0].getAxisValue(AMOTION_EVENT_AXIS_Y));
                sp<InputWindowHandle> touchedWindowHandle =
                        findTouchedWindowAtLocked(displayId, x, y);
                if (touchedWindowHandle != nullptr &&
                    touchedWindowHandle->getApplicationToken() !=
                            mInputTargetWaitApplicationToken) {
                    mNextUnblockedEvent = motionEntry;
                    needWake = true;
                }
            }
            break;
        }
        case EventEntry::Type::CONFIGURATION_CHANGED:
        case EventEntry::Type::DEVICE_RESET:
        case EventEntry::Type::FOCUS: {
            break;
        }
    }
    return needWake;
}
void InputDispatcher::addRecentEventLocked(EventEntry* entry) {
    entry->refCount += 1;
    mRecentQueue.push_back(entry);
    if (mRecentQueue.size() > RECENT_QUEUE_MAX_SIZE) {
        mRecentQueue.front()->release();
        mRecentQueue.pop_front();
    }
}
sp<InputWindowHandle> InputDispatcher::findTouchedWindowAtLocked(int32_t displayId, int32_t x,
                                                                 int32_t y, bool addOutsideTargets,
                                                                 bool addPortalWindows) {
    const std::vector<sp<InputWindowHandle>> windowHandles = getWindowHandlesLocked(displayId);
    for (const sp<InputWindowHandle>& windowHandle : windowHandles) {
        const InputWindowInfo* windowInfo = windowHandle->getInfo();
        if (windowInfo->displayId == displayId) {
            int32_t flags = windowInfo->layoutParamsFlags;
            if (windowInfo->visible) {
                if (!(flags & InputWindowInfo::FLAG_NOT_TOUCHABLE)) {
                    bool isTouchModal = (flags &
                                         (InputWindowInfo::FLAG_NOT_FOCUSABLE |
                                          InputWindowInfo::FLAG_NOT_TOUCH_MODAL)) == 0;
                    if (isTouchModal || windowInfo->touchableRegionContainsPoint(x, y)) {
                        int32_t portalToDisplayId = windowInfo->portalToDisplayId;
                        if (portalToDisplayId != ADISPLAY_ID_NONE &&
                            portalToDisplayId != displayId) {
                            if (addPortalWindows) {
                                mTempTouchState.addPortalWindow(windowHandle);
                            }
                            return findTouchedWindowAtLocked(portalToDisplayId, x, y,
                                                             addOutsideTargets, addPortalWindows);
                        }
                        return windowHandle;
                    }
                }
                if (addOutsideTargets && (flags & InputWindowInfo::FLAG_WATCH_OUTSIDE_TOUCH)) {
                    mTempTouchState.addOrUpdateWindow(windowHandle,
                                                      InputTarget::FLAG_DISPATCH_AS_OUTSIDE,
                                                      BitSet32(0));
                }
            }
        }
    }
    return nullptr;
}
std::vector<TouchedMonitor> InputDispatcher::findTouchedGestureMonitorsLocked(
        int32_t displayId, const std::vector<sp<InputWindowHandle>>& portalWindows) {
    std::vector<TouchedMonitor> touchedMonitors;
    std::vector<Monitor> monitors = getValueByKey(mGestureMonitorsByDisplay, displayId);
    addGestureMonitors(monitors, touchedMonitors);
    for (const sp<InputWindowHandle>& portalWindow : portalWindows) {
        const InputWindowInfo* windowInfo = portalWindow->getInfo();
        monitors = getValueByKey(mGestureMonitorsByDisplay, windowInfo->portalToDisplayId);
        addGestureMonitors(monitors, touchedMonitors, -windowInfo->frameLeft,
                           -windowInfo->frameTop);
    }
    return touchedMonitors;
}
void InputDispatcher::addGestureMonitors(const std::vector<Monitor>& monitors,
                                         std::vector<TouchedMonitor>& outTouchedMonitors,
                                         float xOffset, float yOffset) {
    if (monitors.empty()) {
        return;
    }
    outTouchedMonitors.reserve(monitors.size() + outTouchedMonitors.size());
    for (const Monitor& monitor : monitors) {
        outTouchedMonitors.emplace_back(monitor, xOffset, yOffset);
    }
}
void InputDispatcher::dropInboundEventLocked(const EventEntry& entry, DropReason dropReason) {
    const char* reason;
    switch (dropReason) {
        case DropReason::POLICY:
#if DEBUG_INBOUND_EVENT_DETAILS
            ALOGD("Dropped event because policy consumed it.");
#endif
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
        case DropReason::NOT_DROPPED: {
            LOG_ALWAYS_FATAL("Should not be dropping a NOT_DROPPED event");
            return;
        }
    }
    switch (entry.type) {
        case EventEntry::Type::KEY: {
            CancelationOptions options(CancelationOptions::CANCEL_NON_POINTER_EVENTS, reason);
            synthesizeCancelationEventsForAllConnectionsLocked(options);
            break;
        }
        case EventEntry::Type::MOTION: {
            const MotionEntry& motionEntry = static_cast<const MotionEntry&>(entry);
            if (motionEntry.source & AINPUT_SOURCE_CLASS_POINTER) {
                CancelationOptions options(CancelationOptions::CANCEL_POINTER_EVENTS, reason);
                synthesizeCancelationEventsForAllConnectionsLocked(options);
            } else {
                CancelationOptions options(CancelationOptions::CANCEL_NON_POINTER_EVENTS, reason);
                synthesizeCancelationEventsForAllConnectionsLocked(options);
            }
            break;
        }
        case EventEntry::Type::FOCUS:
        case EventEntry::Type::CONFIGURATION_CHANGED:
        case EventEntry::Type::DEVICE_RESET: {
            LOG_ALWAYS_FATAL("Should not drop %s events", EventEntry::typeToString(entry.type));
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
    return mAppSwitchDueTime != LONG_LONG_MAX;
}
void InputDispatcher::resetPendingAppSwitchLocked(bool handled) {
    mAppSwitchDueTime = LONG_LONG_MAX;
#if DEBUG_APP_SWITCH
    if (handled) {
        ALOGD("App switch has arrived.");
    } else {
        ALOGD("App switch was abandoned.");
    }
#endif
}
bool InputDispatcher::haveCommandsLocked() const {
    return !mCommandQueue.empty();
}
bool InputDispatcher::runCommandsLockedInterruptible() {
    if (mCommandQueue.empty()) {
        return false;
    }
    do {
        std::unique_ptr<CommandEntry> commandEntry = std::move(mCommandQueue.front());
        mCommandQueue.pop_front();
        Command command = commandEntry->command;
        command(*this, commandEntry.get());
        commandEntry->connection.clear();
    } while (!mCommandQueue.empty());
    return true;
}
void InputDispatcher::postCommandLocked(std::unique_ptr<CommandEntry> commandEntry) {
    mCommandQueue.push_back(std::move(commandEntry));
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
void InputDispatcher::reportTouchEventForStatistics(const MotionEntry& motionEntry)
        REQUIRES(mLock) {
        const bool reportForStatistics = (motionEntry.source == AINPUT_SOURCE_TOUCHSCREEN) &&
            !(motionEntry.isSynthesized()) && !mInputFilterEnabled;
        if (!reportForStatistics) {
        return;
        }
        if (mTouchStatistics.shouldReport()) {
        android::util::stats_write(android::util::TOUCH_EVENT_REPORTED, mTouchStatistics.getMin(),
                                   mTouchStatistics.getMax(), mTouchStatistics.getMean(),
                                   mTouchStatistics.getStDev(), mTouchStatistics.getCount());
        mTouchStatistics.reset();
        }
        const float latencyMicros = nanoseconds_to_microseconds(now() - motionEntry.eventTime);
        mTouchStatistics.addValue(latencyMicros);
        }
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
bool InputDispatcher::waitForIdle() {
    constexpr std::chrono::duration TIMEOUT = 100ms;
    std::unique_lock lock(mLock);
    mLooper->wake();
    std::cv_status result = mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);
    return result == std::cv_status::no_timeout;
}
}
