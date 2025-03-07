#define LOG_TAG "PointerChoreographer"
#include <android-base/logging.h>
#include <input/PrintTools.h>
#include "PointerChoreographer.h"
#define INDENT "  "
namespace android {
namespace {
bool isFromMouse(const NotifyMotionArgs& args) {
    return isFromSource(args.source, AINPUT_SOURCE_MOUSE) &&
            args.pointerProperties[0].toolType == ToolType::MOUSE;
}
bool isFromTouchpad(const NotifyMotionArgs& args) {
    return isFromSource(args.source, AINPUT_SOURCE_MOUSE) &&
            args.pointerProperties[0].toolType == ToolType::FINGER;
}
bool isHoverAction(int32_t action) {
    return action == AMOTION_EVENT_ACTION_HOVER_ENTER ||
            action == AMOTION_EVENT_ACTION_HOVER_MOVE || action == AMOTION_EVENT_ACTION_HOVER_EXIT;
}
bool isStylusHoverEvent(const NotifyMotionArgs& args) {
    return isStylusEvent(args.source, args.pointerProperties) && isHoverAction(args.action);
}
inline void notifyPointerDisplayChange(std::optional<std::tuple<int32_t, FloatPoint>> change,
                                       PointerChoreographerPolicyInterface& policy) {
    if (!change) {
        return;
    }
    const auto& [displayId, cursorPosition] = *change;
    policy.notifyPointerDisplayIdChanged(displayId, cursorPosition);
}
}
PointerChoreographer::PointerChoreographer(InputListenerInterface& listener,
                                           PointerChoreographerPolicyInterface& policy)
      : mTouchControllerConstructor([this]() {
            return mPolicy.createPointerController(
                    PointerControllerInterface::ControllerType::TOUCH);
        }),
        mNextListener(listener),
        mPolicy(policy),
        mDefaultMouseDisplayId(ADISPLAY_ID_DEFAULT),
        mNotifiedPointerDisplayId(ADISPLAY_ID_NONE),
        mShowTouchesEnabled(false),
        mStylusPointerIconEnabled(false) {}
void PointerChoreographer::notifyInputDevicesChanged(const NotifyInputDevicesChangedArgs& args) {
    PointerDisplayChange pointerDisplayChange;
    {
        std::scoped_lock _l(mLock);
        mInputDeviceInfos = args.inputDeviceInfos;
        pointerDisplayChange = updatePointerControllersLocked();
    }
    notifyPointerDisplayChange(pointerDisplayChange, mPolicy);
    mNextListener.notify(args);
}
void PointerChoreographer::notifyConfigurationChanged(const NotifyConfigurationChangedArgs& args) {
    mNextListener.notify(args);
}
void PointerChoreographer::notifyKey(const NotifyKeyArgs& args) {
    mNextListener.notify(args);
}
void PointerChoreographer::notifyMotion(const NotifyMotionArgs& args) {
    NotifyMotionArgs newArgs = processMotion(args);
    mNextListener.notify(newArgs);
}
NotifyMotionArgs PointerChoreographer::processMotion(const NotifyMotionArgs& args) {
    std::scoped_lock _l(mLock);
    if (isFromMouse(args)) {
        return processMouseEventLocked(args);
    } else if (isFromTouchpad(args)) {
        return processTouchpadEventLocked(args);
    } else if (mStylusPointerIconEnabled && isStylusHoverEvent(args)) {
        processStylusHoverEventLocked(args);
    } else if (isFromSource(args.source, AINPUT_SOURCE_TOUCHSCREEN)) {
        processTouchscreenAndStylusEventLocked(args);
    }
    return args;
}
NotifyMotionArgs PointerChoreographer::processMouseEventLocked(const NotifyMotionArgs& args) {
    if (args.getPointerCount() != 1) {
        LOG(FATAL) << "Only mouse events with a single pointer are currently supported: "
                   << args.dump();
    }
auto [displayId, pc] = ensureMouseControllerLocked(args.displayId); NotifyMotionArgs newArgs(args); newArgs.displayId = displayId;
    if (MotionEvent::isValidCursorPosition(args.xCursorPosition, args.yCursorPosition)) {
        const auto [x, y] = pc.getPosition();
        const float deltaX = args.xCursorPosition - x;
        const float deltaY = args.yCursorPosition - y;
        newArgs.pointerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_RELATIVE_X, deltaX);
        newArgs.pointerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_RELATIVE_Y, deltaY);
        pc.setPosition(args.xCursorPosition, args.yCursorPosition);
    } else {
        const float deltaX = args.pointerCoords[0].getAxisValue(AMOTION_EVENT_AXIS_RELATIVE_X);
        const float deltaY = args.pointerCoords[0].getAxisValue(AMOTION_EVENT_AXIS_RELATIVE_Y);
        pc.move(deltaX, deltaY);
        const auto [x, y] = pc.getPosition();
        newArgs.pointerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_X, x);
        newArgs.pointerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_Y, y);
        newArgs.xCursorPosition = x;
        newArgs.yCursorPosition = y;
    }
    if (canUnfadeOnDisplay(displayId)) {
        pc.unfade(PointerControllerInterface::Transition::IMMEDIATE);
    }
    return newArgs;
}
NotifyMotionArgs PointerChoreographer::processTouchpadEventLocked(const NotifyMotionArgs& args) {
    auto [displayId, pc] = ensureMouseControllerLocked(args.displayId);
    NotifyMotionArgs newArgs(args);
    newArgs.displayId = displayId;
    if (args.getPointerCount() == 1 && args.classification == MotionClassification::NONE) {
        const float deltaX = args.pointerCoords[0].getAxisValue(AMOTION_EVENT_AXIS_RELATIVE_X);
        const float deltaY = args.pointerCoords[0].getAxisValue(AMOTION_EVENT_AXIS_RELATIVE_Y);
        pc.move(deltaX, deltaY);
        if (canUnfadeOnDisplay(displayId)) {
            pc.unfade(PointerControllerInterface::Transition::IMMEDIATE);
        }
        const auto [x, y] = pc.getPosition();
        newArgs.pointerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_X, x);
        newArgs.pointerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_Y, y);
        newArgs.xCursorPosition = x;
        newArgs.yCursorPosition = y;
    } else {
        if (canUnfadeOnDisplay(displayId)) {
            pc.unfade(PointerControllerInterface::Transition::IMMEDIATE);
        }
        const auto [x, y] = pc.getPosition();
        for (uint32_t i = 0; i < newArgs.getPointerCount(); i++) {
            newArgs.pointerCoords[i].setAxisValue(AMOTION_EVENT_AXIS_X,
                                                  args.pointerCoords[i].getX() + x);
            newArgs.pointerCoords[i].setAxisValue(AMOTION_EVENT_AXIS_Y,
                                                  args.pointerCoords[i].getY() + y);
        }
        newArgs.xCursorPosition = x;
        newArgs.yCursorPosition = y;
    }
    return newArgs;
}
void PointerChoreographer::processTouchscreenAndStylusEventLocked(const NotifyMotionArgs& args) {
    if (args.displayId == ADISPLAY_ID_NONE) {
        return;
    }
    if (const auto it = mMousePointersByDisplay.find(args.displayId);
        it != mMousePointersByDisplay.end() && args.action == AMOTION_EVENT_ACTION_DOWN) {
        it->second->fade(PointerControllerInterface::Transition::GRADUAL);
    }
    if (!mShowTouchesEnabled) {
        return;
    }
    auto [it, _] = mTouchPointersByDevice.try_emplace(args.deviceId, mTouchControllerConstructor);
    PointerControllerInterface& pc = *it->second;
    const PointerCoords* coords = args.pointerCoords.data();
    const int32_t maskedAction = MotionEvent::getActionMasked(args.action);
    const uint8_t actionIndex = MotionEvent::getActionIndex(args.action);
    std::array<uint32_t, MAX_POINTER_ID + 1> idToIndex;
    BitSet32 idBits;
    if (maskedAction != AMOTION_EVENT_ACTION_UP && maskedAction != AMOTION_EVENT_ACTION_CANCEL) {
        for (size_t i = 0; i < args.getPointerCount(); i++) {
            if (maskedAction == AMOTION_EVENT_ACTION_POINTER_UP && actionIndex == i) {
                continue;
            }
            uint32_t id = args.pointerProperties[i].id;
            idToIndex[id] = i;
            idBits.markBit(id);
        }
    }
    pc.setSpots(coords, idToIndex.cbegin(), idBits, args.displayId);
}
void PointerChoreographer::processStylusHoverEventLocked(const NotifyMotionArgs& args) {
    if (args.displayId == ADISPLAY_ID_NONE) {
        return;
    }
    if (args.getPointerCount() != 1) {
        LOG(WARNING) << "Only stylus hover events with a single pointer are currently supported: "
                     << args.dump();
    }
    auto [it, _] =
            mStylusPointersByDevice.try_emplace(args.deviceId,
                                                getStylusControllerConstructor(args.displayId));
    PointerControllerInterface& pc = *it->second;
    const float x = args.pointerCoords[0].getAxisValue(AMOTION_EVENT_AXIS_X);
    const float y = args.pointerCoords[0].getAxisValue(AMOTION_EVENT_AXIS_Y);
    pc.setPosition(x, y);
    if (args.action == AMOTION_EVENT_ACTION_HOVER_EXIT) {
        pc.fade(PointerControllerInterface::Transition::IMMEDIATE);
        pc.updatePointerIcon(PointerIconStyle::TYPE_NOT_SPECIFIED);
    } else if (canUnfadeOnDisplay(args.displayId)) {
        pc.unfade(PointerControllerInterface::Transition::IMMEDIATE);
    }
}
void PointerChoreographer::notifySwitch(const NotifySwitchArgs& args) {
    mNextListener.notify(args);
}
void PointerChoreographer::notifySensor(const NotifySensorArgs& args) {
    mNextListener.notify(args);
}
void PointerChoreographer::notifyVibratorState(const NotifyVibratorStateArgs& args) {
    mNextListener.notify(args);
}
void PointerChoreographer::notifyDeviceReset(const NotifyDeviceResetArgs& args) {
    processDeviceReset(args);
    mNextListener.notify(args);
}
void PointerChoreographer::processDeviceReset(const NotifyDeviceResetArgs& args) {
    std::scoped_lock _l(mLock);
    mTouchPointersByDevice.erase(args.deviceId);
    mStylusPointersByDevice.erase(args.deviceId);
}
void PointerChoreographer::notifyPointerCaptureChanged(
        const NotifyPointerCaptureChangedArgs& args) {
    if (args.request.isEnable()) {
        std::scoped_lock _l(mLock);
        for (const auto& [_, mousePointerController] : mMousePointersByDisplay) {
            mousePointerController->fade(PointerControllerInterface::Transition::IMMEDIATE);
        }
    }
    mNextListener.notify(args);
}
void PointerChoreographer::dump(std::string& dump) {
    std::scoped_lock _l(mLock);
    dump += "PointerChoreographer:\n";
    dump += StringPrintf("show touches: %s\n", mShowTouchesEnabled ? "true" : "false");
    dump += StringPrintf("stylus pointer icon enabled: %s\n",
                         mStylusPointerIconEnabled ? "true" : "false");
    dump += INDENT "MousePointerControllers:\n";
    for (const auto& [displayId, mousePointerController] : mMousePointersByDisplay) {
        std::string pointerControllerDump = addLinePrefix(mousePointerController->dump(), INDENT);
        dump += INDENT + std::to_string(displayId) + " : " + pointerControllerDump;
    }
    dump += INDENT "TouchPointerControllers:\n";
    for (const auto& [deviceId, touchPointerController] : mTouchPointersByDevice) {
        std::string pointerControllerDump = addLinePrefix(touchPointerController->dump(), INDENT);
        dump += INDENT + std::to_string(deviceId) + " : " + pointerControllerDump;
    }
    dump += INDENT "StylusPointerControllers:\n";
    for (const auto& [deviceId, stylusPointerController] : mStylusPointersByDevice) {
        std::string pointerControllerDump = addLinePrefix(stylusPointerController->dump(), INDENT);
        dump += INDENT + std::to_string(deviceId) + " : " + pointerControllerDump;
    }
    dump += "\n";
}
const DisplayViewport* PointerChoreographer::findViewportByIdLocked(int32_t displayId) const {
    for (auto& viewport : mViewports) {
        if (viewport.displayId == displayId) {
            return &viewport;
        }
    }
    return nullptr;
}
int32_t PointerChoreographer::getTargetMouseDisplayLocked(int32_t associatedDisplayId) const {
    return associatedDisplayId == ADISPLAY_ID_NONE ? mDefaultMouseDisplayId : associatedDisplayId;
}
std::pair<int32_t, PointerControllerInterface&> PointerChoreographer::ensureMouseControllerLocked(
        int32_t associatedDisplayId) {
    const int32_t displayId = getTargetMouseDisplayLocked(associatedDisplayId);
    auto it = mMousePointersByDisplay.find(displayId);
    LOG_ALWAYS_FATAL_IF(it == mMousePointersByDisplay.end(),
                        "There is no mouse controller created for display %d", displayId);
    return {displayId, *it->second};
}
InputDeviceInfo* PointerChoreographer::findInputDeviceLocked(DeviceId deviceId) {
    auto it = std::find_if(mInputDeviceInfos.begin(), mInputDeviceInfos.end(),
                           [deviceId](const auto& info) { return info.getId() == deviceId; });
    return it != mInputDeviceInfos.end() ? &(*it) : nullptr;
}
bool PointerChoreographer::canUnfadeOnDisplay(int32_t displayId) {
    return mDisplaysWithPointersHidden.find(displayId) == mDisplaysWithPointersHidden.end();
}
PointerChoreographer::PointerDisplayChange PointerChoreographer::updatePointerControllersLocked() {
    std::set<int32_t > mouseDisplaysToKeep;
    std::set<DeviceId> touchDevicesToKeep;
    std::set<DeviceId> stylusDevicesToKeep;
    for (const auto& info : mInputDeviceInfos) {
        const uint32_t sources = info.getSources();
        if (isFromSource(sources, AINPUT_SOURCE_MOUSE) ||
            isFromSource(sources, AINPUT_SOURCE_MOUSE_RELATIVE)) {
            const int32_t displayId = getTargetMouseDisplayLocked(info.getAssociatedDisplayId());
            mouseDisplaysToKeep.insert(displayId);
            auto [mousePointerIt, isNewMousePointer] =
                    mMousePointersByDisplay.try_emplace(displayId,
                                                        getMouseControllerConstructor(displayId));
            auto [_, isNewMouseDevice] = mMouseDevices.emplace(info.getId());
            if ((isNewMouseDevice || isNewMousePointer) && canUnfadeOnDisplay(displayId)) {
                mousePointerIt->second->unfade(PointerControllerInterface::Transition::IMMEDIATE);
            }
        }
        if (isFromSource(sources, AINPUT_SOURCE_TOUCHSCREEN) && mShowTouchesEnabled &&
            info.getAssociatedDisplayId() != ADISPLAY_ID_NONE) {
            touchDevicesToKeep.insert(info.getId());
        }
        if (isFromSource(sources, AINPUT_SOURCE_STYLUS) && mStylusPointerIconEnabled &&
            info.getAssociatedDisplayId() != ADISPLAY_ID_NONE) {
            stylusDevicesToKeep.insert(info.getId());
        }
    }
    std::erase_if(mMousePointersByDisplay, [&mouseDisplaysToKeep](const auto& pair) {
        return mouseDisplaysToKeep.find(pair.first) == mouseDisplaysToKeep.end();
    });
    std::erase_if(mTouchPointersByDevice, [&touchDevicesToKeep](const auto& pair) {
        return touchDevicesToKeep.find(pair.first) == touchDevicesToKeep.end();
    });
    std::erase_if(mStylusPointersByDevice, [&stylusDevicesToKeep](const auto& pair) {
        return stylusDevicesToKeep.find(pair.first) == stylusDevicesToKeep.end();
    });
    std::erase_if(mMouseDevices, [&](DeviceId id) REQUIRES(mLock) {
        return std::find_if(mInputDeviceInfos.begin(), mInputDeviceInfos.end(),
                            [id](const auto& info) { return info.getId() == id; }) ==
                mInputDeviceInfos.end();
    });
    return calculatePointerDisplayChangeToNotify();
}
PointerChoreographer::PointerDisplayChange
PointerChoreographer::calculatePointerDisplayChangeToNotify() {
    int32_t displayIdToNotify = ADISPLAY_ID_NONE;
    FloatPoint cursorPosition = {0, 0};
    if (const auto it = mMousePointersByDisplay.find(mDefaultMouseDisplayId);
        it != mMousePointersByDisplay.end()) {
        const auto& pointerController = it->second;
        displayIdToNotify = pointerController->getDisplayId();
        cursorPosition = pointerController->getPosition();
    }
    if (mNotifiedPointerDisplayId == displayIdToNotify) {
        return {};
    }
    mNotifiedPointerDisplayId = displayIdToNotify;
    return {{displayIdToNotify, cursorPosition}};
}
void PointerChoreographer::setDefaultMouseDisplayId(int32_t displayId) {
    PointerDisplayChange pointerDisplayChange;
    {
        std::scoped_lock _l(mLock);
        mDefaultMouseDisplayId = displayId;
        pointerDisplayChange = updatePointerControllersLocked();
    }
    notifyPointerDisplayChange(pointerDisplayChange, mPolicy);
}
void PointerChoreographer::setDisplayViewports(const std::vector<DisplayViewport>& viewports) {
    PointerDisplayChange pointerDisplayChange;
    {
        std::scoped_lock _l(mLock);
        for (const auto& viewport : viewports) {
            const int32_t displayId = viewport.displayId;
            if (const auto it = mMousePointersByDisplay.find(displayId);
                it != mMousePointersByDisplay.end()) {
                it->second->setDisplayViewport(viewport);
            }
            for (const auto& [deviceId, stylusPointerController] : mStylusPointersByDevice) {
                const InputDeviceInfo* info = findInputDeviceLocked(deviceId);
                if (info && info->getAssociatedDisplayId() == displayId) {
                    stylusPointerController->setDisplayViewport(viewport);
                }
            }
        }
        mViewports = viewports;
        pointerDisplayChange = calculatePointerDisplayChangeToNotify();
    }
    notifyPointerDisplayChange(pointerDisplayChange, mPolicy);
}
std::optional<DisplayViewport> PointerChoreographer::getViewportForPointerDevice(
        int32_t associatedDisplayId) {
    std::scoped_lock _l(mLock);
    const int32_t resolvedDisplayId = getTargetMouseDisplayLocked(associatedDisplayId);
    if (const auto viewport = findViewportByIdLocked(resolvedDisplayId); viewport) {
        return *viewport;
    }
    return std::nullopt;
}
FloatPoint PointerChoreographer::getMouseCursorPosition(int32_t displayId) {
    std::scoped_lock _l(mLock);
    const int32_t resolvedDisplayId = getTargetMouseDisplayLocked(displayId);
    if (auto it = mMousePointersByDisplay.find(resolvedDisplayId);
        it != mMousePointersByDisplay.end()) {
        return it->second->getPosition();
    }
    return {AMOTION_EVENT_INVALID_CURSOR_POSITION, AMOTION_EVENT_INVALID_CURSOR_POSITION};
}
void PointerChoreographer::setShowTouchesEnabled(bool enabled) {
    PointerDisplayChange pointerDisplayChange;
    {
        std::scoped_lock _l(mLock);
        if (mShowTouchesEnabled == enabled) {
            return;
        }
        mShowTouchesEnabled = enabled;
        pointerDisplayChange = updatePointerControllersLocked();
    }
    notifyPointerDisplayChange(pointerDisplayChange, mPolicy);
}
void PointerChoreographer::setStylusPointerIconEnabled(bool enabled) {
    PointerDisplayChange pointerDisplayChange;
    {
        std::scoped_lock _l(mLock);
        if (mStylusPointerIconEnabled == enabled) {
            return;
        }
        mStylusPointerIconEnabled = enabled;
        pointerDisplayChange = updatePointerControllersLocked();
    }
    notifyPointerDisplayChange(pointerDisplayChange, mPolicy);
}
bool PointerChoreographer::setPointerIcon(
        std::variant<std::unique_ptr<SpriteIcon>, PointerIconStyle> icon, int32_t displayId,
        DeviceId deviceId) {
    std::scoped_lock _l(mLock);
    if (deviceId < 0) {
        LOG(WARNING) << "Invalid device id " << deviceId << ". Cannot set pointer icon.";
        return false;
    }
    const InputDeviceInfo* info = findInputDeviceLocked(deviceId);
    if (!info) {
        LOG(WARNING) << "No input device info found for id " << deviceId
                     << ". Cannot set pointer icon.";
        return false;
    }
    const uint32_t sources = info->getSources();
    const auto stylusPointerIt = mStylusPointersByDevice.find(deviceId);
    if (isFromSource(sources, AINPUT_SOURCE_STYLUS) &&
        stylusPointerIt != mStylusPointersByDevice.end()) {
        if (std::holds_alternative<std::unique_ptr<SpriteIcon>>(icon)) {
            if (std::get<std::unique_ptr<SpriteIcon>>(icon) == nullptr) {
                LOG(FATAL) << "SpriteIcon should not be null";
            }
            stylusPointerIt->second->setCustomPointerIcon(
                    *std::get<std::unique_ptr<SpriteIcon>>(icon));
        } else {
            stylusPointerIt->second->updatePointerIcon(std::get<PointerIconStyle>(icon));
        }
    } else if (isFromSource(sources, AINPUT_SOURCE_MOUSE)) {
        if (const auto mousePointerIt = mMousePointersByDisplay.find(displayId);
            mousePointerIt != mMousePointersByDisplay.end()) {
            if (std::holds_alternative<std::unique_ptr<SpriteIcon>>(icon)) {
                if (std::get<std::unique_ptr<SpriteIcon>>(icon) == nullptr) {
                    LOG(FATAL) << "SpriteIcon should not be null";
                }
                mousePointerIt->second->setCustomPointerIcon(
                        *std::get<std::unique_ptr<SpriteIcon>>(icon));
            } else {
                mousePointerIt->second->updatePointerIcon(std::get<PointerIconStyle>(icon));
            }
        } else {
            LOG(WARNING) << "No mouse pointer controller found for display " << displayId
                         << ", device " << deviceId << ".";
            return false;
        }
    } else {
        LOG(WARNING) << "Cannot set pointer icon for display " << displayId << ", device "
                     << deviceId << ".";
        return false;
    }
    return true;
}
void PointerChoreographer::setPointerIconVisibility(int32_t displayId, bool visible) {
    std::scoped_lock lock(mLock);
    if (visible) {
        mDisplaysWithPointersHidden.erase(displayId);
        return;
    }
    mDisplaysWithPointersHidden.emplace(displayId);
    if (auto it = mMousePointersByDisplay.find(displayId); it != mMousePointersByDisplay.end()) {
        const auto& [_, controller] = *it;
        controller->fade(PointerControllerInterface::Transition::IMMEDIATE);
    }
    for (const auto& [_, controller] : mStylusPointersByDevice) {
        if (controller->getDisplayId() == displayId) {
            controller->fade(PointerControllerInterface::Transition::IMMEDIATE);
        }
    }
}
PointerChoreographer::ControllerConstructor PointerChoreographer::getMouseControllerConstructor(
        int32_t displayId) {
    std::function<std::shared_ptr<PointerControllerInterface>()> ctor =
            [this, displayId]() REQUIRES(mLock) {
                auto pc = mPolicy.createPointerController(
                        PointerControllerInterface::ControllerType::MOUSE);
                if (const auto viewport = findViewportByIdLocked(displayId); viewport) {
                    pc->setDisplayViewport(*viewport);
                }
                return pc;
            };
    return ConstructorDelegate(std::move(ctor));
}
PointerChoreographer::ControllerConstructor PointerChoreographer::getStylusControllerConstructor(
        int32_t displayId) {
    std::function<std::shared_ptr<PointerControllerInterface>()> ctor =
            [this, displayId]() REQUIRES(mLock) {
                auto pc = mPolicy.createPointerController(
                        PointerControllerInterface::ControllerType::STYLUS);
                if (const auto viewport = findViewportByIdLocked(displayId); viewport) {
                    pc->setDisplayViewport(*viewport);
                }
                return pc;
            };
    return ConstructorDelegate(std::move(ctor));
}
}
