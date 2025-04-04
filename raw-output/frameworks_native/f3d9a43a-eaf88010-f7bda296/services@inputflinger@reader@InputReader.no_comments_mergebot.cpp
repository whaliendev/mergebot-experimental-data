#include "Macros.h"
#include "InputReader.h"
#include <android-base/stringprintf.h>
#include <errno.h>
#include <input/Keyboard.h>
#include <input/VirtualKeyMap.h>
#include <inttypes.h>
#include <limits.h>
#include <log/log.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <utils/Errors.h>
#include <utils/Thread.h>
#include "InputDevice.h"
using android::base::StringPrintf;
namespace android {
InputReader::InputReader(std::shared_ptr<EventHubInterface> eventHub, const sp<InputReaderPolicyInterface>& policy, const sp<InputListenerInterface>& listener): mContext(this), mEventHub(eventHub), mPolicy(policy), mNextId(1), mNextSequenceNum(1), mGlobalMetaState(0), mGeneration(1), mNextInputDeviceId(END_RESERVED_ID), mDisableVirtualKeysTimeout(LLONG_MIN), mNextTimeout(LLONG_MAX), mConfigurationChangesToRefresh(0) {
    mQueuedListener = new QueuedInputListener(listener);
    {
        AutoMutex _l(mLock);
        refreshConfigurationLocked(0);
        updateGlobalMetaStateLocked();
    }
}
InputReader::~InputReader() {}
status_t InputReader::start() {
    if (mThread) {
        return ALREADY_EXISTS;
    }
    mThread = std::make_unique<InputThread>(
            "InputReader", [this]() { loopOnce(); }, [this]() { mEventHub->wake(); });
    return OK;
}
status_t InputReader::stop() {
    if (mThread && mThread->isCallingThread()) {
        ALOGE("InputReader cannot be stopped from its own thread!");
        return INVALID_OPERATION;
    }
    mThread.reset();
    return OK;
}
void InputReader::loopOnce() {
    int32_t oldGeneration;
    int32_t timeoutMillis;
    bool inputDevicesChanged = false;
    std::vector<InputDeviceInfo> inputDevices;
    {
        AutoMutex _l(mLock);
        oldGeneration = mGeneration;
        timeoutMillis = -1;
        uint32_t changes = mConfigurationChangesToRefresh;
        if (changes) {
            mConfigurationChangesToRefresh = 0;
            timeoutMillis = 0;
            refreshConfigurationLocked(changes);
        } else if (mNextTimeout != LLONG_MAX) {
            nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
            timeoutMillis = toMillisecondTimeoutDelay(now, mNextTimeout);
        }
    }
    size_t count = mEventHub->getEvents(timeoutMillis, mEventBuffer, EVENT_BUFFER_SIZE);
    {
        AutoMutex _l(mLock);
        mReaderIsAliveCondition.broadcast();
        if (count) {
            processEventsLocked(mEventBuffer, count);
        }
        if (mNextTimeout != LLONG_MAX) {
            nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
            if (now >= mNextTimeout) {
#if DEBUG_RAW_EVENTS
                ALOGD("Timeout expired, latency=%0.3fms", (now - mNextTimeout) * 0.000001f);
#endif
                mNextTimeout = LLONG_MAX;
                timeoutExpiredLocked(now);
            }
        }
        if (oldGeneration != mGeneration) {
            inputDevicesChanged = true;
            getInputDevicesLocked(inputDevices);
        }
    }
    if (inputDevicesChanged) {
        mPolicy->notifyInputDevicesChanged(inputDevices);
    }
    mQueuedListener->flush();
}
void InputReader::processEventsLocked(const RawEvent* rawEvents, size_t count) {
    for (const RawEvent* rawEvent = rawEvents; count;) {
        int32_t type = rawEvent->type;
        size_t batchSize = 1;
        if (type < EventHubInterface::FIRST_SYNTHETIC_EVENT) {
            int32_t deviceId = rawEvent->deviceId;
            while (batchSize < count) {
                if (rawEvent[batchSize].type >= EventHubInterface::FIRST_SYNTHETIC_EVENT ||
                    rawEvent[batchSize].deviceId != deviceId) {
                    break;
                }
                batchSize += 1;
            }
#if DEBUG_RAW_EVENTS
            ALOGD("BatchSize: %zu Count: %zu", batchSize, count);
#endif
            processEventsForDeviceLocked(deviceId, rawEvent, batchSize);
        } else {
            switch (rawEvent->type) {
                case EventHubInterface::DEVICE_ADDED:
                    addDeviceLocked(rawEvent->when, rawEvent->deviceId);
                    break;
                case EventHubInterface::DEVICE_REMOVED:
                    removeDeviceLocked(rawEvent->when, rawEvent->deviceId);
                    break;
                case EventHubInterface::FINISHED_DEVICE_SCAN:
                    handleConfigurationChangedLocked(rawEvent->when);
                    break;
                default:
                    ALOG_ASSERT(false);
                    break;
            }
        }
        count -= batchSize;
        rawEvent += batchSize;
    }
}
void InputReader::addDeviceLocked(nsecs_t when, int32_t eventHubId) {
    if (mDevices.find(eventHubId) != mDevices.end()) {
        ALOGW("Ignoring spurious device added event for eventHubId %d.", eventHubId);
        return;
    }
    InputDeviceIdentifier identifier = mEventHub->getDeviceIdentifier(eventHubId);
    std::shared_ptr<InputDevice> device = createDeviceLocked(eventHubId, identifier);
    device->configure(when, &mConfig, 0);
    device->reset(when);
    if (device->isIgnored()) {
        ALOGI("Device added: id=%d, eventHubId=%d, name='%s', descriptor='%s' "
              "(ignored non-input device)",
              device->getId(), eventHubId, identifier.name.c_str(), identifier.descriptor.c_str());
    } else {
        ALOGI("Device added: id=%d, eventHubId=%d, name='%s', descriptor='%s',sources=0x%08x",
              device->getId(), eventHubId, identifier.name.c_str(), identifier.descriptor.c_str(),
              device->getSources());
    }
    mDevices.emplace(eventHubId, device);
    bumpGenerationLocked();
    if (device->getClasses() & INPUT_DEVICE_CLASS_EXTERNAL_STYLUS) {
        notifyExternalStylusPresenceChanged();
    }
}
void InputReader::removeDeviceLocked(nsecs_t when, int32_t eventHubId) {
    auto deviceIt = mDevices.find(eventHubId);
    if (deviceIt == mDevices.end()) {
        ALOGW("Ignoring spurious device removed event for eventHubId %d.", eventHubId);
        return;
    }
    std::shared_ptr<InputDevice> device = std::move(deviceIt->second);
    mDevices.erase(deviceIt);
    bumpGenerationLocked();
    if (device->isIgnored()) {
        ALOGI("Device removed: id=%d, eventHubId=%d, name='%s', descriptor='%s' "
              "(ignored non-input device)",
              device->getId(), eventHubId, device->getName().c_str(),
              device->getDescriptor().c_str());
    } else {
        ALOGI("Device removed: id=%d, eventHubId=%d, name='%s', descriptor='%s', sources=0x%08x",
              device->getId(), eventHubId, device->getName().c_str(),
              device->getDescriptor().c_str(), device->getSources());
    }
    device->removeEventHubDevice(eventHubId);
    if (device->getClasses() & INPUT_DEVICE_CLASS_EXTERNAL_STYLUS) {
        notifyExternalStylusPresenceChanged();
    }
    if (device->hasEventHubDevices()) {
        device->configure(when, &mConfig, 0);
    }
    device->reset(when);
}
std::shared_ptr<InputDevice> InputReader::createDeviceLocked(
        int32_t eventHubId, const InputDeviceIdentifier& identifier) {
    auto deviceIt = std::find_if(mDevices.begin(), mDevices.end(), [identifier](auto& devicePair) {
        return devicePair.second->getDescriptor().size() && identifier.descriptor.size() &&
                devicePair.second->getDescriptor() == identifier.descriptor;
    });
    std::shared_ptr<InputDevice> device;
    if (deviceIt != mDevices.end()) {
        device = deviceIt->second;
    } else {
        int32_t deviceId = (eventHubId < END_RESERVED_ID) ? eventHubId : nextInputDeviceIdLocked();
        device = std::make_shared<InputDevice>(&mContext, deviceId, bumpGenerationLocked(),
                                               identifier);
    }
    device->addEventHubDevice(eventHubId);
    return device;
}
void InputReader::processEventsForDeviceLocked(int32_t eventHubId, const RawEvent* rawEvents,
                                               size_t count) {
    auto deviceIt = mDevices.find(eventHubId);
    if (deviceIt == mDevices.end()) {
        ALOGW("Discarding event for unknown eventHubId %d.", eventHubId);
        return;
    }
    std::shared_ptr<InputDevice>& device = deviceIt->second;
    if (device->isIgnored()) {
        return;
    }
    device->process(rawEvents, count);
}
InputDevice* InputReader::findInputDevice(int32_t deviceId) {
    auto deviceIt =
            std::find_if(mDevices.begin(), mDevices.end(), [deviceId](const auto& devicePair) {
                return devicePair.second->getId() == deviceId;
            });
    if (deviceIt != mDevices.end()) {
        return deviceIt->second.get();
    }
    return nullptr;
}
void InputReader::timeoutExpiredLocked(nsecs_t when) {
    for (auto& devicePair : mDevices) {
        std::shared_ptr<InputDevice>& device = devicePair.second;
        if (!device->isIgnored()) {
            device->timeoutExpired(when);
        }
    }
}
int32_t InputReader::nextInputDeviceIdLocked() {
    return ++mNextInputDeviceId;
}
void InputReader::handleConfigurationChangedLocked(nsecs_t when) {
    updateGlobalMetaStateLocked();
    NotifyConfigurationChangedArgs args(mContext.getNextId(), when);
    mQueuedListener->notifyConfigurationChanged(&args);
}
void InputReader::refreshConfigurationLocked(uint32_t changes) {
    mPolicy->getReaderConfiguration(&mConfig);
    mEventHub->setExcludedDevices(mConfig.excludedDeviceNames);
    if (changes) {
        ALOGI("Reconfiguring input devices, changes=%s",
              InputReaderConfiguration::changesToString(changes).c_str());
        nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
        if (changes & InputReaderConfiguration::CHANGE_DISPLAY_INFO) {
            updatePointerDisplayLocked();
        }
        if (changes & InputReaderConfiguration::CHANGE_MUST_REOPEN) {
            mEventHub->requestReopenDevices();
        } else {
            for (auto& devicePair : mDevices) {
                std::shared_ptr<InputDevice>& device = devicePair.second;
                device->configure(now, &mConfig, changes);
            }
        }
    }
}
void InputReader::updateGlobalMetaStateLocked() {
    mGlobalMetaState = 0;
    for (auto& devicePair : mDevices) {
        std::shared_ptr<InputDevice>& device = devicePair.second;
        mGlobalMetaState |= device->getMetaState();
    }
}
int32_t InputReader::getGlobalMetaStateLocked() {
    return mGlobalMetaState;
}
void InputReader::notifyExternalStylusPresenceChanged() {
    refreshConfigurationLocked(InputReaderConfiguration::CHANGE_EXTERNAL_STYLUS_PRESENCE);
}
void InputReader::getExternalStylusDevicesLocked(std::vector<InputDeviceInfo>& outDevices) {
    for (auto& devicePair : mDevices) {
        std::shared_ptr<InputDevice>& device = devicePair.second;
        if (device->getClasses() & INPUT_DEVICE_CLASS_EXTERNAL_STYLUS && !device->isIgnored()) {
            InputDeviceInfo info;
            device->getDeviceInfo(&info);
            outDevices.push_back(info);
        }
    }
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
InputReader::ContextImpl::ContextImpl(InputReader* reader)
      : mReader(reader), mIdGenerator(IdGenerator::Source::INPUT_READER) {}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
int32_t InputReader::ContextImpl::getNextId() {
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
<<<<<<< HEAD
    return mIdGenerator.nextId();
||||||| f7bda296de
    return (mReader->mNextSequenceNum)++;
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
=======
    return (mReader->mNextId)++;
>>>>>>> eaf88010
}
}
