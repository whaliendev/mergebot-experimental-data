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
    void onWindowInfosChanged(const std::vector<android::gui::WindowInfo>& windowInfos,
                              const std::vector<android::gui::DisplayInfo>& displayInfos);
    void cancelCurrentTouch() override;
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
    std::map<int32_t , bool > mTouchModePerDisplay REQUIRES(mLock);
    std::deque<std::shared_ptr<EventEntry>> mInboundQueue GUARDED_BY(mLock);
    std::deque<std::shared_ptr<EventEntry>> mRecentQueue GUARDED_BY(mLock);
    using Command = std::function<void()>;
    std::deque<Command> mCommandQueue GUARDED_BY(mLock);
    DropReason mLastDropReason GUARDED_BY(mLock);
    const IdGenerator mIdGenerator;
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
    bool mAppSwitchSawKeyDown GUARDED_BY(mLock);
    nsecs_t mAppSwitchDueTime GUARDED_BY(mLock);
    bool isAppSwitchKeyEvent(const KeyEntry& keyEntry);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
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
    std::condition_variable mInjectionResultAvailable;
    void setInjectionResult(EventEntry& entry,
                            android::os::InputEventInjectionResult injectionResult);
    REQUIRES(mLock);
    REQUIRES(mLock);
    std::condition_variable mInjectionSyncFinished;
    void incrementPendingForegroundDispatches(EventEntry& entry);
    void decrementPendingForegroundDispatches(EventEntry& entry);
    struct KeyRepeatState {
        std::shared_ptr<KeyEntry> lastKeyEntry;
        nsecs_t nextRepeatTime;
    } mKeyRepeatState GUARDED_BY(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
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
    std::unordered_map<KeyReplacement, int32_t, KeyReplacementHash> mReplacedKeys GUARDED_BY(mLock);
    void accelerateMetaShortcuts(const int32_t deviceId, const int32_t action, int32_t& keyCode,
                                 int32_t& metaState);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
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
    std::unordered_map<int32_t , android::gui::DisplayInfo> mDisplayInfos
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
    int32_t mFocusedDisplayId GUARDED_BY(mLock);
    FocusResolver mFocusResolver GUARDED_BY(mLock);
    PointerCaptureRequest mCurrentPointerCaptureRequest GUARDED_BY(mLock);
    sp<IBinder> mWindowTokenWithPointerCapture GUARDED_BY(mLock);
    std::vector<int32_t> mIneligibleDisplaysForPointerCapture GUARDED_BY(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    std::string mLastAnrState GUARDED_BY(mLock);
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
    std::optional<nsecs_t> mNoFocusedWindowTimeoutTime GUARDED_BY(mLock);
    const std::chrono::nanoseconds mStaleEventTimeout;
    bool isStaleEvent(nsecs_t currentTime, const EventEntry& entry);
    REQUIRES(mLock);
    REQUIRES(mLock);
    std::optional<nsecs_t> mKeyIsWaitingForEventsTimeout GUARDED_BY(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    std::shared_ptr<InputApplicationHandle> mAwaitedFocusedApplication GUARDED_BY(mLock);
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
    AnrTracker mAnrTracker GUARDED_BY(mLock);
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
            const CancelationOptions& options)
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
}
#endif
