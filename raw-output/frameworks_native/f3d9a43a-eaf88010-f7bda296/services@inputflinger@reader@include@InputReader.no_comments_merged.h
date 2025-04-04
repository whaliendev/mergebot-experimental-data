#ifndef _UI_INPUTREADER_INPUT_READER_H
#define _UI_INPUTREADER_INPUT_READER_H 
#include "EventHub.h"
#include "InputListener.h"
#include "InputReaderBase.h"
#include "InputReaderContext.h"
#include "InputThread.h"
#include <PointerControllerInterface.h>
#include <utils/Condition.h>
#include <utils/Mutex.h>
#include <unordered_map>
#include <vector>
namespace android {
class InputDevice;
class InputMapper;
struct StylusState;
class InputReader : public InputReaderInterface {
public:
    InputReader(std::shared_ptr<EventHubInterface> eventHub,
                const sp<InputReaderPolicyInterface>& policy,
                const sp<InputListenerInterface>& listener);
    virtual ~InputReader();
    virtual void dump(std::string& dump) override;
    virtual void monitor() override;
    virtual status_t start() override;
    virtual status_t stop() override;
    virtual void getInputDevices(std::vector<InputDeviceInfo>& outInputDevices) override;
    virtual bool isInputDeviceEnabled(int32_t deviceId) override;
    virtual int32_t getScanCodeState(int32_t deviceId, uint32_t sourceMask,
                                     int32_t scanCode) override;
    virtual int32_t getKeyCodeState(int32_t deviceId, uint32_t sourceMask,
                                    int32_t keyCode) override;
    virtual int32_t getSwitchState(int32_t deviceId, uint32_t sourceMask, int32_t sw) override;
    virtual void toggleCapsLockState(int32_t deviceId) override;
    virtual bool hasKeys(int32_t deviceId, uint32_t sourceMask, size_t numCodes,
                         const int32_t* keyCodes, uint8_t* outFlags) override;
    virtual void requestRefreshConfiguration(uint32_t changes) override;
    virtual void vibrate(int32_t deviceId, const nsecs_t* pattern, size_t patternSize,
                         ssize_t repeat, int32_t token) override;
    virtual void cancelVibrate(int32_t deviceId, int32_t token) override;
    virtual bool canDispatchToDisplay(int32_t deviceId, int32_t displayId) override;
protected:
    virtual std::shared_ptr<InputDevice> createDeviceLocked(
            int32_t deviceId, const InputDeviceIdentifier& identifier);
    void loopOnce();
    class ContextImpl : public InputReaderContext {
        InputReader* mReader;
        IdGenerator mIdGenerator;
    public:
        explicit ContextImpl(InputReader* reader);
        virtual void updateGlobalMetaState() override;
        virtual int32_t getGlobalMetaState() override;
        virtual void disableVirtualKeysUntil(nsecs_t time) override;
        virtual bool shouldDropVirtualKey(nsecs_t now, int32_t keyCode, int32_t scanCode) override;
        virtual void fadePointer() override;
        virtual sp<PointerControllerInterface> getPointerController(int32_t deviceId) override;
        virtual void requestTimeoutAtTime(nsecs_t when) override;
        virtual int32_t bumpGeneration() override;
        virtual void getExternalStylusDevices(std::vector<InputDeviceInfo>& outDevices) override;
        virtual void dispatchExternalStylusState(const StylusState& outState) override;
        virtual InputReaderPolicyInterface* getPolicy() override;
        virtual InputListenerInterface* getListener() override;
        virtual EventHubInterface* getEventHub() override;
        virtual int32_t getNextId() override;
    } mContext;
    friend class ContextImpl;
private:
    std::unique_ptr<InputThread> mThread;
    Mutex mLock;
    Condition mReaderIsAliveCondition;
    std::shared_ptr<EventHubInterface> mEventHub;
    sp<InputReaderPolicyInterface> mPolicy;
    sp<QueuedInputListener> mQueuedListener;
    InputReaderConfiguration mConfig;
<<<<<<< HEAD
||||||| f7bda296de
    uint32_t mNextSequenceNum;
=======
    uint32_t mNextId;
>>>>>>> eaf88010
    static const int EVENT_BUFFER_SIZE = 256;
    RawEvent mEventBuffer[EVENT_BUFFER_SIZE];
    std::unordered_map<int32_t , std::shared_ptr<InputDevice>> mDevices;
    void processEventsLocked(const RawEvent* rawEvents, size_t count);
    void addDeviceLocked(nsecs_t when, int32_t eventHubId);
    void removeDeviceLocked(nsecs_t when, int32_t eventHubId);
    void processEventsForDeviceLocked(int32_t eventHubId, const RawEvent* rawEvents, size_t count);
    void timeoutExpiredLocked(nsecs_t when);
    void handleConfigurationChangedLocked(nsecs_t when);
    int32_t mGlobalMetaState;
    void updateGlobalMetaStateLocked();
    int32_t getGlobalMetaStateLocked();
    void notifyExternalStylusPresenceChanged();
    void getExternalStylusDevicesLocked(std::vector<InputDeviceInfo>& outDevices);
    void dispatchExternalStylusState(const StylusState& state);
    wp<PointerControllerInterface> mPointerController;
    sp<PointerControllerInterface> getPointerControllerLocked(int32_t deviceId);
    void updatePointerDisplayLocked();
    void fadePointerLocked();
    int32_t mGeneration;
    int32_t bumpGenerationLocked();
    int32_t mNextInputDeviceId;
    int32_t nextInputDeviceIdLocked();
    void getInputDevicesLocked(std::vector<InputDeviceInfo>& outInputDevices);
    nsecs_t mDisableVirtualKeysTimeout;
    void disableVirtualKeysUntilLocked(nsecs_t time);
    bool shouldDropVirtualKeyLocked(nsecs_t now, int32_t keyCode, int32_t scanCode);
    nsecs_t mNextTimeout;
    void requestTimeoutAtTimeLocked(nsecs_t when);
    uint32_t mConfigurationChangesToRefresh;
    void refreshConfigurationLocked(uint32_t changes);
    typedef int32_t (InputDevice::*GetStateFunc)(uint32_t sourceMask, int32_t code);
    int32_t getStateLocked(int32_t deviceId, uint32_t sourceMask, int32_t code,
                           GetStateFunc getStateFunc);
    bool markSupportedKeyCodesLocked(int32_t deviceId, uint32_t sourceMask, size_t numCodes,
                                     const int32_t* keyCodes, uint8_t* outFlags);
    InputDevice* findInputDevice(int32_t deviceId);
};
}
#endif
