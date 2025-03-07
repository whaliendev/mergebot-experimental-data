       
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
    std::deque<std::shared_ptr<EventEntry>> mInboundQueue GUARDED_BY(mLock);
    std::deque<std::shared_ptr<EventEntry>> mRecentQueue GUARDED_BY(mLock);
    using Command = std::function<void()>;
    std::deque<Command> mCommandQueue GUARDED_BY(mLock);
    DropReason mLastDropReason GUARDED_BY(mLock);
    const IdGenerator mIdGenerator;
    void dispatchOnce();
    void dispatchOnceInnerLocked(nsecs_t* nextWakeupTime) REQUIRES(mLock);
    bool enqueueInboundEventLocked(std::unique_ptr<EventEntry> entry) REQUIRES(mLock);
    void dropInboundEventLocked(const EventEntry& entry, DropReason dropReason) REQUIRES(mLock);
    void enqueueFocusEventLocked(const sp<IBinder>& windowToken, bool hasFocus,
                                 const std::string& reason) REQUIRES(mLock);
    void enqueueDragEventLocked(const sp<android::gui::WindowInfoHandle>& windowToken,
                                bool isExiting, const int32_t rawX, const int32_t rawY)
            REQUIRES(mLock);
    void addRecentEventLocked(std::shared_ptr<EventEntry> entry) REQUIRES(mLock);
    bool mAppSwitchSawKeyDown GUARDED_BY(mLock);
    nsecs_t mAppSwitchDueTime GUARDED_BY(mLock);
    bool isAppSwitchKeyEvent(const KeyEntry& keyEntry);
    bool isAppSwitchPendingLocked() REQUIRES(mLock);
    void resetPendingAppSwitchLocked(bool handled) REQUIRES(mLock);
    std::shared_ptr<EventEntry> mNextUnblockedEvent GUARDED_BY(mLock);
    sp<android::gui::WindowInfoHandle> findTouchedWindowAtLocked(
            int32_t displayId, int32_t x, int32_t y, TouchState* touchState, bool isStylus = false,
            bool addOutsideTargets = false, bool ignoreDragWindow = false) const REQUIRES(mLock);
    std::vector<sp<android::gui::WindowInfoHandle>> findTouchedSpyWindowsAtLocked(
            int32_t displayId, int32_t x, int32_t y, bool isStylus) const REQUIRES(mLock);
    sp<android::gui::WindowInfoHandle> findTouchedForegroundWindowLocked(int32_t displayId) const
            REQUIRES(mLock);
    sp<Connection> getConnectionLocked(const sp<IBinder>& inputConnectionToken) const
            REQUIRES(mLock);
    std::string getConnectionNameLocked(const sp<IBinder>& connectionToken) const REQUIRES(mLock);
    void removeConnectionLocked(const sp<Connection>& connection) REQUIRES(mLock);
    status_t pilferPointersLocked(const sp<IBinder>& token) REQUIRES(mLock);
    template <typename T>
    struct StrongPointerHash {
        std::size_t operator()(const sp<T>& b) const { return std::hash<T*>{}(b.get()); }
    };
    std::unordered_map<sp<IBinder>, sp<Connection>, StrongPointerHash<IBinder>> mConnectionsByToken
            GUARDED_BY(mLock);
    std::optional<int32_t> findMonitorPidByTokenLocked(const sp<IBinder>& token) REQUIRES(mLock);
    std::unordered_map<int32_t, std::vector<Monitor>> mGlobalMonitorsByDisplay GUARDED_BY(mLock);
    const HmacKeyManager mHmacKeyManager;
    const std::array<uint8_t, 32> getSignature(const MotionEntry& motionEntry,
                                               const DispatchEntry& dispatchEntry) const;
    const std::array<uint8_t, 32> getSignature(const KeyEntry& keyEntry,
                                               const DispatchEntry& dispatchEntry) const;
    std::condition_variable mInjectionResultAvailable;
    void setInjectionResult(EventEntry& entry,
                            android::os::InputEventInjectionResult injectionResult);
    void transformMotionEntryForInjectionLocked(MotionEntry&,
                                                const ui::Transform& injectedTransform) const
            REQUIRES(mLock);
    std::condition_variable mInjectionSyncFinished;
    void incrementPendingForegroundDispatches(EventEntry& entry);
    void decrementPendingForegroundDispatches(EventEntry& entry);
    struct KeyRepeatState {
        std::shared_ptr<KeyEntry> lastKeyEntry;
        nsecs_t nextRepeatTime;
    } mKeyRepeatState GUARDED_BY(mLock);
    void resetKeyRepeatLocked() REQUIRES(mLock);
    std::shared_ptr<KeyEntry> synthesizeKeyRepeatLocked(nsecs_t currentTime) REQUIRES(mLock);
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
    bool haveCommandsLocked() const REQUIRES(mLock);
    bool runCommandsLockedInterruptable() REQUIRES(mLock);
    void postCommandLocked(Command&& command) REQUIRES(mLock);
    std::chrono::nanoseconds mMonitorDispatchingTimeout GUARDED_BY(mLock);
    nsecs_t processAnrsLocked() REQUIRES(mLock);
    std::chrono::nanoseconds getDispatchingTimeoutLocked(const sp<Connection>& connection)
            REQUIRES(mLock);
    bool shouldSendKeyToInputFilterLocked(const NotifyKeyArgs* args) REQUIRES(mLock);
    bool shouldSendMotionToInputFilterLocked(const NotifyMotionArgs* args) REQUIRES(mLock);
    void drainInboundQueueLocked() REQUIRES(mLock);
    void releasePendingEventLocked() REQUIRES(mLock);
    void releaseInboundEventLocked(std::shared_ptr<EventEntry> entry) REQUIRES(mLock);
    bool mDispatchEnabled GUARDED_BY(mLock);
    bool mDispatchFrozen GUARDED_BY(mLock);
    bool mInputFilterEnabled GUARDED_BY(mLock);
    float mMaximumObscuringOpacityForTouch GUARDED_BY(mLock);
    std::map<int32_t , bool > mTouchModePerDisplay GUARDED_BY(mLock);
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
    std::unordered_map<int32_t , std::vector<sp<android::gui::WindowInfoHandle>>>
            mWindowHandlesByDisplay GUARDED_BY(mLock);
    std::unordered_map<int32_t , android::gui::DisplayInfo> mDisplayInfos
            GUARDED_BY(mLock);
    void setInputWindowsLocked(
            const std::vector<sp<android::gui::WindowInfoHandle>>& inputWindowHandles,
            int32_t displayId) REQUIRES(mLock);
    const std::vector<sp<android::gui::WindowInfoHandle>>& getWindowHandlesLocked(
            int32_t displayId) const REQUIRES(mLock);
    sp<android::gui::WindowInfoHandle> getWindowHandleLocked(
            const sp<IBinder>& windowHandleToken) const REQUIRES(mLock);
    sp<android::gui::WindowInfoHandle> getWindowHandleLocked(const sp<IBinder>& windowHandleToken,
                                                             int displayId) const REQUIRES(mLock);
    sp<android::gui::WindowInfoHandle> getWindowHandleLocked(
            const sp<android::gui::WindowInfoHandle>& windowHandle) const REQUIRES(mLock);
    std::shared_ptr<InputChannel> getInputChannelLocked(const sp<IBinder>& windowToken) const
            REQUIRES(mLock);
    sp<android::gui::WindowInfoHandle> getFocusedWindowHandleLocked(int displayId) const
            REQUIRES(mLock);
    bool canWindowReceiveMotionLocked(const sp<android::gui::WindowInfoHandle>& window,
                                      const MotionEntry& motionEntry) const REQUIRES(mLock);
    std::vector<InputTarget> getInputTargetsFromWindowHandlesLocked(
            const std::vector<sp<android::gui::WindowInfoHandle>>& windowHandles) const
            REQUIRES(mLock);
    void updateWindowHandlesForDisplayLocked(
            const std::vector<sp<android::gui::WindowInfoHandle>>& inputWindowHandles,
            int32_t displayId) REQUIRES(mLock);
    std::unordered_map<int32_t, TouchState> mTouchStatesByDisplay GUARDED_BY(mLock);
    std::unique_ptr<DragState> mDragState GUARDED_BY(mLock);
    void setFocusedApplicationLocked(
            int32_t displayId,
            const std::shared_ptr<InputApplicationHandle>& inputApplicationHandle) REQUIRES(mLock);
    std::unordered_map<int32_t, std::shared_ptr<InputApplicationHandle>>
            mFocusedApplicationHandlesByDisplay GUARDED_BY(mLock);
    int32_t mFocusedDisplayId GUARDED_BY(mLock);
    FocusResolver mFocusResolver GUARDED_BY(mLock);
    PointerCaptureRequest mCurrentPointerCaptureRequest GUARDED_BY(mLock);
    sp<IBinder> mWindowTokenWithPointerCapture GUARDED_BY(mLock);
    std::vector<int32_t> mIneligibleDisplaysForPointerCapture GUARDED_BY(mLock);
    void disablePointerCaptureForcedLocked() REQUIRES(mLock);
    void setPointerCaptureLocked(bool enable) REQUIRES(mLock);
    std::string mLastAnrState GUARDED_BY(mLock);
    std::unordered_set<sp<IBinder>, StrongPointerHash<IBinder>> mInteractionConnectionTokens
            GUARDED_BY(mLock);
    void updateInteractionTokensLocked(const EventEntry& entry,
                                       const std::vector<InputTarget>& targets) REQUIRES(mLock);
    bool dispatchConfigurationChangedLocked(nsecs_t currentTime,
                                            const ConfigurationChangedEntry& entry) REQUIRES(mLock);
    bool dispatchDeviceResetLocked(nsecs_t currentTime, const DeviceResetEntry& entry)
            REQUIRES(mLock);
    bool dispatchKeyLocked(nsecs_t currentTime, std::shared_ptr<KeyEntry> entry,
                           DropReason* dropReason, nsecs_t* nextWakeupTime) REQUIRES(mLock);
    bool dispatchMotionLocked(nsecs_t currentTime, std::shared_ptr<MotionEntry> entry,
                              DropReason* dropReason, nsecs_t* nextWakeupTime) REQUIRES(mLock);
    void dispatchFocusLocked(nsecs_t currentTime, std::shared_ptr<FocusEntry> entry)
            REQUIRES(mLock);
    void dispatchPointerCaptureChangedLocked(
            nsecs_t currentTime, const std::shared_ptr<PointerCaptureChangedEntry>& entry,
            DropReason& dropReason) REQUIRES(mLock);
    void dispatchTouchModeChangeLocked(nsecs_t currentTime,
                                       const std::shared_ptr<TouchModeEntry>& entry)
            REQUIRES(mLock);
    void dispatchEventLocked(nsecs_t currentTime, std::shared_ptr<EventEntry> entry,
                             const std::vector<InputTarget>& inputTargets) REQUIRES(mLock);
    void dispatchSensorLocked(nsecs_t currentTime, const std::shared_ptr<SensorEntry>& entry,
                              DropReason* dropReason, nsecs_t* nextWakeupTime) REQUIRES(mLock);
    void dispatchDragLocked(nsecs_t currentTime, std::shared_ptr<DragEntry> entry) REQUIRES(mLock);
    void logOutboundKeyDetails(const char* prefix, const KeyEntry& entry);
    void logOutboundMotionDetails(const char* prefix, const MotionEntry& entry);
    std::optional<nsecs_t> mNoFocusedWindowTimeoutTime GUARDED_BY(mLock);
    const std::chrono::nanoseconds mStaleEventTimeout;
    bool isStaleEvent(nsecs_t currentTime, const EventEntry& entry);
    bool shouldPruneInboundQueueLocked(const MotionEntry& motionEntry) REQUIRES(mLock);
    std::optional<nsecs_t> mKeyIsWaitingForEventsTimeout GUARDED_BY(mLock);
    bool shouldWaitToSendKeyLocked(nsecs_t currentTime, const char* focusedWindowName)
            REQUIRES(mLock);
    std::shared_ptr<InputApplicationHandle> mAwaitedFocusedApplication GUARDED_BY(mLock);
    int32_t mAwaitedApplicationDisplayId GUARDED_BY(mLock);
    void processNoFocusedWindowAnrLocked() REQUIRES(mLock);
    void processConnectionUnresponsiveLocked(const Connection& connection, std::string reason)
            REQUIRES(mLock);
    void processConnectionResponsiveLocked(const Connection& connection) REQUIRES(mLock);
    void sendWindowUnresponsiveCommandLocked(const sp<IBinder>& connectionToken,
                                             std::optional<int32_t> pid, std::string reason)
            REQUIRES(mLock);
    void sendWindowResponsiveCommandLocked(const sp<IBinder>& connectionToken,
                                           std::optional<int32_t> pid) REQUIRES(mLock);
    AnrTracker mAnrTracker GUARDED_BY(mLock);
    sp<android::gui::WindowInfoHandle> mLastHoverWindowHandle GUARDED_BY(mLock);
    void cancelEventsForAnrLocked(const sp<Connection>& connection) REQUIRES(mLock);
    void resetNoFocusedWindowTimeoutLocked() REQUIRES(mLock);
    bool shouldSplitTouch(const TouchState& touchState, const MotionEntry& entry) const;
    int32_t getTargetDisplayId(const EventEntry& entry);
    sp<android::gui::WindowInfoHandle> findFocusedWindowTargetLocked(
            nsecs_t currentTime, const EventEntry& entry, nsecs_t* nextWakeupTime,
            android::os::InputEventInjectionResult& outInjectionResult) REQUIRES(mLock);
    std::vector<TouchedWindow> findTouchedWindowTargetsLocked(
            nsecs_t currentTime, const MotionEntry& entry, bool* outConflictingPointerActions,
            android::os::InputEventInjectionResult& outInjectionResult) REQUIRES(mLock);
    std::vector<Monitor> selectResponsiveMonitorsLocked(
            const std::vector<Monitor>& gestureMonitors) const REQUIRES(mLock);
    void addWindowTargetLocked(const sp<android::gui::WindowInfoHandle>& windowHandle,
                               ftl::Flags<InputTarget::Flags> targetFlags, BitSet32 pointerIds,
                               std::optional<nsecs_t> firstDownTimeInTarget,
                               std::vector<InputTarget>& inputTargets) const REQUIRES(mLock);
    void addGlobalMonitoringTargetsLocked(std::vector<InputTarget>& inputTargets, int32_t displayId)
            REQUIRES(mLock);
    void pokeUserActivityLocked(const EventEntry& eventEntry) REQUIRES(mLock);
    void addDragEventLocked(const MotionEntry& entry) REQUIRES(mLock);
    void finishDragAndDrop(int32_t displayId, float x, float y) REQUIRES(mLock);
    struct TouchOcclusionInfo {
        bool hasBlockingOcclusion;
        float obscuringOpacity;
        std::string obscuringPackage;
        int32_t obscuringUid;
        std::vector<std::string> debugInfo;
    };
    TouchOcclusionInfo computeTouchOcclusionInfoLocked(
            const sp<android::gui::WindowInfoHandle>& windowHandle, int32_t x, int32_t y) const
            REQUIRES(mLock);
    bool isTouchTrustedLocked(const TouchOcclusionInfo& occlusionInfo) const REQUIRES(mLock);
    bool isWindowObscuredAtPointLocked(const sp<android::gui::WindowInfoHandle>& windowHandle,
                                       int32_t x, int32_t y) const REQUIRES(mLock);
    bool isWindowObscuredLocked(const sp<android::gui::WindowInfoHandle>& windowHandle) const
            REQUIRES(mLock);
    std::string dumpWindowForTouchOcclusion(const android::gui::WindowInfo* info,
                                            bool isTouchWindow) const;
    std::string getApplicationWindowLabel(const InputApplicationHandle* applicationHandle,
                                          const sp<android::gui::WindowInfoHandle>& windowHandle);
    bool shouldDropInput(const EventEntry& entry,
                         const sp<android::gui::WindowInfoHandle>& windowHandle) const
            REQUIRES(mLock);
    void prepareDispatchCycleLocked(nsecs_t currentTime, const sp<Connection>& connection,
                                    std::shared_ptr<EventEntry>, const InputTarget& inputTarget)
            REQUIRES(mLock);
    void enqueueDispatchEntriesLocked(nsecs_t currentTime, const sp<Connection>& connection,
                                      std::shared_ptr<EventEntry>, const InputTarget& inputTarget)
            REQUIRES(mLock);
    void enqueueDispatchEntryLocked(const sp<Connection>& connection, std::shared_ptr<EventEntry>,
                                    const InputTarget& inputTarget,
                                    ftl::Flags<InputTarget::Flags> dispatchMode) REQUIRES(mLock);
    status_t publishMotionEvent(Connection& connection, DispatchEntry& dispatchEntry) const;
    void startDispatchCycleLocked(nsecs_t currentTime, const sp<Connection>& connection)
            REQUIRES(mLock);
    void finishDispatchCycleLocked(nsecs_t currentTime, const sp<Connection>& connection,
                                   uint32_t seq, bool handled, nsecs_t consumeTime) REQUIRES(mLock);
    void abortBrokenDispatchCycleLocked(nsecs_t currentTime, const sp<Connection>& connection,
                                        bool notify) REQUIRES(mLock);
    void drainDispatchQueue(std::deque<DispatchEntry*>& queue);
    void releaseDispatchEntry(DispatchEntry* dispatchEntry);
    int handleReceiveCallback(int events, sp<IBinder> connectionToken);
    void dispatchPointerDownOutsideFocus(uint32_t source, int32_t action,
                                         const sp<IBinder>& newToken) REQUIRES(mLock);
    void synthesizeCancelationEventsForAllConnectionsLocked(const CancelationOptions& options)
            REQUIRES(mLock);
    void synthesizeCancelationEventsForMonitorsLocked(const CancelationOptions& options)
            REQUIRES(mLock);
    void synthesizeCancelationEventsForInputChannelLocked(
            const std::shared_ptr<InputChannel>& channel, const CancelationOptions& options)
            REQUIRES(mLock);
    void synthesizeCancelationEventsForConnectionLocked(const sp<Connection>& connection,
                                                        const CancelationOptions& options)
            REQUIRES(mLock);
    void synthesizePointerDownEventsForConnectionLocked(const nsecs_t downTime,
                                                        const sp<Connection>& connection,
                                                        ftl::Flags<InputTarget::Flags> targetFlags)
            REQUIRES(mLock);
    void synthesizeCancelationEventsForWindowLocked(
            const sp<android::gui::WindowInfoHandle>& windowHandle,
            const CancelationOptions& options) REQUIRES(mLock);
    std::unique_ptr<MotionEntry> splitMotionEvent(const MotionEntry& originalMotionEntry,
                                                  BitSet32 pointerIds, nsecs_t splitDownTime);
    void resetAndDropEverythingLocked(const char* reason) REQUIRES(mLock);
    void dumpDispatchStateLocked(std::string& dump) REQUIRES(mLock);
    void dumpMonitors(std::string& dump, const std::vector<Monitor>& monitors);
    void logDispatchStateLocked() REQUIRES(mLock);
    std::string dumpPointerCaptureStateLocked() REQUIRES(mLock);
    void removeMonitorChannelLocked(const sp<IBinder>& connectionToken) REQUIRES(mLock);
    status_t removeInputChannelLocked(const sp<IBinder>& connectionToken, bool notify)
            REQUIRES(mLock);
    void doDispatchCycleFinishedCommand(nsecs_t finishTime, const sp<Connection>& connection,
                                        uint32_t seq, bool handled, nsecs_t consumeTime)
            REQUIRES(mLock);
    void doInterceptKeyBeforeDispatchingCommand(const sp<IBinder>& focusedWindowToken,
                                                KeyEntry& entry) REQUIRES(mLock);
    void onFocusChangedLocked(const FocusResolver::FocusChanges& changes) REQUIRES(mLock);
    void sendFocusChangedCommandLocked(const sp<IBinder>& oldToken, const sp<IBinder>& newToken)
            REQUIRES(mLock);
    void sendDropWindowCommandLocked(const sp<IBinder>& token, float x, float y) REQUIRES(mLock);
    void onAnrLocked(const sp<Connection>& connection) REQUIRES(mLock);
    void onAnrLocked(std::shared_ptr<InputApplicationHandle> application) REQUIRES(mLock);
    void updateLastAnrStateLocked(const sp<android::gui::WindowInfoHandle>& window,
                                  const std::string& reason) REQUIRES(mLock);
    void updateLastAnrStateLocked(const InputApplicationHandle& application,
                                  const std::string& reason) REQUIRES(mLock);
    void updateLastAnrStateLocked(const std::string& windowLabel, const std::string& reason)
            REQUIRES(mLock);
    bool afterKeyEventLockedInterruptable(const sp<Connection>& connection,
                                          DispatchEntry* dispatchEntry, KeyEntry& keyEntry,
                                          bool handled) REQUIRES(mLock);
    bool afterMotionEventLockedInterruptable(const sp<Connection>& connection,
                                             DispatchEntry* dispatchEntry, MotionEntry& motionEntry,
                                             bool handled) REQUIRES(mLock);
    std::tuple<TouchState*, TouchedWindow*, int32_t >
    findTouchStateWindowAndDisplayLocked(const sp<IBinder>& token) REQUIRES(mLock);
    LatencyAggregator mLatencyAggregator GUARDED_BY(mLock);
    LatencyTracker mLatencyTracker GUARDED_BY(mLock);
    void traceInboundQueueLengthLocked() REQUIRES(mLock);
    void traceOutboundQueueLength(const Connection& connection);
    void traceWaitQueueLength(const Connection& connection);
    bool focusedWindowIsOwnedByLocked(int32_t pid, int32_t uid) REQUIRES(mLock);
    bool recentWindowsAreOwnedByLocked(int32_t pid, int32_t uid) REQUIRES(mLock);
    sp<InputReporterInterface> mReporter;
    void slipWallpaperTouch(ftl::Flags<InputTarget::Flags> targetFlags,
                            const sp<android::gui::WindowInfoHandle>& oldWindowHandle,
                            const sp<android::gui::WindowInfoHandle>& newWindowHandle,
                            TouchState& state, const BitSet32& pointerIds) REQUIRES(mLock);
    void transferWallpaperTouch(ftl::Flags<InputTarget::Flags> oldTargetFlags,
                                ftl::Flags<InputTarget::Flags> newTargetFlags,
                                const sp<android::gui::WindowInfoHandle> fromWindowHandle,
                                const sp<android::gui::WindowInfoHandle> toWindowHandle,
                                TouchState& state, const BitSet32& pointerIds) REQUIRES(mLock);
    sp<android::gui::WindowInfoHandle> findWallpaperWindowBelow(
            const sp<android::gui::WindowInfoHandle>& windowHandle) const REQUIRES(mLock);
};
}
