#include <CursorInputMapper.h>
#include <InputDevice.h>
#include <InputMapper.h>
#include <InputReader.h>
#include <InputReaderBase.h>
#include <InputReaderFactory.h>
#include <KeyboardInputMapper.h>
#include <MultiTouchInputMapper.h>
#include <SingleTouchInputMapper.h>
#include <SwitchInputMapper.h>
#include <TestInputListener.h>
#include <TouchInputMapper.h>
#include <UinputDevice.h>
#include <android-base/thread_annotations.h>
#include <gtest/gtest.h>
#include <inttypes.h>
#include <math.h>
namespace android {
using std::chrono_literals::operator""ms;
static constexpr std::chrono::duration WAIT_TIMEOUT = 100ms;
static const nsecs_t ARBITRARY_TIME = 1234;
static const int32_t DISPLAY_ID = 0;
static const int32_t SECONDARY_DISPLAY_ID = DISPLAY_ID + 1;
static const int32_t DISPLAY_WIDTH = 480;
static const int32_t DISPLAY_HEIGHT = 800;
static const int32_t VIRTUAL_DISPLAY_ID = 1;
static const int32_t VIRTUAL_DISPLAY_WIDTH = 400;
static const int32_t VIRTUAL_DISPLAY_HEIGHT = 500;
static const char* VIRTUAL_DISPLAY_UNIQUE_ID = "virtual:1";
static constexpr std::optional<uint8_t> NO_PORT = std::nullopt;
static const float EPSILON = 0.001f;
template<typename T>
static inline T min(T a, T b) {
    return a < b ? a : b;
}
static inline float avg(float x, float y) {
    return (x + y) / 2;
}
class FakePointerController : public PointerControllerInterface {
    bool mHaveBounds;
    float mMinX, mMinY, mMaxX, mMaxY;
    float mX, mY;
    int32_t mButtonState;
    int32_t mDisplayId;
protected:
    virtual ~FakePointerController() { }
public:
    FakePointerController() :
        mHaveBounds(false), mMinX(0), mMinY(0), mMaxX(0), mMaxY(0), mX(0), mY(0),
        mButtonState(0), mDisplayId(ADISPLAY_ID_DEFAULT) {
    }
    void setBounds(float minX, float minY, float maxX, float maxY) {
        mHaveBounds = true;
        mMinX = minX;
        mMinY = minY;
        mMaxX = maxX;
        mMaxY = maxY;
    }
    virtual void setPosition(float x, float y) {
        mX = x;
        mY = y;
    }
    virtual void setButtonState(int32_t buttonState) {
        mButtonState = buttonState;
    }
    virtual int32_t getButtonState() const {
        return mButtonState;
    }
    virtual void getPosition(float* outX, float* outY) const {
        *outX = mX;
        *outY = mY;
    }
    virtual int32_t getDisplayId() const {
        return mDisplayId;
    }
    virtual void setDisplayViewport(const DisplayViewport& viewport) {
        mDisplayId = viewport.displayId;
    }
private:
    virtual void clearSpots() {
    }
    virtual void clearSpots() {
    }
    virtual void clearSpots() {
    }
    virtual void clearSpots() {
    }
    virtual void clearSpots() {
    }
    virtual void clearSpots() {
    }
    virtual void clearSpots() {
    }
    virtual void clearSpots() {
    }
    std::map<int32_t, std::vector<int32_t>> mSpotsByDisplay;
};
class FakeInputReaderPolicy : public InputReaderPolicyInterface {
    std::mutex mLock;
    std::condition_variable mDevicesChangedCondition;
    InputReaderConfiguration mConfig;
                              GUARDED_BY(mLock){false};
std::vector<InputDeviceInfo> mInputDevices GUARDED_BY(mLock){false};
bool mInputDevicesChanged GUARDED_BY(mLock){false};
    std::vector<DisplayViewport> mViewports;
    TouchAffineTransformation transform;
protected:
    virtual ~FakeInputReaderPolicy() { }
public:
    FakeInputReaderPolicy() {
    }
    void assertInputDevicesChanged() {
        waitForInputDevices([](bool devicesChanged) {
            if (!devicesChanged) {
                FAIL() << "Timed out waiting for notifyInputDevicesChanged() to be called.";
            }
        });
    }
    void assertInputDevicesNotChanged() {
        waitForInputDevices([](bool devicesChanged) {
            if (devicesChanged) {
                FAIL() << "Expected notifyInputDevicesChanged() to not be called.";
            }
        });
    }
    virtual void clearViewports() {
        mViewports.clear();
        mConfig.setDisplayViewports(mViewports);
    }
    std::optional<DisplayViewport> getDisplayViewportByUniqueId(const std::string& uniqueId) const {
        return mConfig.getDisplayViewportByUniqueId(uniqueId);
    }
    std::optional<DisplayViewport> getDisplayViewportByType(ViewportType type) const {
        return mConfig.getDisplayViewportByType(type);
    }
    std::optional<DisplayViewport> getDisplayViewportByPort(uint8_t displayPort) const {
        return mConfig.getDisplayViewportByPort(displayPort);
    }
    void addDisplayViewport(int32_t displayId, int32_t width, int32_t height, int32_t orientation,
            const std::string& uniqueId, std::optional<uint8_t> physicalPort,
            ViewportType viewportType) {
        const DisplayViewport viewport = createDisplayViewport(displayId, width, height,
                orientation, uniqueId, physicalPort, viewportType);
        mViewports.push_back(viewport);
        mConfig.setDisplayViewports(mViewports);
    }
    bool updateViewport(const DisplayViewport& viewport) {
        size_t count = mViewports.size();
        for (size_t i = 0; i < count; i++) {
            const DisplayViewport& currentViewport = mViewports[i];
            if (currentViewport.displayId == viewport.displayId) {
                mViewports[i] = viewport;
                mConfig.setDisplayViewports(mViewports);
                return true;
            }
        }
        return false;
    }
    void addExcludedDeviceName(const std::string& deviceName) {
        mConfig.excludedDeviceNames.push_back(deviceName);
    }
    void addInputPortAssociation(const std::string& inputPort, uint8_t displayPort) {
        mConfig.portAssociations.insert({inputPort, displayPort});
    }
    void addDisabledDevice(int32_t deviceId) { mConfig.disabledDevices.insert(deviceId); }
    void removeDisabledDevice(int32_t deviceId) { mConfig.disabledDevices.erase(deviceId); }
    void setPointerController(int32_t deviceId, const sp<FakePointerController>& controller) {
        mPointerControllers.add(deviceId, controller);
    }
    const InputReaderConfiguration* getReaderConfiguration() const {
        return &mConfig;
    }
    const std::vector<InputDeviceInfo>& getInputDevices() const {
        return mInputDevices;
    }
    TouchAffineTransformation getTouchAffineTransformation(const std::string& inputDeviceDescriptor,
            int32_t surfaceRotation) {
        return transform;
    }
    void setTouchAffineTransformation(const TouchAffineTransformation t) {
        transform = t;
    }
    void setPointerCapture(bool enabled) {
        mConfig.pointerCapture = enabled;
    }
    void setShowTouches(bool enabled) {
        mConfig.showTouches = enabled;
    }
    void setDefaultPointerDisplayId(int32_t pointerDisplayId) {
        mConfig.defaultPointerDisplayId = pointerDisplayId;
    }
private:
    DisplayViewport createDisplayViewport(int32_t displayId, int32_t width, int32_t height,
            int32_t orientation, const std::string& uniqueId, std::optional<uint8_t> physicalPort,
            ViewportType type) {
        bool isRotated = (orientation == DISPLAY_ORIENTATION_90
                || orientation == DISPLAY_ORIENTATION_270);
        DisplayViewport v;
        v.displayId = displayId;
        v.orientation = orientation;
        v.logicalLeft = 0;
        v.logicalTop = 0;
        v.logicalRight = isRotated ? height : width;
        v.logicalBottom = isRotated ? width : height;
        v.physicalLeft = 0;
        v.physicalTop = 0;
        v.physicalRight = isRotated ? height : width;
        v.physicalBottom = isRotated ? width : height;
        v.deviceWidth = isRotated ? height : width;
        v.deviceHeight = isRotated ? width : height;
        v.uniqueId = uniqueId;
        v.physicalPort = physicalPort;
        v.type = type;
        return v;
    }
    virtual void getReaderConfiguration(InputReaderConfiguration* outConfig) {
        *outConfig = mConfig;
    }
    virtual sp<PointerControllerInterface> obtainPointerController(int32_t deviceId) {
        return mPointerControllers.valueFor(deviceId);
    }
    virtual void notifyInputDevicesChanged(const std::vector<InputDeviceInfo>& inputDevices) {
        std::scoped_lock<std::mutex> lock(mLock);
        mInputDevices = inputDevices;
        mInputDevicesChanged = true;
        mDevicesChangedCondition.notify_all();
    }
    virtual sp<KeyCharacterMap> getKeyboardLayoutOverlay(const InputDeviceIdentifier&) {
        return nullptr;
    }
    virtual std::string getDeviceAlias(const InputDeviceIdentifier&) {
        return "";
    }
    void waitForInputDevices(std::function<void(bool)> processDevicesChanged) {
        std::unique_lock<std::mutex> lock(mLock);
        base::ScopedLockAssertion assumeLocked(mLock);
        const bool devicesChanged =
                mDevicesChangedCondition.wait_for(lock, WAIT_TIMEOUT, [this]() REQUIRES(mLock) {
                    return mInputDevicesChanged;
                });
        ASSERT_NO_FATAL_FAILURE(processDevicesChanged(devicesChanged));
        mInputDevicesChanged = false;
    }
};
class FakeEventHub : public EventHubInterface {
    struct KeyInfo {
        int32_t keyCode;
        uint32_t flags;
    };
    struct Device {
        InputDeviceIdentifier identifier;
        uint32_t classes;
        PropertyMap configuration;
        KeyedVector<int, RawAbsoluteAxisInfo> absoluteAxes;
        KeyedVector<int, bool> relativeAxes;
        KeyedVector<int32_t, int32_t> keyCodeStates;
        KeyedVector<int32_t, int32_t> scanCodeStates;
        KeyedVector<int32_t, int32_t> switchStates;
        KeyedVector<int32_t, int32_t> absoluteAxisValue;
        KeyedVector<int32_t, KeyInfo> keysByScanCode;
        KeyedVector<int32_t, KeyInfo> keysByUsageCode;
        KeyedVector<int32_t, bool> leds;
        std::vector<VirtualKeyDefinition> virtualKeys;
        bool enabled;
        status_t enable() {
            enabled = true;
            return OK;
        }
        status_t disable() {
            enabled = false;
            return OK;
        }
        explicit Device(uint32_t classes) :
                classes(classes), enabled(true) {
        }
    };
    std::mutex mLock;
    std::condition_variable mEventsCondition;
    KeyedVector<int32_t, Device*> mDevices;
    std::vector<std::string> mExcludedDevices;
    std::unordered_map<int32_t , std::vector<TouchVideoFrame>> mVideoFrames;
    std::unordered_map<int32_t , std::vector<TouchVideoFrame>> mVideoFrames;
public:
    virtual ~FakeEventHub() {
        for (size_t i {
        for (size_t i = 0; i < mDevices.size(); i++) {
            delete mDevices.valueAt(i);
        }
    }
    FakeEventHub() { }
    void addDevice(int32_t deviceId, const std::string& name, uint32_t classes) {
        Device* device = new Device(classes);
        device->identifier.name = name;
        mDevices.add(deviceId, device);
        enqueueEvent(ARBITRARY_TIME, deviceId, EventHubInterface::DEVICE_ADDED, 0, 0);
    }
    void removeDevice(int32_t deviceId) {
        delete mDevices.valueFor(deviceId);
        mDevices.removeItem(deviceId);
        enqueueEvent(ARBITRARY_TIME, deviceId, EventHubInterface::DEVICE_REMOVED, 0, 0);
    }
    bool isDeviceEnabled(int32_t deviceId) {
        Device* device = getDevice(deviceId);
        if (device == nullptr) {
            ALOGE("Incorrect device id=%" PRId32 " provided to %s", deviceId, __func__);
            return false;
        }
        return device->enabled;
    }
    status_t enableDevice(int32_t deviceId) {
        status_t result;
        Device* device = getDevice(deviceId);
        if (device == nullptr) {
            ALOGE("Incorrect device id=%" PRId32 " provided to %s", deviceId, __func__);
            return BAD_VALUE;
        }
        if (device->enabled) {
            ALOGW("Duplicate call to %s, device %" PRId32 " already enabled", __func__, deviceId);
            return OK;
        }
        result = device->enable();
        return result;
    }
    status_t disableDevice(int32_t deviceId) {
        Device* device = getDevice(deviceId);
        if (device == nullptr) {
            ALOGE("Incorrect device id=%" PRId32 " provided to %s", deviceId, __func__);
            return BAD_VALUE;
        }
        if (!device->enabled) {
            ALOGW("Duplicate call to %s, device %" PRId32 " already disabled", __func__, deviceId);
            return OK;
        }
        return device->disable();
    }
    void finishDeviceScan() {
        enqueueEvent(ARBITRARY_TIME, 0, EventHubInterface::FINISHED_DEVICE_SCAN, 0, 0);
    }
    void addConfigurationProperty(int32_t deviceId, const String8& key, const String8& value) {
        Device* device = getDevice(deviceId);
        device->configuration.addProperty(key, value);
    }
    void addConfigurationMap(int32_t deviceId, const PropertyMap* configuration) {
        Device* device = getDevice(deviceId);
        device->configuration.addAll(configuration);
    }
    void addAbsoluteAxis(int32_t deviceId, int axis,
            int32_t minValue, int32_t maxValue, int flat, int fuzz, int resolution = 0) {
        Device* device = getDevice(deviceId);
        RawAbsoluteAxisInfo info;
        info.valid = true;
        info.minValue = minValue;
        info.maxValue = maxValue;
        info.flat = flat;
        info.fuzz = fuzz;
        info.resolution = resolution;
        device->absoluteAxes.add(axis, info);
    }
    void addRelativeAxis(int32_t deviceId, int32_t axis) {
        Device* device = getDevice(deviceId);
        device->relativeAxes.add(axis, true);
    }
    void setKeyCodeState(int32_t deviceId, int32_t keyCode, int32_t state) {
        Device* device = getDevice(deviceId);
        device->keyCodeStates.replaceValueFor(keyCode, state);
    }
    void setScanCodeState(int32_t deviceId, int32_t scanCode, int32_t state) {
        Device* device = getDevice(deviceId);
        device->scanCodeStates.replaceValueFor(scanCode, state);
    }
    void setSwitchState(int32_t deviceId, int32_t switchCode, int32_t state) {
        Device* device = getDevice(deviceId);
        device->switchStates.replaceValueFor(switchCode, state);
    }
    void setAbsoluteAxisValue(int32_t deviceId, int32_t axis, int32_t value) {
        Device* device = getDevice(deviceId);
        device->absoluteAxisValue.replaceValueFor(axis, value);
    }
    void addKey(int32_t deviceId, int32_t scanCode, int32_t usageCode,
            int32_t keyCode, uint32_t flags) {
        Device* device = getDevice(deviceId);
        KeyInfo info;
        info.keyCode = keyCode;
        info.flags = flags;
        if (scanCode) {
            device->keysByScanCode.add(scanCode, info);
        }
        if (usageCode) {
            device->keysByUsageCode.add(usageCode, info);
        }
    }
    void addLed(int32_t deviceId, int32_t led, bool initialState) {
        Device* device = getDevice(deviceId);
        device->leds.add(led, initialState);
    }
    bool getLedState(int32_t deviceId, int32_t led) {
        Device* device = getDevice(deviceId);
        return device->leds.valueFor(led);
    }
    std::vector<std::string>& getExcludedDevices() {
        return mExcludedDevices;
    }
    void addVirtualKeyDefinition(int32_t deviceId, const VirtualKeyDefinition& definition) {
        Device* device = getDevice(deviceId);
        device->virtualKeys.push_back(definition);
    }
    void enqueueEvent(nsecs_t when, int32_t deviceId, int32_t type,
            int32_t code, int32_t value) {
        std::scoped_lock<std::mutex> lock(mLock);
        RawEvent event;
        event.when = when;
        event.deviceId = deviceId;
        event.type = type;
        event.code = code;
        event.value = value;
        mEvents.push_back(event);
        if (type == EV_ABS) {
            setAbsoluteAxisValue(deviceId, code, value);
        }
    }
    void setVideoFrames(std::unordered_map<int32_t ,
            std::vector<TouchVideoFrame>> videoFrames) {
        mVideoFrames = std::move(videoFrames);
    }
    void assertQueueIsEmpty() {
        std::unique_lock<std::mutex> lock(mLock);
        base::ScopedLockAssertion assumeLocked(mLock);
        const bool queueIsEmpty =
                mEventsCondition.wait_for(lock, WAIT_TIMEOUT,
                                          [this]() REQUIRES(mLock) { return mEvents.size() == 0; });
        if (!queueIsEmpty) {
            FAIL() << "Timed out waiting for EventHub queue to be emptied.";
        }
    }
private:
    Device* getDevice(int32_t deviceId) const {
        ssize_t index = mDevices.indexOfKey(deviceId);
        return index >= 0 ? mDevices.valueAt(index) : nullptr;
    }
    virtual uint32_t getDeviceClasses(int32_t deviceId) const {
        Device* device = getDevice(deviceId);
        return device ? device->classes : 0;
    }
    virtual InputDeviceIdentifier getDeviceIdentifier(int32_t deviceId) const {
        Device* device = getDevice(deviceId);
        return device ? device->identifier : InputDeviceIdentifier();
    }
    virtual int32_t getDeviceControllerNumber(int32_t) const {
        return 0;
    }
    virtual void getConfiguration(int32_t deviceId, PropertyMap* outConfiguration) const {
        Device* device = getDevice(deviceId);
        if (device) {
            *outConfiguration = device->configuration;
        }
    }
    virtual status_t getAbsoluteAxisInfo(int32_t deviceId, int axis,
            RawAbsoluteAxisInfo* outAxisInfo) const {
        Device* device = getDevice(deviceId);
        if (device && device->enabled) {
            ssize_t index = device->absoluteAxes.indexOfKey(axis);
            if (index >= 0) {
                *outAxisInfo = device->absoluteAxes.valueAt(index);
                return OK;
            }
        }
        outAxisInfo->clear();
        return -1;
    }
    virtual bool hasRelativeAxis(int32_t deviceId, int axis) const {
        Device* device = getDevice(deviceId);
        if (device) {
            return device->relativeAxes.indexOfKey(axis) >= 0;
        }
        return false;
    }
    virtual bool hasInputProperty(int32_t, int) const {
        return false;
    }
    virtual status_t mapKey(int32_t deviceId,
            int32_t scanCode, int32_t usageCode, int32_t metaState,
            int32_t* outKeycode, int32_t *outMetaState, uint32_t* outFlags) const {
        Device* device = getDevice(deviceId);
        if (device) {
            const KeyInfo* key = getKey(device, scanCode, usageCode);
            if (key) {
                if (outKeycode) {
                    *outKeycode = key->keyCode;
                }
                if (outFlags) {
                    *outFlags = key->flags;
                }
                if (outMetaState) {
                    *outMetaState = metaState;
                }
                return OK;
            }
        }
        return NAME_NOT_FOUND;
    }
    const KeyInfo* getKey(Device* device, int32_t scanCode, int32_t usageCode) const {
        if (usageCode) {
            ssize_t index = device->keysByUsageCode.indexOfKey(usageCode);
            if (index >= 0) {
                return &device->keysByUsageCode.valueAt(index);
            }
        }
        if (scanCode) {
            ssize_t index = device->keysByScanCode.indexOfKey(scanCode);
            if (index >= 0) {
                return &device->keysByScanCode.valueAt(index);
            }
        }
        return nullptr;
    }
    virtual status_t mapAxis(int32_t, int32_t, AxisInfo*) const {
        return NAME_NOT_FOUND;
    }
    virtual void setExcludedDevices(const std::vector<std::string>& devices) {
        mExcludedDevices = devices;
    }
    virtual size_t getEvents(int, RawEvent* buffer, size_t) {
        std::scoped_lock<std::mutex> lock(mLock);
        if (mEvents.empty()) {
            return 0;
        }
        *buffer = *mEvents.begin();
        mEvents.erase(mEvents.begin());
        mEventsCondition.notify_all();
        return 1;
    }
    virtual std::vector<TouchVideoFrame> getVideoFrames(int32_t deviceId) {
        auto it = mVideoFrames.find(deviceId);
        if (it != mVideoFrames.end()) {
            std::vector<TouchVideoFrame> frames = std::move(it->second);
            mVideoFrames.erase(deviceId);
            return frames;
        }
        return {};
    }
    virtual int32_t getScanCodeState(int32_t deviceId, int32_t scanCode) const {
        Device* device = getDevice(deviceId);
        if (device) {
            ssize_t index = device->scanCodeStates.indexOfKey(scanCode);
            if (index >= 0) {
                return device->scanCodeStates.valueAt(index);
            }
        }
        return AKEY_STATE_UNKNOWN;
    }
    virtual int32_t getKeyCodeState(int32_t deviceId, int32_t keyCode) const {
        Device* device = getDevice(deviceId);
        if (device) {
            ssize_t index = device->keyCodeStates.indexOfKey(keyCode);
            if (index >= 0) {
                return device->keyCodeStates.valueAt(index);
            }
        }
        return AKEY_STATE_UNKNOWN;
    }
    virtual int32_t getSwitchState(int32_t deviceId, int32_t sw) const {
        Device* device = getDevice(deviceId);
        if (device) {
            ssize_t index = device->switchStates.indexOfKey(sw);
            if (index >= 0) {
                return device->switchStates.valueAt(index);
            }
        }
        return AKEY_STATE_UNKNOWN;
    }
    virtual status_t getAbsoluteAxisValue(int32_t deviceId, int32_t axis,
            int32_t* outValue) const {
        Device* device = getDevice(deviceId);
        if (device) {
            ssize_t index = device->absoluteAxisValue.indexOfKey(axis);
            if (index >= 0) {
                *outValue = device->absoluteAxisValue.valueAt(index);
                return OK;
            }
        }
        *outValue = 0;
        return -1;
    }
    virtual bool markSupportedKeyCodes(int32_t deviceId, size_t numCodes, const int32_t* keyCodes,
            uint8_t* outFlags) const {
        bool result = false;
        Device* device = getDevice(deviceId);
        if (device) {
            for (size_t i = 0; i < numCodes; i++) {
                for (size_t j = 0; j < device->keysByScanCode.size(); j++) {
                    if (keyCodes[i] == device->keysByScanCode.valueAt(j).keyCode) {
                        outFlags[i] = 1;
                        result = true;
                    }
                }
                for (size_t j = 0; j < device->keysByUsageCode.size(); j++) {
                    if (keyCodes[i] == device->keysByUsageCode.valueAt(j).keyCode) {
                        outFlags[i] = 1;
                        result = true;
                    }
                }
            }
        }
        return result;
    }
    virtual bool hasScanCode(int32_t deviceId, int32_t scanCode) const {
        Device* device = getDevice(deviceId);
        if (device) {
            ssize_t index = device->keysByScanCode.indexOfKey(scanCode);
            return index >= 0;
        }
        return false;
    }
    virtual bool hasLed(int32_t deviceId, int32_t led) const {
        Device* device = getDevice(deviceId);
        return device && device->leds.indexOfKey(led) >= 0;
    }
    virtual void setLedState(int32_t deviceId, int32_t led, bool on) {
        Device* device = getDevice(deviceId);
        if (device) {
            ssize_t index = device->leds.indexOfKey(led);
            if (index >= 0) {
                device->leds.replaceValueAt(led, on);
            } else {
                ADD_FAILURE()
                        << "Attempted to set the state of an LED that the EventHub declared "
                        "was not present.  led=" << led;
            }
        }
    }
    virtual void getVirtualKeyDefinitions(int32_t deviceId,
            std::vector<VirtualKeyDefinition>& outVirtualKeys) const {
        outVirtualKeys.clear();
        Device* device = getDevice(deviceId);
        if (device) {
            outVirtualKeys = device->virtualKeys;
        }
    }
    virtual sp<KeyCharacterMap> getKeyCharacterMap(int32_t) const {
        return nullptr;
    }
    virtual bool setKeyboardLayoutOverlay(int32_t, const sp<KeyCharacterMap>&) {
        return false;
    }
    virtual void vibrate(int32_t, nsecs_t) {
    }
    virtual void cancelVibrate(int32_t) {
    }
    virtual bool isExternal(int32_t) const {
        return false;
    }
    virtual void dump(std::string&) {
    }
    virtual void monitor() {
    }
    virtual void requestReopenDevices() {
    }
    virtual void wake() {
    }
};
class FakeInputReaderContext : public InputReaderContext {
    std::shared_ptr<EventHubInterface> mEventHub;
    sp<InputReaderPolicyInterface> mPolicy;
    sp<InputListenerInterface> mListener;
    int32_t mGlobalMetaState;
    bool mUpdateGlobalMetaStateWasCalled;
    int32_t mGeneration;
    int32_t mNextId;
    wp<PointerControllerInterface> mPointerController;
public:
    FakeInputReaderContext(std::shared_ptr<EventHubInterface> eventHub, const sp<InputReaderPolicyInterface>& policy, const sp<InputListenerInterface>& listener): mEventHub(eventHub), mPolicy(policy), mListener(listener), mGlobalMetaState(0), mNextId(1), mNextSequenceNum(1) {}
    virtual ~FakeInputReaderContext() { }
    void assertUpdateGlobalMetaStateWasCalled() {
        ASSERT_TRUE(mUpdateGlobalMetaStateWasCalled)
                << "Expected updateGlobalMetaState() to have been called.";
        mUpdateGlobalMetaStateWasCalled = false;
    }
    void setGlobalMetaState(int32_t state) {
        mGlobalMetaState = state;
    }
    uint32_t getGeneration() {
        return mGeneration;
    }
    void updatePointerDisplay() {
        sp<PointerControllerInterface> controller = mPointerController.promote();
        if (controller != nullptr) {
            InputReaderConfiguration config;
            mPolicy->getReaderConfiguration(&config);
            auto viewport = config.getDisplayViewportById(config.defaultPointerDisplayId);
            if (viewport) {
                controller->setDisplayViewport(*viewport);
            }
        }
    }
private:
    virtual void updateGlobalMetaState() {
        mUpdateGlobalMetaStateWasCalled = true;
    }
    virtual int32_t getGlobalMetaState() {
        return mGlobalMetaState;
    }
    virtual EventHubInterface* getEventHub() {
        return mEventHub.get();
    }
    virtual InputReaderPolicyInterface* getPolicy() {
        return mPolicy.get();
    }
    virtual InputListenerInterface* getListener() {
        return mListener.get();
    }
    virtual void disableVirtualKeysUntil(nsecs_t) {
    }
    virtual bool shouldDropVirtualKey(nsecs_t, int32_t, int32_t) { return false; }
    virtual sp<PointerControllerInterface> getPointerController(int32_t deviceId) {
        sp<PointerControllerInterface> controller = mPointerController.promote();
        if (controller == nullptr) {
            controller = mPolicy->obtainPointerController(deviceId);
            mPointerController = controller;
            updatePointerDisplay();
        }
        return controller;
    }
    virtual void fadePointer() {
    }
    virtual void requestTimeoutAtTime(nsecs_t) {
    }
    virtual int32_t bumpGeneration() {
        return ++mGeneration;
    }
    virtual void getExternalStylusDevices(std::vector<InputDeviceInfo>& outDevices) {
    }
    virtual void dispatchExternalStylusState(const StylusState&) {
    }
    virtual int32_t getNextId() { return mNextId++; }
};
class FakeInputMapper : public InputMapper {
    uint32_t mSources;
    int32_t mKeyboardType;
    int32_t mMetaState;
    KeyedVector<int32_t, int32_t> mKeyCodeStates;
    KeyedVector<int32_t, int32_t> mScanCodeStates;
    KeyedVector<int32_t, int32_t> mSwitchStates;
    std::vector<int32_t> mSupportedKeyCodes;
    std::mutex mLock;
    std::condition_variable mStateChangedCondition;
bool mConfigureWasCalled RawEvent mLastEvent GUARDED_BY(mLock);
bool mResetWasCalled RawEvent mLastEvent GUARDED_BY(mLock);
bool mProcessWasCalled RawEvent mLastEvent GUARDED_BY(mLock);
    RawEvent mLastEvent GUARDED_BY(mLock);
    std::optional<DisplayViewport> mViewport;
public:
    FakeInputMapper(InputDeviceContext& deviceContext, uint32_t sources)
          : InputMapper(deviceContext),
            mSources(sources),
            mKeyboardType(AINPUT_KEYBOARD_TYPE_NONE),
            mMetaState(0),
            mConfigureWasCalled(false),
            mResetWasCalled(false),
            mProcessWasCalled(false) {}
    virtual ~FakeInputMapper() { }
    void setKeyboardType(int32_t keyboardType) {
        mKeyboardType = keyboardType;
    }
    void setMetaState(int32_t metaState) {
        mMetaState = metaState;
    }
    void assertConfigureWasCalled() {
        std::unique_lock<std::mutex> lock(mLock);
        base::ScopedLockAssertion assumeLocked(mLock);
        const bool configureCalled =
                mStateChangedCondition.wait_for(lock, WAIT_TIMEOUT, [this]() REQUIRES(mLock) {
                    return mConfigureWasCalled;
                });
        if (!configureCalled) {
            FAIL() << "Expected configure() to have been called.";
        }
        mConfigureWasCalled = false;
    }
    void assertResetWasCalled() {
        std::unique_lock<std::mutex> lock(mLock);
        base::ScopedLockAssertion assumeLocked(mLock);
        const bool resetCalled =
                mStateChangedCondition.wait_for(lock, WAIT_TIMEOUT, [this]() REQUIRES(mLock) {
                    return mResetWasCalled;
                });
        if (!resetCalled) {
            FAIL() << "Expected reset() to have been called.";
        }
        mResetWasCalled = false;
    }
    void assertProcessWasCalled(RawEvent* outLastEvent = nullptr) {
        std::unique_lock<std::mutex> lock(mLock);
        base::ScopedLockAssertion assumeLocked(mLock);
        const bool processCalled =
                mStateChangedCondition.wait_for(lock, WAIT_TIMEOUT, [this]() REQUIRES(mLock) {
                    return mProcessWasCalled;
                });
        if (!processCalled) {
            FAIL() << "Expected process() to have been called.";
        }
        if (outLastEvent) {
            *outLastEvent = mLastEvent;
        }
        mProcessWasCalled = false;
    }
    void setKeyCodeState(int32_t keyCode, int32_t state) {
        mKeyCodeStates.replaceValueFor(keyCode, state);
    }
    void setScanCodeState(int32_t scanCode, int32_t state) {
        mScanCodeStates.replaceValueFor(scanCode, state);
    }
    void setSwitchState(int32_t switchCode, int32_t state) {
        mSwitchStates.replaceValueFor(switchCode, state);
    }
    void addSupportedKeyCode(int32_t keyCode) {
        mSupportedKeyCodes.push_back(keyCode);
    }
private:
    virtual uint32_t getSources() {
        return mSources;
    }
    virtual void populateDeviceInfo(InputDeviceInfo* deviceInfo) {
        InputMapper::populateDeviceInfo(deviceInfo);
        if (mKeyboardType != AINPUT_KEYBOARD_TYPE_NONE) {
            deviceInfo->setKeyboardType(mKeyboardType);
        }
    }
    virtual void configure(nsecs_t, const InputReaderConfiguration* config, uint32_t changes) {
        std::scoped_lock<std::mutex> lock(mLock);
        mConfigureWasCalled = true;
        const std::optional<uint8_t> displayPort = getDeviceContext().getAssociatedDisplayPort();
        if (displayPort && (changes & InputReaderConfiguration::CHANGE_DISPLAY_INFO)) {
            mViewport = config->getDisplayViewportByPort(*displayPort);
        }
        mStateChangedCondition.notify_all();
    }
    virtual void reset(nsecs_t) {
        std::scoped_lock<std::mutex> lock(mLock);
        mResetWasCalled = true;
        mStateChangedCondition.notify_all();
    }
    virtual void process(const RawEvent* rawEvent) {
        std::scoped_lock<std::mutex> lock(mLock);
        mLastEvent = *rawEvent;
        mProcessWasCalled = true;
        mStateChangedCondition.notify_all();
    }
    virtual int32_t getKeyCodeState(uint32_t, int32_t keyCode) {
        ssize_t index = mKeyCodeStates.indexOfKey(keyCode);
        return index >= 0 ? mKeyCodeStates.valueAt(index) : AKEY_STATE_UNKNOWN;
    }
    virtual int32_t getScanCodeState(uint32_t, int32_t scanCode) {
        ssize_t index = mScanCodeStates.indexOfKey(scanCode);
        return index >= 0 ? mScanCodeStates.valueAt(index) : AKEY_STATE_UNKNOWN;
    }
    virtual int32_t getSwitchState(uint32_t, int32_t switchCode) {
        ssize_t index = mSwitchStates.indexOfKey(switchCode);
        return index >= 0 ? mSwitchStates.valueAt(index) : AKEY_STATE_UNKNOWN;
    }
    virtual bool markSupportedKeyCodes(uint32_t, size_t numCodes,
            const int32_t* keyCodes, uint8_t* outFlags) {
        bool result = false;
        for (size_t i = 0; i < numCodes; i++) {
            for (size_t j = 0; j < mSupportedKeyCodes.size(); j++) {
                if (keyCodes[i] == mSupportedKeyCodes[j]) {
                    outFlags[i] = 1;
                    result = true;
                }
            }
        }
        return result;
    }
    virtual int32_t getMetaState() {
        return mMetaState;
    }
    virtual void fadePointer() {
    }
    virtual std::optional<int32_t> getAssociatedDisplay() {
        if (mViewport) {
            return std::make_optional(mViewport->displayId);
        }
        return std::nullopt;
    }
};
class InstrumentedInputReader : public InputReader {
    std::shared_ptr<InputDevice> mNextDevice;
public:
    InstrumentedInputReader(std::shared_ptr<EventHubInterface> eventHub,
                            const sp<InputReaderPolicyInterface>& policy,
                            const sp<InputListenerInterface>& listener)
          : InputReader(eventHub, policy, listener), mNextDevice(nullptr) {}
    virtual ~InstrumentedInputReader() {}
    void setNextDevice(std::shared_ptr<InputDevice> device) { mNextDevice = device; }
    std::shared_ptr<InputDevice> newDevice(int32_t deviceId, const std::string& name,
                                           const std::string& location = "") {
        InputDeviceIdentifier identifier;
        identifier.name = name;
        identifier.location = location;
        int32_t generation = deviceId + 1;
        return std::make_shared<InputDevice>(&mContext, deviceId, generation, identifier);
    }
    using InputReader::loopOnce;
protected:
    virtual std::shared_ptr<InputDevice> createDeviceLocked(
            int32_t eventHubId, const InputDeviceIdentifier& identifier) {
        if (mNextDevice) {
            std::shared_ptr<InputDevice> device(mNextDevice);
            mNextDevice = nullptr;
            return device;
        }
        return InputReader::createDeviceLocked(eventHubId, identifier);
    }
    friend class InputReaderTest;
};
class InputReaderPolicyTest : public testing::Test {
protected:
    sp<FakeInputReaderPolicy> mFakePolicy;
    virtual void SetUp() override { mFakePolicy = new FakeInputReaderPolicy(); }
    virtual void TearDown() override { mFakePolicy.clear(); }
};
TEST_F(InputReaderPolicyTest, Viewports_GetByPort) {
    constexpr ViewportType type = ViewportType::VIEWPORT_EXTERNAL;
    const std::string uniqueId1 = "uniqueId1";
    const std::string uniqueId2 = "uniqueId2";
    constexpr int32_t displayId1 = 1;
    constexpr int32_t displayId2 = 2;
    const uint8_t hdmi1 = 0;
    const uint8_t hdmi2 = 1;
    const uint8_t hdmi3 = 2;
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(displayId1, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, uniqueId1, hdmi3, type);
    mFakePolicy->addDisplayViewport(displayId2, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, uniqueId2, hdmi1, type);
    std::optional<DisplayViewport> hdmi1Viewport = mFakePolicy->getDisplayViewportByPort(hdmi1);
    ASSERT_TRUE(hdmi1Viewport);
    ASSERT_EQ(displayId2, hdmi1Viewport->displayId);
    ASSERT_EQ(uniqueId2, hdmi1Viewport->uniqueId);
    hdmi1Viewport = mFakePolicy->getDisplayViewportByUniqueId(uniqueId2);
    ASSERT_TRUE(hdmi1Viewport);
    ASSERT_EQ(displayId2, hdmi1Viewport->displayId);
    ASSERT_EQ(uniqueId2, hdmi1Viewport->uniqueId);
    ASSERT_EQ(type, hdmi1Viewport->type);
    std::optional<DisplayViewport> hdmi2Viewport = mFakePolicy->getDisplayViewportByPort(hdmi2);
    ASSERT_FALSE(hdmi2Viewport);
}
TEST_F(InputReaderPolicyTest, Viewports_GetByPort) {
    constexpr ViewportType type = ViewportType::VIEWPORT_EXTERNAL;
    const std::string uniqueId1 = "uniqueId1";
    const std::string uniqueId2 = "uniqueId2";
    constexpr int32_t displayId1 = 1;
    constexpr int32_t displayId2 = 2;
    const uint8_t hdmi1 = 0;
    const uint8_t hdmi2 = 1;
    const uint8_t hdmi3 = 2;
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(displayId1, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, uniqueId1, hdmi3, type);
    mFakePolicy->addDisplayViewport(displayId2, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, uniqueId2, hdmi1, type);
    std::optional<DisplayViewport> hdmi1Viewport = mFakePolicy->getDisplayViewportByPort(hdmi1);
    ASSERT_TRUE(hdmi1Viewport);
    ASSERT_EQ(displayId2, hdmi1Viewport->displayId);
    ASSERT_EQ(uniqueId2, hdmi1Viewport->uniqueId);
    hdmi1Viewport = mFakePolicy->getDisplayViewportByUniqueId(uniqueId2);
    ASSERT_TRUE(hdmi1Viewport);
    ASSERT_EQ(displayId2, hdmi1Viewport->displayId);
    ASSERT_EQ(uniqueId2, hdmi1Viewport->uniqueId);
    ASSERT_EQ(type, hdmi1Viewport->type);
    std::optional<DisplayViewport> hdmi2Viewport = mFakePolicy->getDisplayViewportByPort(hdmi2);
    ASSERT_FALSE(hdmi2Viewport);
}
TEST_F(InputReaderPolicyTest, Viewports_GetByPort) {
    constexpr ViewportType type = ViewportType::VIEWPORT_EXTERNAL;
    const std::string uniqueId1 = "uniqueId1";
    const std::string uniqueId2 = "uniqueId2";
    constexpr int32_t displayId1 = 1;
    constexpr int32_t displayId2 = 2;
    const uint8_t hdmi1 = 0;
    const uint8_t hdmi2 = 1;
    const uint8_t hdmi3 = 2;
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(displayId1, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, uniqueId1, hdmi3, type);
    mFakePolicy->addDisplayViewport(displayId2, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, uniqueId2, hdmi1, type);
    std::optional<DisplayViewport> hdmi1Viewport = mFakePolicy->getDisplayViewportByPort(hdmi1);
    ASSERT_TRUE(hdmi1Viewport);
    ASSERT_EQ(displayId2, hdmi1Viewport->displayId);
    ASSERT_EQ(uniqueId2, hdmi1Viewport->uniqueId);
    hdmi1Viewport = mFakePolicy->getDisplayViewportByUniqueId(uniqueId2);
    ASSERT_TRUE(hdmi1Viewport);
    ASSERT_EQ(displayId2, hdmi1Viewport->displayId);
    ASSERT_EQ(uniqueId2, hdmi1Viewport->uniqueId);
    ASSERT_EQ(type, hdmi1Viewport->type);
    std::optional<DisplayViewport> hdmi2Viewport = mFakePolicy->getDisplayViewportByPort(hdmi2);
    ASSERT_FALSE(hdmi2Viewport);
}
TEST_F(InputReaderPolicyTest, Viewports_GetByPort) {
    constexpr ViewportType type = ViewportType::VIEWPORT_EXTERNAL;
    const std::string uniqueId1 = "uniqueId1";
    const std::string uniqueId2 = "uniqueId2";
    constexpr int32_t displayId1 = 1;
    constexpr int32_t displayId2 = 2;
    const uint8_t hdmi1 = 0;
    const uint8_t hdmi2 = 1;
    const uint8_t hdmi3 = 2;
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(displayId1, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, uniqueId1, hdmi3, type);
    mFakePolicy->addDisplayViewport(displayId2, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, uniqueId2, hdmi1, type);
    std::optional<DisplayViewport> hdmi1Viewport = mFakePolicy->getDisplayViewportByPort(hdmi1);
    ASSERT_TRUE(hdmi1Viewport);
    ASSERT_EQ(displayId2, hdmi1Viewport->displayId);
    ASSERT_EQ(uniqueId2, hdmi1Viewport->uniqueId);
    hdmi1Viewport = mFakePolicy->getDisplayViewportByUniqueId(uniqueId2);
    ASSERT_TRUE(hdmi1Viewport);
    ASSERT_EQ(displayId2, hdmi1Viewport->displayId);
    ASSERT_EQ(uniqueId2, hdmi1Viewport->uniqueId);
    ASSERT_EQ(type, hdmi1Viewport->type);
    std::optional<DisplayViewport> hdmi2Viewport = mFakePolicy->getDisplayViewportByPort(hdmi2);
    ASSERT_FALSE(hdmi2Viewport);
}
class InputReaderTest : public testing::Test {
protected:
    sp<TestInputListener> mFakeListener;
    sp<FakeInputReaderPolicy> mFakePolicy;
    std::shared_ptr<FakeEventHub> mFakeEventHub;
    std::unique_ptr<InstrumentedInputReader> mReader;
    virtual void SetUp() override {
        mFakeEventHub = std::make_unique<FakeEventHub>();
        mFakePolicy = new FakeInputReaderPolicy();
        mFakeListener = new TestInputListener();
        mReader = std::make_unique<InstrumentedInputReader>(mFakeEventHub, mFakePolicy,
                                                            mFakeListener);
    }
    virtual void TearDown() override {
        mFakeListener.clear();
        mFakePolicy.clear();
    }
    void addDevice(int32_t eventHubId, const std::string& name, uint32_t classes,
                   const PropertyMap* configuration) {
        mFakeEventHub->addDevice(eventHubId, name, classes);
        if (configuration) {
            mFakeEventHub->addConfigurationMap(eventHubId, configuration);
        }
        mFakeEventHub->finishDeviceScan();
        mReader->loopOnce();
        mReader->loopOnce();
        ASSERT_NO_FATAL_FAILURE(mFakePolicy->assertInputDevicesChanged());
        ASSERT_NO_FATAL_FAILURE(mFakeEventHub->assertQueueIsEmpty());
    }
    void disableDevice(int32_t deviceId) {
        mFakePolicy->addDisabledDevice(deviceId);
        mReader->requestRefreshConfiguration(InputReaderConfiguration::CHANGE_ENABLED_STATE);
    }
    void enableDevice(int32_t deviceId) {
        mFakePolicy->removeDisabledDevice(deviceId);
        mReader->requestRefreshConfiguration(InputReaderConfiguration::CHANGE_ENABLED_STATE);
    }
    FakeInputMapper& addDeviceWithFakeInputMapper(int32_t deviceId, int32_t eventHubId,
                                                  const std::string& name, uint32_t classes,
                                                  uint32_t sources,
                                                  const PropertyMap* configuration) {
        std::shared_ptr<InputDevice> device = mReader->newDevice(deviceId, name);
        FakeInputMapper& mapper = device->addMapper<FakeInputMapper>(eventHubId, sources);
        mReader->setNextDevice(device);
        addDevice(eventHubId, name, classes, configuration);
        return mapper;
    }
};
TEST_F(InputReaderTest, Device_CanDispatchToDisplay) {
    constexpr int32_t deviceId {
    constexpr int32_t deviceId = END_RESERVED_ID + 1000;
    constexpr uint32_t deviceClass = INPUT_DEVICE_CLASS_KEYBOARD;
    constexpr int32_t eventHubId = 1;
    const char* DEVICE_LOCATION = "USB1";
    std::shared_ptr<InputDevice> device = mReader->newDevice(deviceId, "fake", DEVICE_LOCATION);
    FakeInputMapper& mapper =
            device->addMapper<FakeInputMapper>(eventHubId, AINPUT_SOURCE_TOUCHSCREEN);
    mReader->setNextDevice(device);
    const uint8_t hdmi1 = 1;
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:0", NO_PORT, ViewportType::VIEWPORT_INTERNAL);
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:1", hdmi1, ViewportType::VIEWPORT_EXTERNAL);
    mReader->requestRefreshConfiguration(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mReader->loopOnce();
    ASSERT_NO_FATAL_FAILURE(addDevice(eventHubId, "fake", deviceClass, nullptr));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyConfigurationChangedWasCalled());
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyDeviceResetWasCalled());
    ASSERT_NO_FATAL_FAILURE(mapper.assertConfigureWasCalled());
    ASSERT_EQ(deviceId, device->getId());
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, DISPLAY_ID));
    ASSERT_TRUE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
    disableDevice(deviceId);
    mReader->loopOnce();
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
}
TEST_F(InputReaderTest, Device_CanDispatchToDisplay) {
    constexpr int32_t deviceId {
    constexpr int32_t deviceId = END_RESERVED_ID + 1000;
    constexpr uint32_t deviceClass = INPUT_DEVICE_CLASS_KEYBOARD;
    constexpr int32_t eventHubId = 1;
    const char* DEVICE_LOCATION = "USB1";
    std::shared_ptr<InputDevice> device = mReader->newDevice(deviceId, "fake", DEVICE_LOCATION);
    FakeInputMapper& mapper =
            device->addMapper<FakeInputMapper>(eventHubId, AINPUT_SOURCE_TOUCHSCREEN);
    mReader->setNextDevice(device);
    const uint8_t hdmi1 = 1;
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:0", NO_PORT, ViewportType::VIEWPORT_INTERNAL);
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:1", hdmi1, ViewportType::VIEWPORT_EXTERNAL);
    mReader->requestRefreshConfiguration(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mReader->loopOnce();
    ASSERT_NO_FATAL_FAILURE(addDevice(eventHubId, "fake", deviceClass, nullptr));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyConfigurationChangedWasCalled());
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyDeviceResetWasCalled());
    ASSERT_NO_FATAL_FAILURE(mapper.assertConfigureWasCalled());
    ASSERT_EQ(deviceId, device->getId());
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, DISPLAY_ID));
    ASSERT_TRUE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
    disableDevice(deviceId);
    mReader->loopOnce();
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
}
TEST_F(InputReaderTest, Device_CanDispatchToDisplay) {
    constexpr int32_t deviceId {
    constexpr int32_t deviceId = END_RESERVED_ID + 1000;
    constexpr uint32_t deviceClass = INPUT_DEVICE_CLASS_KEYBOARD;
    constexpr int32_t eventHubId = 1;
    const char* DEVICE_LOCATION = "USB1";
    std::shared_ptr<InputDevice> device = mReader->newDevice(deviceId, "fake", DEVICE_LOCATION);
    FakeInputMapper& mapper =
            device->addMapper<FakeInputMapper>(eventHubId, AINPUT_SOURCE_TOUCHSCREEN);
    mReader->setNextDevice(device);
    const uint8_t hdmi1 = 1;
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:0", NO_PORT, ViewportType::VIEWPORT_INTERNAL);
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:1", hdmi1, ViewportType::VIEWPORT_EXTERNAL);
    mReader->requestRefreshConfiguration(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mReader->loopOnce();
    ASSERT_NO_FATAL_FAILURE(addDevice(eventHubId, "fake", deviceClass, nullptr));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyConfigurationChangedWasCalled());
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyDeviceResetWasCalled());
    ASSERT_NO_FATAL_FAILURE(mapper.assertConfigureWasCalled());
    ASSERT_EQ(deviceId, device->getId());
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, DISPLAY_ID));
    ASSERT_TRUE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
    disableDevice(deviceId);
    mReader->loopOnce();
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
}
TEST_F(InputReaderTest, Device_CanDispatchToDisplay) {
    constexpr int32_t deviceId {
    constexpr int32_t deviceId = END_RESERVED_ID + 1000;
    constexpr uint32_t deviceClass = INPUT_DEVICE_CLASS_KEYBOARD;
    constexpr int32_t eventHubId = 1;
    const char* DEVICE_LOCATION = "USB1";
    std::shared_ptr<InputDevice> device = mReader->newDevice(deviceId, "fake", DEVICE_LOCATION);
    FakeInputMapper& mapper =
            device->addMapper<FakeInputMapper>(eventHubId, AINPUT_SOURCE_TOUCHSCREEN);
    mReader->setNextDevice(device);
    const uint8_t hdmi1 = 1;
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:0", NO_PORT, ViewportType::VIEWPORT_INTERNAL);
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:1", hdmi1, ViewportType::VIEWPORT_EXTERNAL);
    mReader->requestRefreshConfiguration(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mReader->loopOnce();
    ASSERT_NO_FATAL_FAILURE(addDevice(eventHubId, "fake", deviceClass, nullptr));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyConfigurationChangedWasCalled());
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyDeviceResetWasCalled());
    ASSERT_NO_FATAL_FAILURE(mapper.assertConfigureWasCalled());
    ASSERT_EQ(deviceId, device->getId());
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, DISPLAY_ID));
    ASSERT_TRUE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
    disableDevice(deviceId);
    mReader->loopOnce();
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
}
TEST_F(InputReaderTest, Device_CanDispatchToDisplay) {
    constexpr int32_t deviceId {
    constexpr int32_t deviceId = END_RESERVED_ID + 1000;
    constexpr uint32_t deviceClass = INPUT_DEVICE_CLASS_KEYBOARD;
    constexpr int32_t eventHubId = 1;
    const char* DEVICE_LOCATION = "USB1";
    std::shared_ptr<InputDevice> device = mReader->newDevice(deviceId, "fake", DEVICE_LOCATION);
    FakeInputMapper& mapper =
            device->addMapper<FakeInputMapper>(eventHubId, AINPUT_SOURCE_TOUCHSCREEN);
    mReader->setNextDevice(device);
    const uint8_t hdmi1 = 1;
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:0", NO_PORT, ViewportType::VIEWPORT_INTERNAL);
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:1", hdmi1, ViewportType::VIEWPORT_EXTERNAL);
    mReader->requestRefreshConfiguration(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mReader->loopOnce();
    ASSERT_NO_FATAL_FAILURE(addDevice(eventHubId, "fake", deviceClass, nullptr));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyConfigurationChangedWasCalled());
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyDeviceResetWasCalled());
    ASSERT_NO_FATAL_FAILURE(mapper.assertConfigureWasCalled());
    ASSERT_EQ(deviceId, device->getId());
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, DISPLAY_ID));
    ASSERT_TRUE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
    disableDevice(deviceId);
    mReader->loopOnce();
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
}
TEST_F(InputReaderTest, Device_CanDispatchToDisplay) {
    constexpr int32_t deviceId {
    constexpr int32_t deviceId = END_RESERVED_ID + 1000;
    constexpr uint32_t deviceClass = INPUT_DEVICE_CLASS_KEYBOARD;
    constexpr int32_t eventHubId = 1;
    const char* DEVICE_LOCATION = "USB1";
    std::shared_ptr<InputDevice> device = mReader->newDevice(deviceId, "fake", DEVICE_LOCATION);
    FakeInputMapper& mapper =
            device->addMapper<FakeInputMapper>(eventHubId, AINPUT_SOURCE_TOUCHSCREEN);
    mReader->setNextDevice(device);
    const uint8_t hdmi1 = 1;
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:0", NO_PORT, ViewportType::VIEWPORT_INTERNAL);
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:1", hdmi1, ViewportType::VIEWPORT_EXTERNAL);
    mReader->requestRefreshConfiguration(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mReader->loopOnce();
    ASSERT_NO_FATAL_FAILURE(addDevice(eventHubId, "fake", deviceClass, nullptr));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyConfigurationChangedWasCalled());
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyDeviceResetWasCalled());
    ASSERT_NO_FATAL_FAILURE(mapper.assertConfigureWasCalled());
    ASSERT_EQ(deviceId, device->getId());
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, DISPLAY_ID));
    ASSERT_TRUE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
    disableDevice(deviceId);
    mReader->loopOnce();
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
}
TEST_F(InputReaderTest, Device_CanDispatchToDisplay) {
    constexpr int32_t deviceId {
    constexpr int32_t deviceId = END_RESERVED_ID + 1000;
    constexpr uint32_t deviceClass = INPUT_DEVICE_CLASS_KEYBOARD;
    constexpr int32_t eventHubId = 1;
    const char* DEVICE_LOCATION = "USB1";
    std::shared_ptr<InputDevice> device = mReader->newDevice(deviceId, "fake", DEVICE_LOCATION);
    FakeInputMapper& mapper =
            device->addMapper<FakeInputMapper>(eventHubId, AINPUT_SOURCE_TOUCHSCREEN);
    mReader->setNextDevice(device);
    const uint8_t hdmi1 = 1;
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:0", NO_PORT, ViewportType::VIEWPORT_INTERNAL);
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:1", hdmi1, ViewportType::VIEWPORT_EXTERNAL);
    mReader->requestRefreshConfiguration(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mReader->loopOnce();
    ASSERT_NO_FATAL_FAILURE(addDevice(eventHubId, "fake", deviceClass, nullptr));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyConfigurationChangedWasCalled());
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyDeviceResetWasCalled());
    ASSERT_NO_FATAL_FAILURE(mapper.assertConfigureWasCalled());
    ASSERT_EQ(deviceId, device->getId());
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, DISPLAY_ID));
    ASSERT_TRUE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
    disableDevice(deviceId);
    mReader->loopOnce();
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
}
TEST_F(InputReaderTest, Device_CanDispatchToDisplay) {
    constexpr int32_t deviceId {
    constexpr int32_t deviceId = END_RESERVED_ID + 1000;
    constexpr uint32_t deviceClass = INPUT_DEVICE_CLASS_KEYBOARD;
    constexpr int32_t eventHubId = 1;
    const char* DEVICE_LOCATION = "USB1";
    std::shared_ptr<InputDevice> device = mReader->newDevice(deviceId, "fake", DEVICE_LOCATION);
    FakeInputMapper& mapper =
            device->addMapper<FakeInputMapper>(eventHubId, AINPUT_SOURCE_TOUCHSCREEN);
    mReader->setNextDevice(device);
    const uint8_t hdmi1 = 1;
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:0", NO_PORT, ViewportType::VIEWPORT_INTERNAL);
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:1", hdmi1, ViewportType::VIEWPORT_EXTERNAL);
    mReader->requestRefreshConfiguration(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mReader->loopOnce();
    ASSERT_NO_FATAL_FAILURE(addDevice(eventHubId, "fake", deviceClass, nullptr));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyConfigurationChangedWasCalled());
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyDeviceResetWasCalled());
    ASSERT_NO_FATAL_FAILURE(mapper.assertConfigureWasCalled());
    ASSERT_EQ(deviceId, device->getId());
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, DISPLAY_ID));
    ASSERT_TRUE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
    disableDevice(deviceId);
    mReader->loopOnce();
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
}
TEST_F(InputReaderTest, Device_CanDispatchToDisplay) {
    constexpr int32_t deviceId {
    constexpr int32_t deviceId = END_RESERVED_ID + 1000;
    constexpr uint32_t deviceClass = INPUT_DEVICE_CLASS_KEYBOARD;
    constexpr int32_t eventHubId = 1;
    const char* DEVICE_LOCATION = "USB1";
    std::shared_ptr<InputDevice> device = mReader->newDevice(deviceId, "fake", DEVICE_LOCATION);
    FakeInputMapper& mapper =
            device->addMapper<FakeInputMapper>(eventHubId, AINPUT_SOURCE_TOUCHSCREEN);
    mReader->setNextDevice(device);
    const uint8_t hdmi1 = 1;
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:0", NO_PORT, ViewportType::VIEWPORT_INTERNAL);
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:1", hdmi1, ViewportType::VIEWPORT_EXTERNAL);
    mReader->requestRefreshConfiguration(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mReader->loopOnce();
    ASSERT_NO_FATAL_FAILURE(addDevice(eventHubId, "fake", deviceClass, nullptr));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyConfigurationChangedWasCalled());
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyDeviceResetWasCalled());
    ASSERT_NO_FATAL_FAILURE(mapper.assertConfigureWasCalled());
    ASSERT_EQ(deviceId, device->getId());
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, DISPLAY_ID));
    ASSERT_TRUE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
    disableDevice(deviceId);
    mReader->loopOnce();
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
}
TEST_F(InputReaderTest, Device_CanDispatchToDisplay) {
    constexpr int32_t deviceId {
    constexpr int32_t deviceId = END_RESERVED_ID + 1000;
    constexpr uint32_t deviceClass = INPUT_DEVICE_CLASS_KEYBOARD;
    constexpr int32_t eventHubId = 1;
    const char* DEVICE_LOCATION = "USB1";
    std::shared_ptr<InputDevice> device = mReader->newDevice(deviceId, "fake", DEVICE_LOCATION);
    FakeInputMapper& mapper =
            device->addMapper<FakeInputMapper>(eventHubId, AINPUT_SOURCE_TOUCHSCREEN);
    mReader->setNextDevice(device);
    const uint8_t hdmi1 = 1;
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->clearViewports();
    mFakePolicy->addDisplayViewport(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:0", NO_PORT, ViewportType::VIEWPORT_INTERNAL);
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, "local:1", hdmi1, ViewportType::VIEWPORT_EXTERNAL);
    mReader->requestRefreshConfiguration(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mReader->loopOnce();
    ASSERT_NO_FATAL_FAILURE(addDevice(eventHubId, "fake", deviceClass, nullptr));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyConfigurationChangedWasCalled());
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyDeviceResetWasCalled());
    ASSERT_NO_FATAL_FAILURE(mapper.assertConfigureWasCalled());
    ASSERT_EQ(deviceId, device->getId());
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, DISPLAY_ID));
    ASSERT_TRUE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
    disableDevice(deviceId);
    mReader->loopOnce();
    ASSERT_FALSE(mReader->canDispatchToDisplay(deviceId, SECONDARY_DISPLAY_ID));
}
class InputReaderIntegrationTest : public testing::Test {
protected:
    sp<TestInputListener> mTestListener;
    sp<FakeInputReaderPolicy> mFakePolicy;
    sp<InputReaderInterface> mReader;
    virtual void SetUp() override {
        mFakePolicy = new FakeInputReaderPolicy();
        mTestListener = new TestInputListener();
        mReader = new InputReader(std::make_shared<EventHub>(), mFakePolicy, mTestListener);
        ASSERT_EQ(mReader->start(), OK);
        ASSERT_NO_FATAL_FAILURE(mFakePolicy->assertInputDevicesChanged());
        ASSERT_NO_FATAL_FAILURE(mTestListener->assertNotifyConfigurationChangedWasCalled());
    }
    virtual void TearDown() override {
        ASSERT_EQ(mReader->stop(), OK);
        mTestListener.clear();
        mFakePolicy.clear();
    }
};
TEST_F(InputReaderIntegrationTest, SendsEventsToInputListener) {
    std::unique_ptr<UinputHomeKey> keyboard = createUinputDevice<UinputHomeKey>();
    ASSERT_NO_FATAL_FAILURE(mFakePolicy->assertInputDevicesChanged());
    NotifyConfigurationChangedArgs configChangedArgs;
    ASSERT_NO_FATAL_FAILURE(
            mTestListener->assertNotifyConfigurationChangedWasCalled(&configChangedArgs));
    int32_t prevId = configChangedArgs.id;
    nsecs_t prevTimestamp = configChangedArgs.eventTime;
    NotifyKeyArgs keyArgs;
    keyboard->pressAndReleaseHomeKey();
    ASSERT_NO_FATAL_FAILURE(mTestListener->assertNotifyKeyWasCalled(&keyArgs));
    ASSERT_EQ(AKEY_EVENT_ACTION_DOWN, keyArgs.action);
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
    prevId = keyArgs.id;
||||||| f7bda296de
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
    prevId = keyArgs.id;
||||||| f7bda296de
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
    prevId = keyArgs.id;
||||||| f7bda296de
    ASSERT_LT(prevSequenceNum, keyArgs.sequenceNum);
    prevSequenceNum = keyArgs.sequenceNum;
=======
    ASSERT_LT(prevId, keyArgs.id);
    prevId = keyArgs.id;
>>>>>>> eaf88010
=======
    ASSERT_LT(prevId, keyArgs.id);
    prevId = keyArgs.id;
>>>>>>> eaf88010
=======
    ASSERT_LT(prevId, keyArgs.id);
    prevId = keyArgs.id;
>>>>>>> eaf88010
    ASSERT_LE(prevTimestamp, keyArgs.eventTime);
    prevTimestamp = keyArgs.eventTime;
    ASSERT_NO_FATAL_FAILURE(mTestListener->assertNotifyKeyWasCalled(&keyArgs));
    ASSERT_EQ(AKEY_EVENT_ACTION_UP, keyArgs.action);
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
||||||| f7bda296de
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
||||||| f7bda296de
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
||||||| f7bda296de
    ASSERT_LT(prevSequenceNum, keyArgs.sequenceNum);
=======
    ASSERT_LT(prevId, keyArgs.id);
>>>>>>> eaf88010
=======
    ASSERT_LT(prevId, keyArgs.id);
>>>>>>> eaf88010
=======
    ASSERT_LT(prevId, keyArgs.id);
>>>>>>> eaf88010
    ASSERT_LE(prevTimestamp, keyArgs.eventTime);
}
TEST_F(InputReaderIntegrationTest, SendsEventsToInputListener) {
    std::unique_ptr<UinputHomeKey> keyboard = createUinputDevice<UinputHomeKey>();
    ASSERT_NO_FATAL_FAILURE(mFakePolicy->assertInputDevicesChanged());
    NotifyConfigurationChangedArgs configChangedArgs;
    ASSERT_NO_FATAL_FAILURE(
            mTestListener->assertNotifyConfigurationChangedWasCalled(&configChangedArgs));
    int32_t prevId = configChangedArgs.id;
    nsecs_t prevTimestamp = configChangedArgs.eventTime;
    NotifyKeyArgs keyArgs;
    keyboard->pressAndReleaseHomeKey();
    ASSERT_NO_FATAL_FAILURE(mTestListener->assertNotifyKeyWasCalled(&keyArgs));
    ASSERT_EQ(AKEY_EVENT_ACTION_DOWN, keyArgs.action);
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
    prevId = keyArgs.id;
||||||| f7bda296de
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
    prevId = keyArgs.id;
||||||| f7bda296de
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
    prevId = keyArgs.id;
||||||| f7bda296de
    ASSERT_LT(prevSequenceNum, keyArgs.sequenceNum);
    prevSequenceNum = keyArgs.sequenceNum;
=======
    ASSERT_LT(prevId, keyArgs.id);
    prevId = keyArgs.id;
>>>>>>> eaf88010
=======
    ASSERT_LT(prevId, keyArgs.id);
    prevId = keyArgs.id;
>>>>>>> eaf88010
=======
    ASSERT_LT(prevId, keyArgs.id);
    prevId = keyArgs.id;
>>>>>>> eaf88010
    ASSERT_LE(prevTimestamp, keyArgs.eventTime);
    prevTimestamp = keyArgs.eventTime;
    ASSERT_NO_FATAL_FAILURE(mTestListener->assertNotifyKeyWasCalled(&keyArgs));
    ASSERT_EQ(AKEY_EVENT_ACTION_UP, keyArgs.action);
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
||||||| f7bda296de
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
||||||| f7bda296de
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
||||||| f7bda296de
    ASSERT_LT(prevSequenceNum, keyArgs.sequenceNum);
=======
    ASSERT_LT(prevId, keyArgs.id);
>>>>>>> eaf88010
=======
    ASSERT_LT(prevId, keyArgs.id);
>>>>>>> eaf88010
=======
    ASSERT_LT(prevId, keyArgs.id);
>>>>>>> eaf88010
    ASSERT_LE(prevTimestamp, keyArgs.eventTime);
}
TEST_F(InputReaderIntegrationTest, SendsEventsToInputListener) {
    std::unique_ptr<UinputHomeKey> keyboard = createUinputDevice<UinputHomeKey>();
    ASSERT_NO_FATAL_FAILURE(mFakePolicy->assertInputDevicesChanged());
    NotifyConfigurationChangedArgs configChangedArgs;
    ASSERT_NO_FATAL_FAILURE(
            mTestListener->assertNotifyConfigurationChangedWasCalled(&configChangedArgs));
    int32_t prevId = configChangedArgs.id;
    nsecs_t prevTimestamp = configChangedArgs.eventTime;
    NotifyKeyArgs keyArgs;
    keyboard->pressAndReleaseHomeKey();
    ASSERT_NO_FATAL_FAILURE(mTestListener->assertNotifyKeyWasCalled(&keyArgs));
    ASSERT_EQ(AKEY_EVENT_ACTION_DOWN, keyArgs.action);
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
    prevId = keyArgs.id;
||||||| f7bda296de
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
    prevId = keyArgs.id;
||||||| f7bda296de
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
    prevId = keyArgs.id;
||||||| f7bda296de
    ASSERT_LT(prevSequenceNum, keyArgs.sequenceNum);
    prevSequenceNum = keyArgs.sequenceNum;
=======
    ASSERT_LT(prevId, keyArgs.id);
    prevId = keyArgs.id;
>>>>>>> eaf88010
=======
    ASSERT_LT(prevId, keyArgs.id);
    prevId = keyArgs.id;
>>>>>>> eaf88010
=======
    ASSERT_LT(prevId, keyArgs.id);
    prevId = keyArgs.id;
>>>>>>> eaf88010
    ASSERT_LE(prevTimestamp, keyArgs.eventTime);
    prevTimestamp = keyArgs.eventTime;
    ASSERT_NO_FATAL_FAILURE(mTestListener->assertNotifyKeyWasCalled(&keyArgs));
    ASSERT_EQ(AKEY_EVENT_ACTION_UP, keyArgs.action);
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
||||||| f7bda296de
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
||||||| f7bda296de
<<<<<<< HEAD
    ASSERT_NE(prevId, keyArgs.id);
||||||| f7bda296de
    ASSERT_LT(prevSequenceNum, keyArgs.sequenceNum);
=======
    ASSERT_LT(prevId, keyArgs.id);
>>>>>>> eaf88010
=======
    ASSERT_LT(prevId, keyArgs.id);
>>>>>>> eaf88010
=======
    ASSERT_LT(prevId, keyArgs.id);
>>>>>>> eaf88010
    ASSERT_LE(prevTimestamp, keyArgs.eventTime);
}
class InputDeviceTest : public testing::Test {
protected:
    static const char* DEVICE_NAME;
    static const char* DEVICE_LOCATION;
    static const int32_t DEVICE_ID;
    static const int32_t DEVICE_GENERATION;
    static const int32_t DEVICE_CONTROLLER_NUMBER;
    static const uint32_t DEVICE_CLASSES;
    static const int32_t EVENTHUB_ID;
    std::shared_ptr<FakeEventHub> mFakeEventHub;
    sp<FakeInputReaderPolicy> mFakePolicy;
    sp<TestInputListener> mFakeListener;
    FakeInputReaderContext* mFakeContext;
    std::shared_ptr<InputDevice> mDevice;
    virtual void SetUp() override {
        mFakeEventHub = std::make_unique<FakeEventHub>();
        mFakePolicy = new FakeInputReaderPolicy();
        mFakeListener = new TestInputListener();
        mFakeContext = new FakeInputReaderContext(mFakeEventHub, mFakePolicy, mFakeListener);
        mFakeEventHub->addDevice(EVENTHUB_ID, DEVICE_NAME, 0);
        InputDeviceIdentifier identifier;
        identifier.name = DEVICE_NAME;
        identifier.location = DEVICE_LOCATION;
        mDevice = std::make_shared<InputDevice>(mFakeContext, DEVICE_ID, DEVICE_GENERATION,
                                                identifier);
    }
    virtual void TearDown() override {
        mDevice = nullptr;
        delete mFakeContext;
        mFakeListener.clear();
        mFakePolicy.clear();
    }
};
const int32_t InputDeviceTest::EVENTHUB_ID = 1;
const int32_t InputDeviceTest::EVENTHUB_ID = 1;
const int32_t InputDeviceTest::EVENTHUB_ID = 1;
const int32_t InputDeviceTest::EVENTHUB_ID = 1;
const int32_t InputDeviceTest::EVENTHUB_ID = 1;
const int32_t InputDeviceTest::EVENTHUB_ID = 1;
const int32_t InputDeviceTest::EVENTHUB_ID = 1;
TEST_F(InputDeviceTest, Configure_AssignsDisplayPort) {
    mDevice->addMapper<FakeInputMapper>(EVENTHUB_ID, AINPUT_SOURCE_TOUCHSCREEN);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0);
    ASSERT_TRUE(mDevice->isEnabled());
    constexpr uint8_t hdmi {
    mDevice->addMapper<FakeInputMapper>(EVENTHUB_ID, AINPUT_SOURCE_TOUCHSCREEN);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0);
    ASSERT_TRUE(mDevice->isEnabled());
    constexpr uint8_t hdmi = 1;
    const std::string UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(mDevice->isEnabled());
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                    DISPLAY_ORIENTATION_0, UNIQUE_ID, hdmi,
                                    ViewportType::VIEWPORT_INTERNAL);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    mFakePolicy->addDisabledDevice(mDevice->getId());
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_ENABLED_STATE);
    ASSERT_FALSE(mDevice->isEnabled());
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(mDevice->isEnabled());
}
TEST_F(InputDeviceTest, Configure_AssignsDisplayPort) {
    mDevice->addMapper<FakeInputMapper>(EVENTHUB_ID, AINPUT_SOURCE_TOUCHSCREEN);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0);
    ASSERT_TRUE(mDevice->isEnabled());
    constexpr uint8_t hdmi {
    mDevice->addMapper<FakeInputMapper>(EVENTHUB_ID, AINPUT_SOURCE_TOUCHSCREEN);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0);
    ASSERT_TRUE(mDevice->isEnabled());
    constexpr uint8_t hdmi = 1;
    const std::string UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(mDevice->isEnabled());
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                    DISPLAY_ORIENTATION_0, UNIQUE_ID, hdmi,
                                    ViewportType::VIEWPORT_INTERNAL);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    mFakePolicy->addDisabledDevice(mDevice->getId());
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_ENABLED_STATE);
    ASSERT_FALSE(mDevice->isEnabled());
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(mDevice->isEnabled());
}
TEST_F(InputDeviceTest, Configure_AssignsDisplayPort) {
    mDevice->addMapper<FakeInputMapper>(EVENTHUB_ID, AINPUT_SOURCE_TOUCHSCREEN);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0);
    ASSERT_TRUE(mDevice->isEnabled());
    constexpr uint8_t hdmi {
    mDevice->addMapper<FakeInputMapper>(EVENTHUB_ID, AINPUT_SOURCE_TOUCHSCREEN);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0);
    ASSERT_TRUE(mDevice->isEnabled());
    constexpr uint8_t hdmi = 1;
    const std::string UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(mDevice->isEnabled());
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                    DISPLAY_ORIENTATION_0, UNIQUE_ID, hdmi,
                                    ViewportType::VIEWPORT_INTERNAL);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    mFakePolicy->addDisabledDevice(mDevice->getId());
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_ENABLED_STATE);
    ASSERT_FALSE(mDevice->isEnabled());
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(mDevice->isEnabled());
}
TEST_F(InputDeviceTest, Configure_AssignsDisplayPort) {
    mDevice->addMapper<FakeInputMapper>(EVENTHUB_ID, AINPUT_SOURCE_TOUCHSCREEN);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0);
    ASSERT_TRUE(mDevice->isEnabled());
    constexpr uint8_t hdmi {
    mDevice->addMapper<FakeInputMapper>(EVENTHUB_ID, AINPUT_SOURCE_TOUCHSCREEN);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0);
    ASSERT_TRUE(mDevice->isEnabled());
    constexpr uint8_t hdmi = 1;
    const std::string UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(mDevice->isEnabled());
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                    DISPLAY_ORIENTATION_0, UNIQUE_ID, hdmi,
                                    ViewportType::VIEWPORT_INTERNAL);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    mFakePolicy->addDisabledDevice(mDevice->getId());
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_ENABLED_STATE);
    ASSERT_FALSE(mDevice->isEnabled());
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(mDevice->isEnabled());
}
TEST_F(InputDeviceTest, Configure_AssignsDisplayPort) {
    mDevice->addMapper<FakeInputMapper>(EVENTHUB_ID, AINPUT_SOURCE_TOUCHSCREEN);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0);
    ASSERT_TRUE(mDevice->isEnabled());
    constexpr uint8_t hdmi {
    mDevice->addMapper<FakeInputMapper>(EVENTHUB_ID, AINPUT_SOURCE_TOUCHSCREEN);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0);
    ASSERT_TRUE(mDevice->isEnabled());
    constexpr uint8_t hdmi = 1;
    const std::string UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(mDevice->isEnabled());
    mFakePolicy->addDisplayViewport(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                    DISPLAY_ORIENTATION_0, UNIQUE_ID, hdmi,
                                    ViewportType::VIEWPORT_INTERNAL);
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    mFakePolicy->addDisabledDevice(mDevice->getId());
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_ENABLED_STATE);
    ASSERT_FALSE(mDevice->isEnabled());
    mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(mDevice->isEnabled());
}
class InputMapperTest : public testing::Test {
protected:
    static const char* DEVICE_NAME;
    static const char* DEVICE_LOCATION;
    static const int32_t DEVICE_ID;
    static const int32_t DEVICE_GENERATION;
    static const int32_t DEVICE_CONTROLLER_NUMBER;
    static const uint32_t DEVICE_CLASSES;
    static const int32_t EVENTHUB_ID;
    std::shared_ptr<FakeEventHub> mFakeEventHub;
    sp<FakeInputReaderPolicy> mFakePolicy;
    sp<TestInputListener> mFakeListener;
    FakeInputReaderContext* mFakeContext;
    InputDevice* mDevice;
    virtual void SetUp(uint32_t classes) {
        mFakeEventHub = std::make_unique<FakeEventHub>();
        mFakePolicy = new FakeInputReaderPolicy();
        mFakeListener = new TestInputListener();
        mFakeContext = new FakeInputReaderContext(mFakeEventHub, mFakePolicy, mFakeListener);
        InputDeviceIdentifier identifier;
        identifier.name = DEVICE_NAME;
        identifier.location = DEVICE_LOCATION;
        mDevice = new InputDevice(mFakeContext, DEVICE_ID, DEVICE_GENERATION, identifier);
        mFakeEventHub->addDevice(EVENTHUB_ID, DEVICE_NAME, classes);
    }
    virtual void SetUp() override { SetUp(DEVICE_CLASSES){ SetUp(DEVICE_CLASSES); }
    virtual void TearDown() override {
        delete mDevice;
        delete mFakeContext;
        mFakeListener.clear();
        mFakePolicy.clear();
    }
    void addConfigurationProperty(const char* key, const char* value) {
        mFakeEventHub->addConfigurationProperty(EVENTHUB_ID, String8(key), String8(value));
    }
    void configureDevice(uint32_t changes) {
        if (!changes || (changes & InputReaderConfiguration::CHANGE_DISPLAY_INFO)) {
            mFakeContext->updatePointerDisplay();
        }
        mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), changes);
    }
    template <class T, typename... Args>
    T& addMapperAndConfigure(Args... args) {
        T& mapper = mDevice->addMapper<T>(EVENTHUB_ID, args...);
        configureDevice(0);
        mDevice->reset(ARBITRARY_TIME);
        return mapper;
    }
    void setDisplayInfoAndReconfigure(int32_t displayId, int32_t width, int32_t height,
            int32_t orientation, const std::string& uniqueId,
            std::optional<uint8_t> physicalPort, ViewportType viewportType) {
        mFakePolicy->addDisplayViewport(
                displayId, width, height, orientation, uniqueId, physicalPort, viewportType);
        configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    }
    void clearViewports() {
        mFakePolicy->clearViewports();
    }
    static void process(InputMapper& mapper, nsecs_t when, int32_t type, int32_t code,
                        int32_t value) {
        RawEvent event;
        event.when = when;
        event.deviceId = mapper.getDeviceContext().getEventHubId();
        event.type = type;
        event.code = code;
        event.value = value;
        mapper.process(&event);
    }
    static void assertMotionRange(const InputDeviceInfo& info,
            int32_t axis, uint32_t source, float min, float max, float flat, float fuzz) {
        const InputDeviceInfo::MotionRange* range = info.getMotionRange(axis, source);
        ASSERT_TRUE(range != nullptr) << "Axis: " << axis << " Source: " << source;
        ASSERT_EQ(axis, range->axis) << "Axis: " << axis << " Source: " << source;
        ASSERT_EQ(source, range->source) << "Axis: " << axis << " Source: " << source;
        ASSERT_NEAR(min, range->min, EPSILON) << "Axis: " << axis << " Source: " << source;
        ASSERT_NEAR(max, range->max, EPSILON) << "Axis: " << axis << " Source: " << source;
        ASSERT_NEAR(flat, range->flat, EPSILON) << "Axis: " << axis << " Source: " << source;
        ASSERT_NEAR(fuzz, range->fuzz, EPSILON) << "Axis: " << axis << " Source: " << source;
    }
    static void assertPointerCoords(const PointerCoords& coords,
            float x, float y, float pressure, float size,
            float touchMajor, float touchMinor, float toolMajor, float toolMinor,
            float orientation, float distance) {
        ASSERT_NEAR(x, coords.getAxisValue(AMOTION_EVENT_AXIS_X), 1);
        ASSERT_NEAR(y, coords.getAxisValue(AMOTION_EVENT_AXIS_Y), 1);
        ASSERT_NEAR(pressure, coords.getAxisValue(AMOTION_EVENT_AXIS_PRESSURE), EPSILON);
        ASSERT_NEAR(size, coords.getAxisValue(AMOTION_EVENT_AXIS_SIZE), EPSILON);
        ASSERT_NEAR(touchMajor, coords.getAxisValue(AMOTION_EVENT_AXIS_TOUCH_MAJOR), 1);
        ASSERT_NEAR(touchMinor, coords.getAxisValue(AMOTION_EVENT_AXIS_TOUCH_MINOR), 1);
        ASSERT_NEAR(toolMajor, coords.getAxisValue(AMOTION_EVENT_AXIS_TOOL_MAJOR), 1);
        ASSERT_NEAR(toolMinor, coords.getAxisValue(AMOTION_EVENT_AXIS_TOOL_MINOR), 1);
        ASSERT_NEAR(orientation, coords.getAxisValue(AMOTION_EVENT_AXIS_ORIENTATION), EPSILON);
        ASSERT_NEAR(distance, coords.getAxisValue(AMOTION_EVENT_AXIS_DISTANCE), EPSILON);
    }
    static void assertPosition(const sp<FakePointerController>& controller, float x, float y) {
        float actualX, actualY;
        controller->getPosition(&actualX, &actualY);
        ASSERT_NEAR(x, actualX, 1);
        ASSERT_NEAR(y, actualY, 1);
    }
};
const int32_t InputMapperTest::EVENTHUB_ID = 1;
const int32_t InputMapperTest::EVENTHUB_ID = 1;
const int32_t InputMapperTest::EVENTHUB_ID = 1;
const int32_t InputMapperTest::EVENTHUB_ID = 1;
const int32_t InputMapperTest::EVENTHUB_ID = 1;
const int32_t InputMapperTest::EVENTHUB_ID = 1;
const int32_t InputMapperTest::EVENTHUB_ID = 1;
class SwitchInputMapperTest : public InputMapperTest {
};
TEST_F(SwitchInputMapperTest, Process) {
    SwitchInputMapper& mapper = addMapperAndConfigure<SwitchInputMapper>();
    process(mapper, ARBITRARY_TIME, EV_SW, SW_LID, 1);
    process(mapper, ARBITRARY_TIME, EV_SW, SW_JACK_PHYSICAL_INSERT, 1);
    process(mapper, ARBITRARY_TIME, EV_SW, SW_HEADPHONE_INSERT, 0);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    NotifySwitchArgs args;
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifySwitchWasCalled(&args));
    ASSERT_EQ(ARBITRARY_TIME, args.eventTime);
    ASSERT_EQ((1U << SW_LID) | (1U << SW_JACK_PHYSICAL_INSERT), args.switchValues);
    ASSERT_EQ((1U << SW_LID) | (1U << SW_JACK_PHYSICAL_INSERT) | (1 << SW_HEADPHONE_INSERT),
            args.switchMask);
    ASSERT_EQ(uint32_t(0), args.policyFlags);
}
TEST_F(SwitchInputMapperTest, Process) {
    SwitchInputMapper& mapper = addMapperAndConfigure<SwitchInputMapper>();
    process(mapper, ARBITRARY_TIME, EV_SW, SW_LID, 1);
    process(mapper, ARBITRARY_TIME, EV_SW, SW_JACK_PHYSICAL_INSERT, 1);
    process(mapper, ARBITRARY_TIME, EV_SW, SW_HEADPHONE_INSERT, 0);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    NotifySwitchArgs args;
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifySwitchWasCalled(&args));
    ASSERT_EQ(ARBITRARY_TIME, args.eventTime);
    ASSERT_EQ((1U << SW_LID) | (1U << SW_JACK_PHYSICAL_INSERT), args.switchValues);
    ASSERT_EQ((1U << SW_LID) | (1U << SW_JACK_PHYSICAL_INSERT) | (1 << SW_HEADPHONE_INSERT),
            args.switchMask);
    ASSERT_EQ(uint32_t(0), args.policyFlags);
}
TEST_F(SwitchInputMapperTest, Process) {
    SwitchInputMapper& mapper = addMapperAndConfigure<SwitchInputMapper>();
    process(mapper, ARBITRARY_TIME, EV_SW, SW_LID, 1);
    process(mapper, ARBITRARY_TIME, EV_SW, SW_JACK_PHYSICAL_INSERT, 1);
    process(mapper, ARBITRARY_TIME, EV_SW, SW_HEADPHONE_INSERT, 0);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    NotifySwitchArgs args;
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifySwitchWasCalled(&args));
    ASSERT_EQ(ARBITRARY_TIME, args.eventTime);
    ASSERT_EQ((1U << SW_LID) | (1U << SW_JACK_PHYSICAL_INSERT), args.switchValues);
    ASSERT_EQ((1U << SW_LID) | (1U << SW_JACK_PHYSICAL_INSERT) | (1 << SW_HEADPHONE_INSERT),
            args.switchMask);
    ASSERT_EQ(uint32_t(0), args.policyFlags);
}
class KeyboardInputMapperTest : public InputMapperTest {
protected:
    const std::string UNIQUE_ID = "local:0";
    void prepareDisplay(int32_t orientation);
    void testDPadKeyRotation(KeyboardInputMapper& mapper, int32_t originalScanCode,
                             int32_t originalKeyCode, int32_t rotatedKeyCode,
                             int32_t displayId = ADISPLAY_ID_NONE);
};
void KeyboardInputMapperTest::prepareDisplay(int32_t orientation) {
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            orientation, UNIQUE_ID, NO_PORT, ViewportType::VIEWPORT_INTERNAL);
}
void KeyboardInputMapperTest::testDPadKeyRotation(KeyboardInputMapper& mapper,
                                                  int32_t originalScanCode, int32_t originalKeyCode,
                                                  int32_t rotatedKeyCode, int32_t displayId) {
    NotifyKeyArgs args;
    process(mapper, ARBITRARY_TIME, EV_KEY, originalScanCode, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AKEY_EVENT_ACTION_DOWN, args.action);
    ASSERT_EQ(originalScanCode, args.scanCode);
    ASSERT_EQ(rotatedKeyCode, args.keyCode);
    ASSERT_EQ(displayId, args.displayId);
    process(mapper, ARBITRARY_TIME, EV_KEY, originalScanCode, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AKEY_EVENT_ACTION_UP, args.action);
    ASSERT_EQ(originalScanCode, args.scanCode);
    ASSERT_EQ(rotatedKeyCode, args.keyCode);
    ASSERT_EQ(displayId, args.displayId);
}
TEST_F(KeyboardInputMapperTest, Configure_AssignsDisplayPort) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    const std::string USB2 = "USB2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    InputDeviceIdentifier identifier;
    identifier.name = "KEYBOARD2";
    identifier.location = USB2;
    std::unique_ptr<InputDevice> device2 =
            std::make_unique<InputDevice>(mFakeContext, SECOND_DEVICE_ID, DEVICE_GENERATION,
                                          identifier);
    mFakeEventHub->addDevice(SECOND_EVENTHUB_ID, DEVICE_NAME, 0 );
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    KeyboardInputMapper& mapper2 =
            device2->addMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID, AINPUT_SOURCE_KEYBOARD,
                                                    AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0 );
    device2->reset(ARBITRARY_TIME);
    constexpr uint8_t hdmi1 = 0;
    constexpr uint8_t hdmi2 = 1;
    const std::string SECONDARY_UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->addInputPortAssociation(USB2, hdmi2);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(device2->isEnabled());
    constexpr int32_t newDisplayId = 2;
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 UNIQUE_ID, hdmi1, ViewportType::VIEWPORT_INTERNAL);
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 SECONDARY_UNIQUE_ID, hdmi2, ViewportType::VIEWPORT_EXTERNAL);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    ASSERT_TRUE(device2->isEnabled());
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper2, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, newDisplayId));
}
TEST_F(KeyboardInputMapperTest, Configure_AssignsDisplayPort) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    const std::string USB2 = "USB2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    InputDeviceIdentifier identifier;
    identifier.name = "KEYBOARD2";
    identifier.location = USB2;
    std::unique_ptr<InputDevice> device2 =
            std::make_unique<InputDevice>(mFakeContext, SECOND_DEVICE_ID, DEVICE_GENERATION,
                                          identifier);
    mFakeEventHub->addDevice(SECOND_EVENTHUB_ID, DEVICE_NAME, 0 );
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    KeyboardInputMapper& mapper2 =
            device2->addMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID, AINPUT_SOURCE_KEYBOARD,
                                                    AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0 );
    device2->reset(ARBITRARY_TIME);
    constexpr uint8_t hdmi1 = 0;
    constexpr uint8_t hdmi2 = 1;
    const std::string SECONDARY_UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->addInputPortAssociation(USB2, hdmi2);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(device2->isEnabled());
    constexpr int32_t newDisplayId = 2;
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 UNIQUE_ID, hdmi1, ViewportType::VIEWPORT_INTERNAL);
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 SECONDARY_UNIQUE_ID, hdmi2, ViewportType::VIEWPORT_EXTERNAL);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    ASSERT_TRUE(device2->isEnabled());
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper2, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, newDisplayId));
}
TEST_F(KeyboardInputMapperTest, Configure_AssignsDisplayPort) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    const std::string USB2 = "USB2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    InputDeviceIdentifier identifier;
    identifier.name = "KEYBOARD2";
    identifier.location = USB2;
    std::unique_ptr<InputDevice> device2 =
            std::make_unique<InputDevice>(mFakeContext, SECOND_DEVICE_ID, DEVICE_GENERATION,
                                          identifier);
    mFakeEventHub->addDevice(SECOND_EVENTHUB_ID, DEVICE_NAME, 0 );
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    KeyboardInputMapper& mapper2 =
            device2->addMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID, AINPUT_SOURCE_KEYBOARD,
                                                    AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0 );
    device2->reset(ARBITRARY_TIME);
    constexpr uint8_t hdmi1 = 0;
    constexpr uint8_t hdmi2 = 1;
    const std::string SECONDARY_UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->addInputPortAssociation(USB2, hdmi2);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(device2->isEnabled());
    constexpr int32_t newDisplayId = 2;
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 UNIQUE_ID, hdmi1, ViewportType::VIEWPORT_INTERNAL);
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 SECONDARY_UNIQUE_ID, hdmi2, ViewportType::VIEWPORT_EXTERNAL);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    ASSERT_TRUE(device2->isEnabled());
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper2, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, newDisplayId));
}
TEST_F(KeyboardInputMapperTest, Configure_AssignsDisplayPort) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    const std::string USB2 = "USB2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    InputDeviceIdentifier identifier;
    identifier.name = "KEYBOARD2";
    identifier.location = USB2;
    std::unique_ptr<InputDevice> device2 =
            std::make_unique<InputDevice>(mFakeContext, SECOND_DEVICE_ID, DEVICE_GENERATION,
                                          identifier);
    mFakeEventHub->addDevice(SECOND_EVENTHUB_ID, DEVICE_NAME, 0 );
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    KeyboardInputMapper& mapper2 =
            device2->addMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID, AINPUT_SOURCE_KEYBOARD,
                                                    AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0 );
    device2->reset(ARBITRARY_TIME);
    constexpr uint8_t hdmi1 = 0;
    constexpr uint8_t hdmi2 = 1;
    const std::string SECONDARY_UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->addInputPortAssociation(USB2, hdmi2);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(device2->isEnabled());
    constexpr int32_t newDisplayId = 2;
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 UNIQUE_ID, hdmi1, ViewportType::VIEWPORT_INTERNAL);
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 SECONDARY_UNIQUE_ID, hdmi2, ViewportType::VIEWPORT_EXTERNAL);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    ASSERT_TRUE(device2->isEnabled());
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper2, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, newDisplayId));
}
TEST_F(KeyboardInputMapperTest, Configure_AssignsDisplayPort) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    const std::string USB2 = "USB2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    InputDeviceIdentifier identifier;
    identifier.name = "KEYBOARD2";
    identifier.location = USB2;
    std::unique_ptr<InputDevice> device2 =
            std::make_unique<InputDevice>(mFakeContext, SECOND_DEVICE_ID, DEVICE_GENERATION,
                                          identifier);
    mFakeEventHub->addDevice(SECOND_EVENTHUB_ID, DEVICE_NAME, 0 );
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    KeyboardInputMapper& mapper2 =
            device2->addMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID, AINPUT_SOURCE_KEYBOARD,
                                                    AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0 );
    device2->reset(ARBITRARY_TIME);
    constexpr uint8_t hdmi1 = 0;
    constexpr uint8_t hdmi2 = 1;
    const std::string SECONDARY_UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->addInputPortAssociation(USB2, hdmi2);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(device2->isEnabled());
    constexpr int32_t newDisplayId = 2;
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 UNIQUE_ID, hdmi1, ViewportType::VIEWPORT_INTERNAL);
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 SECONDARY_UNIQUE_ID, hdmi2, ViewportType::VIEWPORT_EXTERNAL);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    ASSERT_TRUE(device2->isEnabled());
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper2, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, newDisplayId));
}
TEST_F(KeyboardInputMapperTest, Configure_AssignsDisplayPort) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    const std::string USB2 = "USB2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    InputDeviceIdentifier identifier;
    identifier.name = "KEYBOARD2";
    identifier.location = USB2;
    std::unique_ptr<InputDevice> device2 =
            std::make_unique<InputDevice>(mFakeContext, SECOND_DEVICE_ID, DEVICE_GENERATION,
                                          identifier);
    mFakeEventHub->addDevice(SECOND_EVENTHUB_ID, DEVICE_NAME, 0 );
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    KeyboardInputMapper& mapper2 =
            device2->addMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID, AINPUT_SOURCE_KEYBOARD,
                                                    AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0 );
    device2->reset(ARBITRARY_TIME);
    constexpr uint8_t hdmi1 = 0;
    constexpr uint8_t hdmi2 = 1;
    const std::string SECONDARY_UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->addInputPortAssociation(USB2, hdmi2);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(device2->isEnabled());
    constexpr int32_t newDisplayId = 2;
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 UNIQUE_ID, hdmi1, ViewportType::VIEWPORT_INTERNAL);
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 SECONDARY_UNIQUE_ID, hdmi2, ViewportType::VIEWPORT_EXTERNAL);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    ASSERT_TRUE(device2->isEnabled());
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper2, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, newDisplayId));
}
TEST_F(KeyboardInputMapperTest, Configure_AssignsDisplayPort) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    const std::string USB2 = "USB2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    InputDeviceIdentifier identifier;
    identifier.name = "KEYBOARD2";
    identifier.location = USB2;
    std::unique_ptr<InputDevice> device2 =
            std::make_unique<InputDevice>(mFakeContext, SECOND_DEVICE_ID, DEVICE_GENERATION,
                                          identifier);
    mFakeEventHub->addDevice(SECOND_EVENTHUB_ID, DEVICE_NAME, 0 );
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    KeyboardInputMapper& mapper2 =
            device2->addMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID, AINPUT_SOURCE_KEYBOARD,
                                                    AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0 );
    device2->reset(ARBITRARY_TIME);
    constexpr uint8_t hdmi1 = 0;
    constexpr uint8_t hdmi2 = 1;
    const std::string SECONDARY_UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->addInputPortAssociation(USB2, hdmi2);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(device2->isEnabled());
    constexpr int32_t newDisplayId = 2;
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 UNIQUE_ID, hdmi1, ViewportType::VIEWPORT_INTERNAL);
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 SECONDARY_UNIQUE_ID, hdmi2, ViewportType::VIEWPORT_EXTERNAL);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    ASSERT_TRUE(device2->isEnabled());
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper2, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, newDisplayId));
}
TEST_F(KeyboardInputMapperTest, Configure_AssignsDisplayPort) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    const std::string USB2 = "USB2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    InputDeviceIdentifier identifier;
    identifier.name = "KEYBOARD2";
    identifier.location = USB2;
    std::unique_ptr<InputDevice> device2 =
            std::make_unique<InputDevice>(mFakeContext, SECOND_DEVICE_ID, DEVICE_GENERATION,
                                          identifier);
    mFakeEventHub->addDevice(SECOND_EVENTHUB_ID, DEVICE_NAME, 0 );
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    KeyboardInputMapper& mapper2 =
            device2->addMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID, AINPUT_SOURCE_KEYBOARD,
                                                    AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0 );
    device2->reset(ARBITRARY_TIME);
    constexpr uint8_t hdmi1 = 0;
    constexpr uint8_t hdmi2 = 1;
    const std::string SECONDARY_UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->addInputPortAssociation(USB2, hdmi2);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(device2->isEnabled());
    constexpr int32_t newDisplayId = 2;
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 UNIQUE_ID, hdmi1, ViewportType::VIEWPORT_INTERNAL);
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 SECONDARY_UNIQUE_ID, hdmi2, ViewportType::VIEWPORT_EXTERNAL);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    ASSERT_TRUE(device2->isEnabled());
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper2, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, newDisplayId));
}
TEST_F(KeyboardInputMapperTest, Configure_AssignsDisplayPort) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    const std::string USB2 = "USB2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    InputDeviceIdentifier identifier;
    identifier.name = "KEYBOARD2";
    identifier.location = USB2;
    std::unique_ptr<InputDevice> device2 =
            std::make_unique<InputDevice>(mFakeContext, SECOND_DEVICE_ID, DEVICE_GENERATION,
                                          identifier);
    mFakeEventHub->addDevice(SECOND_EVENTHUB_ID, DEVICE_NAME, 0 );
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    KeyboardInputMapper& mapper2 =
            device2->addMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID, AINPUT_SOURCE_KEYBOARD,
                                                    AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0 );
    device2->reset(ARBITRARY_TIME);
    constexpr uint8_t hdmi1 = 0;
    constexpr uint8_t hdmi2 = 1;
    const std::string SECONDARY_UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->addInputPortAssociation(USB2, hdmi2);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(device2->isEnabled());
    constexpr int32_t newDisplayId = 2;
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 UNIQUE_ID, hdmi1, ViewportType::VIEWPORT_INTERNAL);
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 SECONDARY_UNIQUE_ID, hdmi2, ViewportType::VIEWPORT_EXTERNAL);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    ASSERT_TRUE(device2->isEnabled());
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper2, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, newDisplayId));
}
TEST_F(KeyboardInputMapperTest, Configure_AssignsDisplayPort) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    const std::string USB2 = "USB2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    InputDeviceIdentifier identifier;
    identifier.name = "KEYBOARD2";
    identifier.location = USB2;
    std::unique_ptr<InputDevice> device2 =
            std::make_unique<InputDevice>(mFakeContext, SECOND_DEVICE_ID, DEVICE_GENERATION,
                                          identifier);
    mFakeEventHub->addDevice(SECOND_EVENTHUB_ID, DEVICE_NAME, 0 );
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    KeyboardInputMapper& mapper2 =
            device2->addMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID, AINPUT_SOURCE_KEYBOARD,
                                                    AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0 );
    device2->reset(ARBITRARY_TIME);
    constexpr uint8_t hdmi1 = 0;
    constexpr uint8_t hdmi2 = 1;
    const std::string SECONDARY_UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->addInputPortAssociation(USB2, hdmi2);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(device2->isEnabled());
    constexpr int32_t newDisplayId = 2;
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 UNIQUE_ID, hdmi1, ViewportType::VIEWPORT_INTERNAL);
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 SECONDARY_UNIQUE_ID, hdmi2, ViewportType::VIEWPORT_EXTERNAL);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    ASSERT_TRUE(device2->isEnabled());
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper2, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, newDisplayId));
}
TEST_F(KeyboardInputMapperTest, Configure_AssignsDisplayPort) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    const std::string USB2 = "USB2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    InputDeviceIdentifier identifier;
    identifier.name = "KEYBOARD2";
    identifier.location = USB2;
    std::unique_ptr<InputDevice> device2 =
            std::make_unique<InputDevice>(mFakeContext, SECOND_DEVICE_ID, DEVICE_GENERATION,
                                          identifier);
    mFakeEventHub->addDevice(SECOND_EVENTHUB_ID, DEVICE_NAME, 0 );
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    KeyboardInputMapper& mapper2 =
            device2->addMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID, AINPUT_SOURCE_KEYBOARD,
                                                    AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0 );
    device2->reset(ARBITRARY_TIME);
    constexpr uint8_t hdmi1 = 0;
    constexpr uint8_t hdmi2 = 1;
    const std::string SECONDARY_UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->addInputPortAssociation(USB2, hdmi2);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(device2->isEnabled());
    constexpr int32_t newDisplayId = 2;
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 UNIQUE_ID, hdmi1, ViewportType::VIEWPORT_INTERNAL);
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 SECONDARY_UNIQUE_ID, hdmi2, ViewportType::VIEWPORT_EXTERNAL);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    ASSERT_TRUE(device2->isEnabled());
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper2, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, newDisplayId));
}
TEST_F(KeyboardInputMapperTest, Configure_AssignsDisplayPort) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    const std::string USB2 = "USB2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    InputDeviceIdentifier identifier;
    identifier.name = "KEYBOARD2";
    identifier.location = USB2;
    std::unique_ptr<InputDevice> device2 =
            std::make_unique<InputDevice>(mFakeContext, SECOND_DEVICE_ID, DEVICE_GENERATION,
                                          identifier);
    mFakeEventHub->addDevice(SECOND_EVENTHUB_ID, DEVICE_NAME, 0 );
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    KeyboardInputMapper& mapper2 =
            device2->addMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID, AINPUT_SOURCE_KEYBOARD,
                                                    AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), 0 );
    device2->reset(ARBITRARY_TIME);
    constexpr uint8_t hdmi1 = 0;
    constexpr uint8_t hdmi2 = 1;
    const std::string SECONDARY_UNIQUE_ID = "local:1";
    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->addInputPortAssociation(USB2, hdmi2);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_FALSE(device2->isEnabled());
    constexpr int32_t newDisplayId = 2;
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 UNIQUE_ID, hdmi1, ViewportType::VIEWPORT_INTERNAL);
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ORIENTATION_0,
                                 SECONDARY_UNIQUE_ID, hdmi2, ViewportType::VIEWPORT_EXTERNAL);
    device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                       InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    ASSERT_TRUE(mDevice->isEnabled());
    ASSERT_TRUE(device2->isEnabled());
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper2, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, newDisplayId));
}
class KeyboardInputMapperTest_ExternalDevice : public InputMapperTest {
protected:
    virtual void SetUp() override {
        InputMapperTest::SetUp(DEVICE_CLASSES | INPUT_DEVICE_CLASS_EXTERNAL);
    }
};
TEST_F(KeyboardInputMapperTest_ExternalDevice, DoNotWakeByDefaultBehavior) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_HOME, 0, AKEYCODE_HOME, POLICY_FLAG_WAKE);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_PLAY, 0, AKEYCODE_MEDIA_PLAY, POLICY_FLAG_WAKE);
    addConfigurationProperty("keyboard.doNotWakeByDefault", "1");
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    process(mapper, ARBITRARY_TIME, EV_KEY, KEY_HOME, 1);
    NotifyKeyArgs args;
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
    process(mapper, ARBITRARY_TIME + 1, EV_KEY, KEY_HOME, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
    process(mapper, ARBITRARY_TIME, EV_KEY, KEY_DOWN, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(uint32_t(0), args.policyFlags);
    process(mapper, ARBITRARY_TIME + 1, EV_KEY, KEY_DOWN, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(uint32_t(0), args.policyFlags);
    process(mapper, ARBITRARY_TIME, EV_KEY, KEY_PLAY, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
    process(mapper, ARBITRARY_TIME + 1, EV_KEY, KEY_PLAY, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
}
TEST_F(KeyboardInputMapperTest_ExternalDevice, DoNotWakeByDefaultBehavior) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_HOME, 0, AKEYCODE_HOME, POLICY_FLAG_WAKE);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_PLAY, 0, AKEYCODE_MEDIA_PLAY, POLICY_FLAG_WAKE);
    addConfigurationProperty("keyboard.doNotWakeByDefault", "1");
    KeyboardInputMapper& mapper =
            addMapperAndConfigure<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD,
                                                       AINPUT_KEYBOARD_TYPE_ALPHABETIC);
    process(mapper, ARBITRARY_TIME, EV_KEY, KEY_HOME, 1);
    NotifyKeyArgs args;
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
    process(mapper, ARBITRARY_TIME + 1, EV_KEY, KEY_HOME, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
    process(mapper, ARBITRARY_TIME, EV_KEY, KEY_DOWN, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(uint32_t(0), args.policyFlags);
    process(mapper, ARBITRARY_TIME + 1, EV_KEY, KEY_DOWN, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(uint32_t(0), args.policyFlags);
    process(mapper, ARBITRARY_TIME, EV_KEY, KEY_PLAY, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
    process(mapper, ARBITRARY_TIME + 1, EV_KEY, KEY_PLAY, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
}
class CursorInputMapperTest : public InputMapperTest {
protected:
    static const int32_t TRACKBALL_MOVEMENT_THRESHOLD;
    sp<FakePointerController> mFakePointerController;
    virtual void SetUp() override {
        InputMapperTest::SetUp();
        mFakePointerController = new FakePointerController();
        mFakePolicy->setPointerController(mDevice->getId(), mFakePointerController);
    }
    void testMotionRotation(CursorInputMapper& mapper, int32_t originalX, int32_t originalY,
                            int32_t rotatedX, int32_t rotatedY);
    void prepareDisplay(int32_t orientation) {
        const std::string uniqueId = "local:0";
        const ViewportType viewportType = ViewportType::VIEWPORT_INTERNAL;
        setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                orientation, uniqueId, NO_PORT, viewportType);
    }
};
const int32_t CursorInputMapperTest::TRACKBALL_MOVEMENT_THRESHOLD = 6;
void CursorInputMapperTest::testMotionRotation(CursorInputMapper& mapper, int32_t originalX,
                                               int32_t originalY, int32_t rotatedX,
                                               int32_t rotatedY) {
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, originalX);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, originalY);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AMOTION_EVENT_ACTION_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            float(rotatedX) / TRACKBALL_MOVEMENT_THRESHOLD,
            float(rotatedY) / TRACKBALL_MOVEMENT_THRESHOLD,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
TEST_F(CursorInputMapperTest, Process_ShouldHandleDisplayId) {
    CursorInputMapper& mapper = addMapperAndConfigure<CursorInputMapper>();
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    const std::string SECOND_DISPLAY_UNIQUE_ID = "local:1";
    mFakePolicy->addDisplayViewport(SECOND_DISPLAY_ID, 800, 480, DISPLAY_ORIENTATION_0,
                                    SECOND_DISPLAY_UNIQUE_ID, NO_PORT,
                                    ViewportType::VIEWPORT_EXTERNAL);
    mFakePolicy->setDefaultPointerDisplayId(SECOND_DISPLAY_ID);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
    mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
    mFakePointerController->setPosition(100, 200);
    mFakePointerController->setButtonState(0);
    NotifyMotionArgs args;
    process(mapper, ARBITRARY_TIME, EV_REL, REL_X, 10);
    process(mapper, ARBITRARY_TIME, EV_REL, REL_Y, 20);
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&args));
    ASSERT_EQ(AINPUT_SOURCE_MOUSE, args.source);
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, args.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(args.pointerCoords[0],
            110.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    ASSERT_NO_FATAL_FAILURE(assertPosition(mFakePointerController, 110.0f, 220.0f));
    ASSERT_EQ(SECOND_DISPLAY_ID, args.displayId);
}
class TouchInputMapperTest : public InputMapperTest {
protected:
    static const int32_t RAW_X_MIN;
    static const int32_t RAW_X_MAX;
    static const int32_t RAW_Y_MIN;
    static const int32_t RAW_Y_MAX;
    static const int32_t RAW_TOUCH_MIN;
    static const int32_t RAW_TOUCH_MAX;
    static const int32_t RAW_TOOL_MIN;
    static const int32_t RAW_TOOL_MAX;
    static const int32_t RAW_PRESSURE_MIN;
    static const int32_t RAW_PRESSURE_MAX;
    static const int32_t RAW_ORIENTATION_MIN;
    static const int32_t RAW_ORIENTATION_MAX;
    static const int32_t RAW_DISTANCE_MIN;
    static const int32_t RAW_DISTANCE_MAX;
    static const int32_t RAW_TILT_MIN;
    static const int32_t RAW_TILT_MAX;
    static const int32_t RAW_ID_MIN;
    static const int32_t RAW_ID_MAX;
    static const int32_t RAW_SLOT_MIN;
    static const int32_t RAW_SLOT_MAX;
    static const float X_PRECISION;
    static const float Y_PRECISION;
    static const float X_PRECISION_VIRTUAL;
    static const float Y_PRECISION_VIRTUAL;
    static const float GEOMETRIC_SCALE;
    static const TouchAffineTransformation AFFINE_TRANSFORM;
    static const VirtualKeyDefinition VIRTUAL_KEYS[2];
    const std::string UNIQUE_ID = "local:0";
    const std::string SECONDARY_UNIQUE_ID = "local:1";
    enum Axes {
        POSITION = 1 << 0,
        TOUCH = 1 << 1,
        TOOL = 1 << 2,
        PRESSURE = 1 << 3,
        ORIENTATION = 1 << 4,
        MINOR = 1 << 5,
        ID = 1 << 6,
        DISTANCE = 1 << 7,
        TILT = 1 << 8,
        SLOT = 1 << 9,
        TOOL_TYPE = 1 << 10,
    };
    void prepareDisplay(int32_t orientation, std::optional<uint8_t> port = NO_PORT);
    void prepareSecondaryDisplay(ViewportType type, std::optional<uint8_t> port = NO_PORT);
    void prepareVirtualDisplay(int32_t orientation);
    void prepareVirtualKeys();
    void prepareLocationCalibration();
    int32_t toRawX(float displayX);
    int32_t toRawY(float displayY);
    float toCookedX(float rawX, float rawY);
    float toCookedY(float rawX, float rawY);
    float toDisplayX(int32_t rawX);
    float toDisplayX(int32_t rawX, int32_t displayWidth);
    float toDisplayY(int32_t rawY);
    float toDisplayY(int32_t rawY, int32_t displayHeight);
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
const VirtualKeyDefinition TouchInputMapperTest::VIRTUAL_KEYS[2] = {
        { KEY_HOME, 60, DISPLAY_HEIGHT + 15, 20, 20 },
        { KEY_MENU, DISPLAY_HEIGHT - 60, DISPLAY_WIDTH + 15, 20, 20 },
};
void TouchInputMapperTest::prepareDisplay(int32_t orientation, std::optional<uint8_t> port) {
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, orientation,
            UNIQUE_ID, port, ViewportType::VIEWPORT_INTERNAL);
}
void TouchInputMapperTest::prepareSecondaryDisplay(ViewportType type, std::optional<uint8_t> port) {
    setDisplayInfoAndReconfigure(SECONDARY_DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_ORIENTATION_0, SECONDARY_UNIQUE_ID, port, type);
}
void TouchInputMapperTest::prepareVirtualDisplay(int32_t orientation) {
    setDisplayInfoAndReconfigure(VIRTUAL_DISPLAY_ID, VIRTUAL_DISPLAY_WIDTH,
        VIRTUAL_DISPLAY_HEIGHT, orientation,
        VIRTUAL_DISPLAY_UNIQUE_ID, NO_PORT, ViewportType::VIEWPORT_VIRTUAL);
}
void TouchInputMapperTest::prepareVirtualKeys() {
    mFakeEventHub->addVirtualKeyDefinition(EVENTHUB_ID, VIRTUAL_KEYS[0]);
    mFakeEventHub->addVirtualKeyDefinition(EVENTHUB_ID, VIRTUAL_KEYS[1]);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_HOME, 0, AKEYCODE_HOME, POLICY_FLAG_WAKE);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_MENU, 0, AKEYCODE_MENU, POLICY_FLAG_WAKE);
}
void TouchInputMapperTest::prepareLocationCalibration() {
    mFakePolicy->setTouchAffineTransformation(AFFINE_TRANSFORM);
}
int32_t TouchInputMapperTest::toRawX(float displayX) {
    return int32_t(displayX * (RAW_X_MAX - RAW_X_MIN + 1) / DISPLAY_WIDTH + RAW_X_MIN);
}
int32_t TouchInputMapperTest::toRawY(float displayY) {
    return int32_t(displayY * (RAW_Y_MAX - RAW_Y_MIN + 1) / DISPLAY_HEIGHT + RAW_Y_MIN);
}
float TouchInputMapperTest::toCookedX(float rawX, float rawY) {
    AFFINE_TRANSFORM.applyTo(rawX, rawY);
    return rawX;
}
float TouchInputMapperTest::toCookedY(float rawX, float rawY) {
    AFFINE_TRANSFORM.applyTo(rawX, rawY);
    return rawY;
}
float TouchInputMapperTest::toDisplayX(int32_t rawX) {
    return toDisplayX(rawX, DISPLAY_WIDTH);
}
float TouchInputMapperTest::toDisplayX(int32_t rawX, int32_t displayWidth) {
    return float(rawX - RAW_X_MIN) * displayWidth / (RAW_X_MAX - RAW_X_MIN + 1);
}
float TouchInputMapperTest::toDisplayY(int32_t rawY) {
    return toDisplayY(rawY, DISPLAY_HEIGHT);
}
float TouchInputMapperTest::toDisplayY(int32_t rawY, int32_t displayHeight) {
    return float(rawY - RAW_Y_MIN) * displayHeight / (RAW_Y_MAX - RAW_Y_MIN + 1);
}
class SingleTouchInputMapperTest : public TouchInputMapperTest {
protected:
    void prepareButtons();
    void prepareAxes(int axes);
    void processDown(SingleTouchInputMapper& mapper, int32_t x, int32_t y);
    void processMove(SingleTouchInputMapper& mapper, int32_t x, int32_t y);
    void processUp(SingleTouchInputMapper& mappery);
    void processPressure(SingleTouchInputMapper& mapper, int32_t pressure);
    void processToolMajor(SingleTouchInputMapper& mapper, int32_t toolMajor);
    void processDistance(SingleTouchInputMapper& mapper, int32_t distance);
    void processTilt(SingleTouchInputMapper& mapper, int32_t tiltX, int32_t tiltY);
    void processKey(SingleTouchInputMapper& mapper, int32_t code, int32_t value);
    void processSync(SingleTouchInputMapper& mapper);
};
void SingleTouchInputMapperTest::prepareButtons() {
    mFakeEventHub->addKey(EVENTHUB_ID, BTN_TOUCH, 0, AKEYCODE_UNKNOWN, 0);
}
void SingleTouchInputMapperTest::prepareAxes(int axes) {
    if (axes & POSITION) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_X, RAW_X_MIN, RAW_X_MAX, 0, 0);
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_Y, RAW_Y_MIN, RAW_Y_MAX, 0, 0);
    }
    if (axes & PRESSURE) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_PRESSURE, RAW_PRESSURE_MIN,
                                       RAW_PRESSURE_MAX, 0, 0);
    }
    if (axes & TOOL) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_TOOL_WIDTH, RAW_TOOL_MIN, RAW_TOOL_MAX, 0,
                                       0);
    }
    if (axes & DISTANCE) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_DISTANCE, RAW_DISTANCE_MIN,
                                       RAW_DISTANCE_MAX, 0, 0);
    }
    if (axes & TILT) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_TILT_X, RAW_TILT_MIN, RAW_TILT_MAX, 0, 0);
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_TILT_Y, RAW_TILT_MIN, RAW_TILT_MAX, 0, 0);
    }
}
void SingleTouchInputMapperTest::processDown(SingleTouchInputMapper& mapper, int32_t x, int32_t y) {
    process(mapper, ARBITRARY_TIME, EV_KEY, BTN_TOUCH, 1);
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_X, x);
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_Y, y);
}
void SingleTouchInputMapperTest::processMove(SingleTouchInputMapper& mapper, int32_t x, int32_t y) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_X, x);
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_Y, y);
}
void SingleTouchInputMapperTest::processUp(SingleTouchInputMapper& mapper) {
    process(mapper, ARBITRARY_TIME, EV_KEY, BTN_TOUCH, 0);
}
void SingleTouchInputMapperTest::processPressure(SingleTouchInputMapper& mapper, int32_t pressure) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_PRESSURE, pressure);
}
void SingleTouchInputMapperTest::processToolMajor(SingleTouchInputMapper& mapper,
                                                  int32_t toolMajor) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_TOOL_WIDTH, toolMajor);
}
void SingleTouchInputMapperTest::processDistance(SingleTouchInputMapper& mapper, int32_t distance) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_DISTANCE, distance);
}
void SingleTouchInputMapperTest::processTilt(SingleTouchInputMapper& mapper, int32_t tiltX,
                                             int32_t tiltY) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_TILT_X, tiltX);
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_TILT_Y, tiltY);
}
void SingleTouchInputMapperTest::processKey(SingleTouchInputMapper& mapper, int32_t code,
                                            int32_t value) {
    process(mapper, ARBITRARY_TIME, EV_KEY, code, value);
}
void SingleTouchInputMapperTest::processSync(SingleTouchInputMapper& mapper) {
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
TEST_F(SingleTouchInputMapperTest, Process_WhenAbsPressureIsPresent_HoversIfItsValueIsZero) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareButtons();
    prepareAxes(POSITION | PRESSURE);
    SingleTouchInputMapper& mapper = addMapperAndConfigure<SingleTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    processDown(mapper, 100, 200);
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(100), toDisplayY(200), 0, 0, 0, 0, 0, 0, 0, 0));
    processMove(mapper, 150, 250);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, RAW_PRESSURE_MAX);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    processPressure(mapper, 0);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_UP, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 1, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_ENTER, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_MOVE, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
    processUp(mapper);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_HOVER_EXIT, motionArgs.action);
    ASSERT_NO_FATAL_FAILURE(assertPointerCoords(motionArgs.pointerCoords[0],
            toDisplayX(150), toDisplayY(250), 0, 0, 0, 0, 0, 0, 0, 0));
}
class MultiTouchInputMapperTest : public TouchInputMapperTest {
protected:
    void prepareAxes(int axes);
    void processPosition(MultiTouchInputMapper& mapper, int32_t x, int32_t y);
    void processTouchMajor(MultiTouchInputMapper& mapper, int32_t touchMajor);
    void processTouchMinor(MultiTouchInputMapper& mapper, int32_t touchMinor);
    void processToolMajor(MultiTouchInputMapper& mapper, int32_t toolMajor);
    void processToolMinor(MultiTouchInputMapper& mapper, int32_t toolMinor);
    void processOrientation(MultiTouchInputMapper& mapper, int32_t orientation);
    void processPressure(MultiTouchInputMapper& mapper, int32_t pressure);
    void processDistance(MultiTouchInputMapper& mapper, int32_t distance);
    void processId(MultiTouchInputMapper& mapper, int32_t id);
    void processSlot(MultiTouchInputMapper& mapper, int32_t slot);
    void processToolType(MultiTouchInputMapper& mapper, int32_t toolType);
    void processKey(MultiTouchInputMapper& mapper, int32_t code, int32_t value);
    void processMTSync(MultiTouchInputMapper& mapper);
    void processSync(MultiTouchInputMapper& mapper);
};
void MultiTouchInputMapperTest::prepareAxes(int axes) {
    if (axes & POSITION) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_POSITION_X, RAW_X_MIN, RAW_X_MAX, 0, 0);
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_POSITION_Y, RAW_Y_MIN, RAW_Y_MAX, 0, 0);
    }
    if (axes & TOUCH) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_TOUCH_MAJOR, RAW_TOUCH_MIN,
                                       RAW_TOUCH_MAX, 0, 0);
        if (axes & MINOR) {
            mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_TOUCH_MINOR, RAW_TOUCH_MIN,
                                           RAW_TOUCH_MAX, 0, 0);
        }
    }
    if (axes & TOOL) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_WIDTH_MAJOR, RAW_TOOL_MIN, RAW_TOOL_MAX,
                                       0, 0);
        if (axes & MINOR) {
            mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_WIDTH_MINOR, RAW_TOOL_MAX,
                                           RAW_TOOL_MAX, 0, 0);
        }
    }
    if (axes & ORIENTATION) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_ORIENTATION, RAW_ORIENTATION_MIN,
                                       RAW_ORIENTATION_MAX, 0, 0);
    }
    if (axes & PRESSURE) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_PRESSURE, RAW_PRESSURE_MIN,
                                       RAW_PRESSURE_MAX, 0, 0);
    }
    if (axes & DISTANCE) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_DISTANCE, RAW_DISTANCE_MIN,
                                       RAW_DISTANCE_MAX, 0, 0);
    }
    if (axes & ID) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_TRACKING_ID, RAW_ID_MIN, RAW_ID_MAX, 0,
                                       0);
    }
    if (axes & SLOT) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_SLOT, RAW_SLOT_MIN, RAW_SLOT_MAX, 0, 0);
        mFakeEventHub->setAbsoluteAxisValue(EVENTHUB_ID, ABS_MT_SLOT, 0);
    }
    if (axes & TOOL_TYPE) {
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_TOOL_TYPE, 0, MT_TOOL_MAX, 0, 0);
    }
}
void MultiTouchInputMapperTest::processPosition(MultiTouchInputMapper& mapper, int32_t x,
                                                int32_t y) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_MT_POSITION_X, x);
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_MT_POSITION_Y, y);
}
void MultiTouchInputMapperTest::processTouchMajor(MultiTouchInputMapper& mapper,
                                                  int32_t touchMajor) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_MT_TOUCH_MAJOR, touchMajor);
}
void MultiTouchInputMapperTest::processTouchMinor(MultiTouchInputMapper& mapper,
                                                  int32_t touchMinor) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_MT_TOUCH_MINOR, touchMinor);
}
void MultiTouchInputMapperTest::processToolMajor(MultiTouchInputMapper& mapper, int32_t toolMajor) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_MT_WIDTH_MAJOR, toolMajor);
}
void MultiTouchInputMapperTest::processToolMinor(MultiTouchInputMapper& mapper, int32_t toolMinor) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_MT_WIDTH_MINOR, toolMinor);
}
void MultiTouchInputMapperTest::processOrientation(MultiTouchInputMapper& mapper,
                                                   int32_t orientation) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_MT_ORIENTATION, orientation);
}
void MultiTouchInputMapperTest::processPressure(MultiTouchInputMapper& mapper, int32_t pressure) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_MT_PRESSURE, pressure);
}
void MultiTouchInputMapperTest::processDistance(MultiTouchInputMapper& mapper, int32_t distance) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_MT_DISTANCE, distance);
}
void MultiTouchInputMapperTest::processId(MultiTouchInputMapper& mapper, int32_t id) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_MT_TRACKING_ID, id);
}
void MultiTouchInputMapperTest::processSlot(MultiTouchInputMapper& mapper, int32_t slot) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_MT_SLOT, slot);
}
void MultiTouchInputMapperTest::processToolType(MultiTouchInputMapper& mapper, int32_t toolType) {
    process(mapper, ARBITRARY_TIME, EV_ABS, ABS_MT_TOOL_TYPE, toolType);
}
void MultiTouchInputMapperTest::processKey(MultiTouchInputMapper& mapper, int32_t code,
                                           int32_t value) {
    process(mapper, ARBITRARY_TIME, EV_KEY, code, value);
}
void MultiTouchInputMapperTest::processMTSync(MultiTouchInputMapper& mapper) {
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_MT_REPORT, 0);
}
void MultiTouchInputMapperTest::processSync(MultiTouchInputMapper& mapper) {
    process(mapper, ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
TEST_F(MultiTouchInputMapperTest, Process_ShouldHandlePalmToolType) {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper {
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    prepareAxes(POSITION | ID | SLOT | TOOL_TYPE);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    NotifyMotionArgs motionArgs;
    constexpr int32_t x1 = 100, y1 = 200, x2 = 120, y2 = 220, x3 = 140, y3 = 240;
    processId(mapper, 1);
    processPosition(mapper, x1, y1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
    processToolType(mapper, MT_TOOL_PALM);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionArgs.action);
    processId(mapper, 1);
    processPosition(mapper, x2, y2);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processId(mapper, -1);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasNotCalled());
    processToolType(mapper, MT_TOOL_FINGER);
    processId(mapper, 1);
    processPosition(mapper, x3, y3);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(AMOTION_EVENT_ACTION_DOWN, motionArgs.action);
    ASSERT_EQ(AMOTION_EVENT_TOOL_TYPE_FINGER, motionArgs.pointerProperties[0].toolType);
}
class MultiTouchInputMapperTest_ExternalDevice : public MultiTouchInputMapperTest {
protected:
    virtual void SetUp() override {
        InputMapperTest::SetUp(DEVICE_CLASSES | INPUT_DEVICE_CLASS_EXTERNAL);
    }
};
TEST_F(MultiTouchInputMapperTest_ExternalDevice, Viewports_Fallback) {
    prepareAxes(POSITION);
    addConfigurationProperty("touch.deviceType", "touchScreen");
    prepareDisplay(DISPLAY_ORIENTATION_0);
    MultiTouchInputMapper& mapper = addMapperAndConfigure<MultiTouchInputMapper>();
    ASSERT_EQ(AINPUT_SOURCE_TOUCHSCREEN, mapper.getSources());
    NotifyMotionArgs motionArgs;
    processPosition(mapper, 100, 100);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(ADISPLAY_ID_DEFAULT, motionArgs.displayId);
    prepareSecondaryDisplay(ViewportType::VIEWPORT_EXTERNAL);
    processPosition(mapper, 100, 100);
    processSync(mapper);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyMotionWasCalled(&motionArgs));
    ASSERT_EQ(SECONDARY_DISPLAY_ID, motionArgs.displayId);
}
}
