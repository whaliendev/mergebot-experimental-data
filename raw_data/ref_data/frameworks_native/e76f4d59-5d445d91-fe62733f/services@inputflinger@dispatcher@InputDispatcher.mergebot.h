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

#ifndef _UI_INPUT_DISPATCHER_H
#define _UI_INPUT_DISPATCHER_H

#include "AnrTracker.h"
#include "CancelationOptions.h"
#include "DragState.h"
#include "Entry.h"
#include "FocusResolver.h"
#include "InjectionState.h"
#include "InputDispatcherConfiguration.h"
#include "InputDispatcherInterface.h"
#include "InputDispatcherPolicyInterface.h"
#include "InputState.h"
#include "InputTarget.h"
#include "InputThread.h"
#include "LatencyAggregator.h"
#include "LatencyTracker.h"
#include "Monitor.h"
#include "TouchState.h"
#include "TouchedWindow.h"
#include <attestation/HmacKeyManager.h>
#include <gui/InputApplication.h>
#include <gui/WindowInfo.h>
#include <input/Input.h>
#include <input/InputTransport.h>
#include <limits.h>
#include <stddef.h>
#include <ui/Region.h>
#include <unistd.h>
#include <utils/BitSet.h>
#include <utils/Looper.h>
#include <utils/Timers.h>
#include <utils/threads.h>
#include <condition_variable>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <InputListener.h>
#include <InputReporterInterface.h>
#include <gui/WindowInfosListener.h>

namespace android::inputdispatcher {

class Connection;

/* Dispatches events to input targets.  Some functions of the input dispatcher, such as
 * identifying input targets, are controlled by a separate policy object.
 *
 * IMPORTANT INVARIANT:
 *     Because the policy can potentially block or cause re-entrance into the input dispatcher,
 *     the input dispatcher never calls into the policy while holding its internal locks.
 *     The implementation is also carefully designed to recover from scenarios such as an
 *     input channel becoming unregistered while identifying input targets or processing timeouts.
 *
 *     Methods marked 'Locked' must be called with the lock acquired.
 *
 *     Methods marked 'LockedInterruptible' must be called with the lock acquired but
 *     may during the course of their execution release the lock, call into the policy, and
 *     then reacquire the lock.  The caller is responsible for recovering gracefully.
 *
 *     A 'LockedInterruptible' method may called a 'Locked' method, but NOT vice-versa.
 */
class InputDispatcher : public android::InputDispatcherInterface {
public:
    static constexpr bool kDefaultInTouchMode = true;

    explicit InputDispatcher(const sp<InputDispatcherPolicyInterface>& policy);
    explicit InputDispatcher(const sp<InputDispatcherPolicyInterface>& policy,
                             std::chrono::nanoseconds staleEventTimeout);
    ~InputDispatcher() override;

    void dump(std::string& dump) override;
    void monitor() override;
    bool waitForIdle() override;
    status_t start() override;
    status_t stop() override;

    void notifyConfigurationChanged(const NotifyConfigurationChangedArgs* args) override;
    void notifyKey(const NotifyKeyArgs* args) override;
    void notifyMotion(const NotifyMotionArgs* args) override;
    void notifySwitch(const NotifySwitchArgs* args) override;
    void notifySensor(const NotifySensorArgs* args) override;
    void notifyVibratorState(const NotifyVibratorStateArgs* args) override;
    void notifyDeviceReset(const NotifyDeviceResetArgs* args) override;
    void notifyPointerCaptureChanged(const NotifyPointerCaptureChangedArgs* args) override;

    android::os::InputEventInjectionResult injectInputEvent(
            const InputEvent* event, std::optional<int32_t> targetUid,
            android::os::InputEventInjectionSync syncMode, std::chrono::milliseconds timeout,
            uint32_t policyFlags) override;

    std::unique_ptr<VerifiedInputEvent> verifyInputEvent(const InputEvent& event) override;

    void setInputWindows(
            const std::unordered_map<int32_t, std::vector<sp<android::gui::WindowInfoHandle>>>&
                    handlesPerDisplay) override;
    void setFocusedApplication(
            int32_t displayId,
            const std::shared_ptr<InputApplicationHandle>& inputApplicationHandle) override;
    void setFocusedDisplay(int32_t displayId) override;
    void setInputDispatchMode(bool enabled, bool frozen) override;
    void setInputFilterEnabled(bool enabled) override;
    bool setInTouchMode(bool inTouchMode, int32_t pid, int32_t uid, bool hasPermission,
                        int32_t displayId) override;
    void setMaximumObscuringOpacityForTouch(float opacity) override;
    bool transferTouchFocus(const sp<IBinder>& fromToken, const sp<IBinder>& toToken,
                            bool isDragDrop = false) override;
    bool transferTouch(const sp<IBinder>& destChannelToken, int32_t displayId) override;

    base::Result<std::unique_ptr<InputChannel>> createInputChannel(
            const std::string& name) override;
    void setFocusedWindow(const android::gui::FocusRequest&) override;
    base::Result<std::unique_ptr<InputChannel>> createInputMonitor(int32_t displayId,
                                                                   const std::string& name,
                                                                   int32_t pid) override;
    status_t removeInputChannel(const sp<IBinder>& connectionToken) override;
    status_t pilferPointers(const sp<IBinder>& token) override;
    void requestPointerCapture(const sp<IBinder>& windowToken, bool enabled) override;
    bool flushSensor(int deviceId, InputDeviceSensorType sensorType) override;
    void setDisplayEligibilityForPointerCapture(int displayId, bool isEligible) override;

    std::array<uint8_t, 32> sign(const VerifiedInputEvent& event) const;

    void displayRemoved(int32_t displayId) override;

    // Public because it's also used by tests to simulate the WindowInfosListener callback
    void onWindowInfosChanged(const std::vector<android::gui::WindowInfo>& windowInfos,
                              const std::vector<android::gui::DisplayInfo>& displayInfos);

    void cancelCurrentTouch() override;

    // Public to allow tests to verify that a Monitor can get ANR.
    void setMonitorDispatchingTimeoutForTest(std::chrono::nanoseconds timeout);

private:
    enum class DropReason {
        NOT_DROPPED,
        POLICY,
        APP_SWITCH,
        DISABLED,
        BLOCKED,
        STALE,
        NO_POINTER_CAPTURE,
    };

    std::unique_ptr<InputThread> mThread;

    sp<InputDispatcherPolicyInterface> mPolicy;
    android::InputDispatcherConfiguration mConfig;

    std::mutex mLock;

    std::condition_variable mDispatcherIsAlive;
    std::condition_variable mDispatcherEnteredIdle;

    sp<Looper> mLooper;

    std::shared_ptr<EventEntry> mPendingEvent GUARDED_BY(mLock);
    // This map is not really needed, but it helps a lot with debugging (dumpsys input).
    // In the java layer, touch mode states are spread across multiple DisplayContent objects,
    // making harder to snapshot and retrieve them.
    std::map<int32_t /*displayId*/, bool /*inTouchMode*/> mTouchModePerDisplay REQUIRES(mLock);

    std::deque<std::shared_ptr<EventEntry>> mInboundQueue GUARDED_BY(mLock);
    std::deque<std::shared_ptr<EventEntry>> mRecentQueue GUARDED_BY(mLock);
    // A command entry captures state and behavior for an action to be performed in the
    // dispatch loop after the initial processing has taken place.  It is essentially
    // a kind of continuation used to postpone sensitive policy interactions to a point
    // in the dispatch loop where it is safe to release the lock (generally after finishing
    // the critical parts of the dispatch cycle).
    //
    // The special thing about commands is that they can voluntarily release and reacquire
    // the dispatcher lock at will.  Initially when the command starts running, the
    // dispatcher lock is held.  However, if the command needs to call into the policy to
    // do some work, it can release the lock, do the work, then reacquire the lock again
    // before returning.
    //
    // This mechanism is a bit clunky but it helps to preserve the invariant that the dispatch
    // never calls into the policy while holding its lock.
    //
    // Commands are called with the lock held, but they can release and re-acquire the lock from
    // within.
    using Command = std::function<void()>;
    std::deque<Command> mCommandQueue GUARDED_BY(mLock);
    DropReason mLastDropReason GUARDED_BY(mLock);
    const IdGenerator mIdGenerator;

    // With each iteration, InputDispatcher nominally processes one queued event,
    // a timeout, or a response from an input consumer.
    // This method should only be called on the input dispatcher's own thread.
    void dispatchOnce();

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    // App switch latency optimization.
    bool mAppSwitchSawKeyDown GUARDED_BY(mLock);
    nsecs_t mAppSwitchDueTime GUARDED_BY(mLock);
    bool isAppSwitchKeyEvent(const KeyEntry& keyEntry);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    // Blocked event latency optimization.  Drops old events when the user intends
    // to transfer focus to a new application.
    std::shared_ptr<EventEntry> mNextUnblockedEvent GUARDED_BY(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    template <typename T>
    struct StrongPointerHash {
        std::size_t operator()(const sp<T>& b) const { return std::hash<T*>{}(b.get()); }
    };

    // All registered connections mapped by input channel token.
    std::unordered_map<sp<IBinder>, sp<Connection>, StrongPointerHash<IBinder>> mConnectionsByToken
            GUARDED_BY(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    GUARDED_BY(mLock);
    GUARDED_BY(mLock);
    const HmacKeyManager mHmacKeyManager;
    const std::array<uint8_t, 32> getSignature(const MotionEntry& motionEntry,
                                               const DispatchEntry& dispatchEntry) const;
    const std::array<uint8_t, 32> getSignature(const KeyEntry& keyEntry,
                                               const DispatchEntry& dispatchEntry) const;

    // Event injection and synchronization.
    std::condition_variable mInjectionResultAvailable;
    void setInjectionResult(EventEntry& entry,
                            android::os::InputEventInjectionResult injectionResult);
    REQUIRES(mLock);

    REQUIRES(mLock);

    std::condition_variable mInjectionSyncFinished;
    void incrementPendingForegroundDispatches(EventEntry& entry);
    void decrementPendingForegroundDispatches(EventEntry& entry);

    // Key repeat tracking.
    struct KeyRepeatState {
        std::shared_ptr<KeyEntry> lastKeyEntry; // or null if no repeat
        nsecs_t nextRepeatTime;
    } mKeyRepeatState GUARDED_BY(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    // Key replacement tracking
    struct KeyReplacement {
        int32_t keyCode;
        int32_t deviceId;
        bool operator==(const KeyReplacement& rhs) const {
            return keyCode == rhs.keyCode && deviceId == rhs.deviceId;
        }
    };
    struct KeyReplacementHash {
        size_t operator()(const KeyReplacement& key) const {
            return std::hash<int32_t>()(key.keyCode) ^ (std::hash<int32_t>()(key.deviceId) << 1);
        }
    };
    // Maps the key code replaced, device id tuple to the key code it was replaced with
    std::unordered_map<KeyReplacement, int32_t, KeyReplacementHash> mReplacedKeys GUARDED_BY(mLock);
    // Process certain Meta + Key combinations
    void accelerateMetaShortcuts(const int32_t deviceId, const int32_t action, int32_t& keyCode,
                                 int32_t& metaState);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    // The dispatching timeout to use for Monitors.
    std::chrono::nanoseconds mMonitorDispatchingTimeout GUARDED_BY(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    // Dispatch state.
    bool mDispatchEnabled GUARDED_BY(mLock);
    bool mDispatchFrozen GUARDED_BY(mLock);
    bool mInputFilterEnabled GUARDED_BY(mLock);
    GUARDED_BY(mLock);
    float mMaximumObscuringOpacityForTouch GUARDED_BY(mLock);
    GUARDED_BY(mLock);
    class DispatcherWindowListener : public gui::WindowInfosListener {
    public:
        explicit DispatcherWindowListener(InputDispatcher& dispatcher) : mDispatcher(dispatcher){};
        void onWindowInfosChanged(
                const std::vector<android::gui::WindowInfo>& windowInfos,
                const std::vector<android::gui::DisplayInfo>& displayInfos) override;

    private:
        InputDispatcher& mDispatcher;
    };
    sp<gui::WindowInfosListener> mWindowInfoListener;

    GUARDED_BY(mLock);
    GUARDED_BY(mLock);
    std::unordered_map<int32_t /*displayId*/, android::gui::DisplayInfo> mDisplayInfos
            GUARDED_BY(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    std::unordered_map<int32_t, TouchState> mTouchStatesByDisplay GUARDED_BY(mLock);
    GUARDED_BY(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    GUARDED_BY(mLock);
    GUARDED_BY(mLock);
    // Top focused display.
    int32_t mFocusedDisplayId GUARDED_BY(mLock);
    // Keeps track of the focused window per display and determines focus changes.
    FocusResolver mFocusResolver GUARDED_BY(mLock);
    // The enabled state of this request is true iff the focused window on the focused display has
    // requested Pointer Capture. This request also contains the sequence number associated with the
    // current request. The state of this variable should always be in sync with the state of
    // Pointer Capture in the policy, and is only updated through setPointerCaptureLocked(request).
    PointerCaptureRequest mCurrentPointerCaptureRequest GUARDED_BY(mLock);
    // The window token that has Pointer Capture.
    // This should be in sync with PointerCaptureChangedEvents dispatched to the input channel.
    sp<IBinder> mWindowTokenWithPointerCapture GUARDED_BY(mLock);
    // Displays that are ineligible for pointer capture.
    // TODO(b/214621487): Remove or move to a display flag.
    std::vector<int32_t> mIneligibleDisplaysForPointerCapture GUARDED_BY(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    // Dispatcher state at time of last ANR.
    std::string mLastAnrState GUARDED_BY(mLock);
    // The connection tokens of the channels that the user last interacted (used for debugging and
    // when switching touch mode state).
    std::unordered_set<sp<IBinder>, StrongPointerHash<IBinder>> mInteractionConnectionTokens
            GUARDED_BY(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    void logOutboundKeyDetails(const char* prefix, const KeyEntry& entry);
    void logOutboundMotionDetails(const char* prefix, const MotionEntry& entry);

    /**
     * This field is set if there is no focused window, and we have an event that requires
     * a focused window to be dispatched (for example, a KeyEvent).
     * When this happens, we will wait until *mNoFocusedWindowTimeoutTime before
     * dropping the event and raising an ANR for that application.
     * This is useful if an application is slow to add a focused window.
     */
    std::optional<nsecs_t> mNoFocusedWindowTimeoutTime GUARDED_BY(mLock);
    // Amount of time to allow for an event to be dispatched (measured since its eventTime)
    // before considering it stale and dropping it.
    const std::chrono::nanoseconds mStaleEventTimeout;
    bool isStaleEvent(nsecs_t currentTime, const EventEntry& entry);

    REQUIRES(mLock);

    REQUIRES(mLock);

    /**
     * Time to stop waiting for the events to be processed while trying to dispatch a key.
     * When this time expires, we just send the pending key event to the currently focused window,
     * without waiting on other events to be processed first.
     */
    std::optional<nsecs_t> mKeyIsWaitingForEventsTimeout GUARDED_BY(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    /**
     * The focused application at the time when no focused window was present.
     * Used to raise an ANR when we have no focused window.
     */
    std::shared_ptr<InputApplicationHandle> mAwaitedFocusedApplication GUARDED_BY(mLock);
    /**
     * The displayId that the focused application is associated with.
     */
    int32_t mAwaitedApplicationDisplayId GUARDED_BY(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    // Optimization: AnrTracker is used to quickly find which connection is due for a timeout next.
    // AnrTracker must be kept in-sync with all responsive connection.waitQueues.
    // If a connection is not responsive, then the entries should not be added to the AnrTracker.
    // Once a connection becomes unresponsive, its entries are removed from AnrTracker to
    // prevent unneeded wakeups.
    AnrTracker mAnrTracker GUARDED_BY(mLock);
    // Contains the last window which received a hover event.
    sp<android::gui::WindowInfoHandle> mLastHoverWindowHandle GUARDED_BY(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    bool shouldSplitTouch(const TouchState& touchState, const MotionEntry& entry) const;
    REQUIRES(mLock);

    int32_t getTargetDisplayId(const EventEntry& entry);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    struct TouchOcclusionInfo {
        bool hasBlockingOcclusion;
        float obscuringOpacity;
        std::string obscuringPackage;
        int32_t obscuringUid;
        std::vector<std::string> debugInfo;
    };

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    std::string dumpWindowForTouchOcclusion(const android::gui::WindowInfo* info,
                                            bool isTouchWindow) const;
    std::string getApplicationWindowLabel(const InputApplicationHandle* applicationHandle,
                                          const sp<android::gui::WindowInfoHandle>& windowHandle);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    status_t publishMotionEvent(Connection& connection, DispatchEntry& dispatchEntry) const;
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    void drainDispatchQueue(std::deque<DispatchEntry*>& queue);
    void releaseDispatchEntry(DispatchEntry* dispatchEntry);
    int handleReceiveCallback(int events, sp<IBinder> connectionToken);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    void synthesizeCancelationEventsForWindowLocked(
            const sp<android::gui::WindowInfoHandle>& windowHandle,
            const CancelationOptions& options) // Splitting motion events across windows. When
                                               // splitting motion event for a target,
            // splitDownTime refers to the time of first 'down' event on that particular target
            std::unique_ptr<MotionEntry> splitMotionEvent(const MotionEntry& originalMotionEntry,
                                                          BitSet32 pointerIds,
                                                          nsecs_t splitDownTime);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);
    REQUIRES(mLock);
    void dumpMonitors(std::string& dump, const std::vector<Monitor>& monitors);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    REQUIRES(mLock);

    // Statistics gathering.
    LatencyAggregator mLatencyAggregator GUARDED_BY(mLock);
    LatencyTracker mLatencyTracker GUARDED_BY(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    void traceOutboundQueueLength(const Connection& connection);
    void traceWaitQueueLength(const Connection& connection);

    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);

    REQUIRES(mLock);

    sp<InputReporterInterface> mReporter;

    REQUIRES(mLock);
    REQUIRES(mLock);

    void slipWallpaperTouch(int32_t targetFlags,
                            const sp<android::gui::WindowInfoHandle>& oldWindowHandle,
                            const sp<android::gui::WindowInfoHandle>& newWindowHandle,
                            TouchState& state, const BitSet32& pointerIds) REQUIRES(mLock);
    void transferWallpaperTouch(int32_t oldTargetFlags, int32_t newTargetFlags,
                                const sp<android::gui::WindowInfoHandle> fromWindowHandle,
                                const sp<android::gui::WindowInfoHandle> toWindowHandle,
                                TouchState& state, const BitSet32& pointerIds) REQUIRES(mLock);

    sp<android::gui::WindowInfoHandle> findWallpaperWindowBelow(
            const sp<android::gui::WindowInfoHandle>& windowHandle) const REQUIRES(mLock);
};

} // namespace android::inputdispatcher

#endif // _UI_INPUT_DISPATCHER_H