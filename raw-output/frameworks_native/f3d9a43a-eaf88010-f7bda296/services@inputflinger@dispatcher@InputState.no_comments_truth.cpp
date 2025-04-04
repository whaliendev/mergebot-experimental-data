#include "InputState.h"
#include "InputDispatcher.h"
namespace android::inputdispatcher {
InputState::InputState(const IdGenerator& idGenerator) : mIdGenerator(idGenerator) {}
InputState::~InputState() {}
bool InputState::isNeutral() const {
    return mKeyMementos.empty() && mMotionMementos.empty();
}
bool InputState::isHovering(int32_t deviceId, uint32_t source, int32_t displayId) const {
    for (const MotionMemento& memento : mMotionMementos) {
        if (memento.deviceId == deviceId && memento.source == source &&
            memento.displayId == displayId && memento.hovering) {
            return true;
        }
    }
    return false;
}
bool InputState::trackKey(const KeyEntry& entry, int32_t action, int32_t flags) {
    switch (action) {
        case AKEY_EVENT_ACTION_UP: {
            if (entry.flags & AKEY_EVENT_FLAG_FALLBACK) {
                for (size_t i = 0; i < mFallbackKeys.size();) {
                    if (mFallbackKeys.valueAt(i) == entry.keyCode) {
                        mFallbackKeys.removeItemsAt(i);
                    } else {
                        i += 1;
                    }
                }
            }
            ssize_t index = findKeyMemento(entry);
            if (index >= 0) {
                mKeyMementos.erase(mKeyMementos.begin() + index);
                return true;
            }
            return true;
        }
        case AKEY_EVENT_ACTION_DOWN: {
            ssize_t index = findKeyMemento(entry);
            if (index >= 0) {
                mKeyMementos.erase(mKeyMementos.begin() + index);
            }
            addKeyMemento(entry, flags);
            return true;
        }
        default:
            return true;
    }
}
bool InputState::trackMotion(const MotionEntry& entry, int32_t action, int32_t flags) {
    int32_t actionMasked = action & AMOTION_EVENT_ACTION_MASK;
    switch (actionMasked) {
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL: {
            ssize_t index = findMotionMemento(entry, false );
            if (index >= 0) {
                mMotionMementos.erase(mMotionMementos.begin() + index);
                return true;
            }
#if DEBUG_OUTBOUND_EVENT_DETAILS
            ALOGD("Dropping inconsistent motion up or cancel event: deviceId=%d, source=%08x, "
                  "displayId=%" PRId32 ", actionMasked=%d",
                  entry.deviceId, entry.source, entry.displayId, actionMasked);
#endif
            return false;
        }
        case AMOTION_EVENT_ACTION_DOWN: {
            ssize_t index = findMotionMemento(entry, false );
            if (index >= 0) {
                mMotionMementos.erase(mMotionMementos.begin() + index);
            }
            addMotionMemento(entry, flags, false );
            return true;
        }
        case AMOTION_EVENT_ACTION_POINTER_UP:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
        case AMOTION_EVENT_ACTION_MOVE: {
            if (entry.source & AINPUT_SOURCE_CLASS_NAVIGATION) {
                return true;
            }
            ssize_t index = findMotionMemento(entry, false );
            if (entry.source & AINPUT_SOURCE_CLASS_JOYSTICK) {
                if (index >= 0) {
                    if (entry.pointerCoords[0].isEmpty()) {
                        mMotionMementos.erase(mMotionMementos.begin() + index);
                    } else {
                        MotionMemento& memento = mMotionMementos[index];
                        memento.setPointers(entry);
                    }
                } else if (!entry.pointerCoords[0].isEmpty()) {
                    addMotionMemento(entry, flags, false );
                }
                return true;
            }
            if (index >= 0) {
                MotionMemento& memento = mMotionMementos[index];
                if (memento.firstNewPointerIdx < 0) {
                    memento.setPointers(entry);
                    return true;
                }
            }
#if DEBUG_OUTBOUND_EVENT_DETAILS
            ALOGD("Dropping inconsistent motion pointer up/down or move event: "
                  "deviceId=%d, source=%08x, displayId=%" PRId32 ", actionMasked=%d",
                  entry.deviceId, entry.source, entry.displayId, actionMasked);
#endif
            return false;
        }
        case AMOTION_EVENT_ACTION_HOVER_EXIT: {
            ssize_t index = findMotionMemento(entry, true );
            if (index >= 0) {
                mMotionMementos.erase(mMotionMementos.begin() + index);
                return true;
            }
#if DEBUG_OUTBOUND_EVENT_DETAILS
            ALOGD("Dropping inconsistent motion hover exit event: deviceId=%d, source=%08x, "
                  "displayId=%" PRId32,
                  entry.deviceId, entry.source, entry.displayId);
#endif
            return false;
        }
        case AMOTION_EVENT_ACTION_HOVER_ENTER:
        case AMOTION_EVENT_ACTION_HOVER_MOVE: {
            ssize_t index = findMotionMemento(entry, true );
            if (index >= 0) {
                mMotionMementos.erase(mMotionMementos.begin() + index);
            }
            addMotionMemento(entry, flags, true );
            return true;
        }
        default:
            return true;
    }
}
ssize_t InputState::findKeyMemento(const KeyEntry& entry) const {
    for (size_t i = 0; i < mKeyMementos.size(); i++) {
        const KeyMemento& memento = mKeyMementos[i];
        if (memento.deviceId == entry.deviceId && memento.source == entry.source &&
            memento.displayId == entry.displayId && memento.keyCode == entry.keyCode &&
            memento.scanCode == entry.scanCode) {
            return i;
        }
    }
    return -1;
}
ssize_t InputState::findMotionMemento(const MotionEntry& entry, bool hovering) const {
    for (size_t i = 0; i < mMotionMementos.size(); i++) {
        const MotionMemento& memento = mMotionMementos[i];
        if (memento.deviceId == entry.deviceId && memento.source == entry.source &&
            memento.displayId == entry.displayId && memento.hovering == hovering) {
            return i;
        }
    }
    return -1;
}
void InputState::addKeyMemento(const KeyEntry& entry, int32_t flags) {
    KeyMemento memento;
    memento.deviceId = entry.deviceId;
    memento.source = entry.source;
    memento.displayId = entry.displayId;
    memento.keyCode = entry.keyCode;
    memento.scanCode = entry.scanCode;
    memento.metaState = entry.metaState;
    memento.flags = flags;
    memento.downTime = entry.downTime;
    memento.policyFlags = entry.policyFlags;
    mKeyMementos.push_back(memento);
}
void InputState::addMotionMemento(const MotionEntry& entry, int32_t flags, bool hovering) {
    MotionMemento memento;
    memento.deviceId = entry.deviceId;
    memento.source = entry.source;
    memento.displayId = entry.displayId;
    memento.flags = flags;
    memento.xPrecision = entry.xPrecision;
    memento.yPrecision = entry.yPrecision;
    memento.xCursorPosition = entry.xCursorPosition;
    memento.yCursorPosition = entry.yCursorPosition;
    memento.downTime = entry.downTime;
    memento.setPointers(entry);
    memento.hovering = hovering;
    memento.policyFlags = entry.policyFlags;
    mMotionMementos.push_back(memento);
}
void InputState::MotionMemento::setPointers(const MotionEntry& entry) {
    pointerCount = entry.pointerCount;
    for (uint32_t i = 0; i < entry.pointerCount; i++) {
        pointerProperties[i].copyFrom(entry.pointerProperties[i]);
        pointerCoords[i].copyFrom(entry.pointerCoords[i]);
    }
}
void InputState::MotionMemento::mergePointerStateTo(MotionMemento& other) const {
    for (uint32_t i = 0; i < pointerCount; i++) {
        if (other.firstNewPointerIdx < 0) {
            other.firstNewPointerIdx = other.pointerCount;
        }
        other.pointerProperties[other.pointerCount].copyFrom(pointerProperties[i]);
        other.pointerCoords[other.pointerCount].copyFrom(pointerCoords[i]);
        other.pointerCount++;
    }
}
std::vector<EventEntry*> InputState::synthesizeCancelationEvents(
        nsecs_t currentTime, const CancelationOptions& options) {
    std::vector<EventEntry*> events;
    for (KeyMemento& memento : mKeyMementos) {
        if (shouldCancelKey(memento, options)) {
            events.push_back(new KeyEntry(mIdGenerator.nextId(), currentTime, memento.deviceId,
                                          memento.source, memento.displayId, memento.policyFlags,
                                          AKEY_EVENT_ACTION_UP,
                                          memento.flags | AKEY_EVENT_FLAG_CANCELED, memento.keyCode,
                                          memento.scanCode, memento.metaState, 0 ,
                                          memento.downTime));
        }
    }
    for (const MotionMemento& memento : mMotionMementos) {
        if (shouldCancelMotion(memento, options)) {
            const int32_t action = memento.hovering ? AMOTION_EVENT_ACTION_HOVER_EXIT
                                                    : AMOTION_EVENT_ACTION_CANCEL;
            events.push_back(new MotionEntry(mIdGenerator.nextId(), currentTime, memento.deviceId,
                                             memento.source, memento.displayId, memento.policyFlags,
                                             action, 0 , memento.flags, AMETA_NONE,
                                             0 , MotionClassification::NONE,
                                             AMOTION_EVENT_EDGE_FLAG_NONE, memento.xPrecision,
                                             memento.yPrecision, memento.xCursorPosition,
                                             memento.yCursorPosition, memento.downTime,
                                             memento.pointerCount, memento.pointerProperties,
                                             memento.pointerCoords, 0 , 0 ));
        }
    }
    return events;
}
std::vector<EventEntry*> InputState::synthesizePointerDownEvents(nsecs_t currentTime) {
    std::vector<EventEntry*> events;
    for (MotionMemento& memento : mMotionMementos) {
        if (!(memento.source & AINPUT_SOURCE_CLASS_POINTER)) {
            continue;
        }
        if (memento.firstNewPointerIdx < 0) {
            continue;
        }
        uint32_t pointerCount = 0;
        PointerProperties pointerProperties[MAX_POINTERS];
        PointerCoords pointerCoords[MAX_POINTERS];
        for (uint32_t i = 0; i < static_cast<uint32_t>(memento.firstNewPointerIdx); i++) {
            pointerProperties[i].copyFrom(memento.pointerProperties[i]);
            pointerCoords[i].copyFrom(memento.pointerCoords[i]);
            pointerCount++;
        }
        for (uint32_t i = static_cast<uint32_t>(memento.firstNewPointerIdx);
                i < memento.pointerCount; i++) {
            pointerProperties[i].copyFrom(memento.pointerProperties[i]);
            pointerCoords[i].copyFrom(memento.pointerCoords[i]);
            pointerCount++;
            const int32_t action = (pointerCount <= 1)
                    ? AMOTION_EVENT_ACTION_DOWN
                    : AMOTION_EVENT_ACTION_POINTER_DOWN
                            | (i << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
            events.push_back(new MotionEntry(mIdGenerator.nextId(), currentTime, memento.deviceId,
                                             memento.source, memento.displayId, memento.policyFlags,
                                             action, 0 , memento.flags, AMETA_NONE,
                                             0 , MotionClassification::NONE,
                                             AMOTION_EVENT_EDGE_FLAG_NONE, memento.xPrecision,
                                             memento.yPrecision, memento.xCursorPosition,
                                             memento.yCursorPosition, memento.downTime,
                                             pointerCount, pointerProperties, pointerCoords,
                                             0 , 0 ));
        }
        memento.firstNewPointerIdx = INVALID_POINTER_INDEX;
    }
    return events;
}
void InputState::clear() {
    mKeyMementos.clear();
    mMotionMementos.clear();
    mFallbackKeys.clear();
}
void InputState::mergePointerStateTo(InputState& other) {
    for (size_t i = 0; i < mMotionMementos.size(); i++) {
        MotionMemento& memento = mMotionMementos[i];
        if (memento.source & AINPUT_SOURCE_CLASS_POINTER) {
            bool merged = false;
            for (size_t j = 0; j < other.mMotionMementos.size(); j++) {
                MotionMemento& otherMemento = other.mMotionMementos[j];
                if (memento.deviceId == otherMemento.deviceId &&
                    memento.source == otherMemento.source &&
                    memento.displayId == otherMemento.displayId) {
                    memento.mergePointerStateTo(otherMemento);
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                memento.firstNewPointerIdx = 0;
                other.mMotionMementos.push_back(memento);
            }
        }
    }
}
int32_t InputState::getFallbackKey(int32_t originalKeyCode) {
    ssize_t index = mFallbackKeys.indexOfKey(originalKeyCode);
    return index >= 0 ? mFallbackKeys.valueAt(index) : -1;
}
void InputState::setFallbackKey(int32_t originalKeyCode, int32_t fallbackKeyCode) {
    ssize_t index = mFallbackKeys.indexOfKey(originalKeyCode);
    if (index >= 0) {
        mFallbackKeys.replaceValueAt(index, fallbackKeyCode);
    } else {
        mFallbackKeys.add(originalKeyCode, fallbackKeyCode);
    }
}
void InputState::removeFallbackKey(int32_t originalKeyCode) {
    mFallbackKeys.removeItem(originalKeyCode);
}
bool InputState::shouldCancelKey(const KeyMemento& memento, const CancelationOptions& options) {
    if (options.keyCode && memento.keyCode != options.keyCode.value()) {
        return false;
    }
    if (options.deviceId && memento.deviceId != options.deviceId.value()) {
        return false;
    }
    if (options.displayId && memento.displayId != options.displayId.value()) {
        return false;
    }
    switch (options.mode) {
        case CancelationOptions::CANCEL_ALL_EVENTS:
        case CancelationOptions::CANCEL_NON_POINTER_EVENTS:
            return true;
        case CancelationOptions::CANCEL_FALLBACK_EVENTS:
            return memento.flags & AKEY_EVENT_FLAG_FALLBACK;
        default:
            return false;
    }
}
bool InputState::shouldCancelMotion(const MotionMemento& memento,
                                    const CancelationOptions& options) {
    if (options.deviceId && memento.deviceId != options.deviceId.value()) {
        return false;
    }
    if (options.displayId && memento.displayId != options.displayId.value()) {
        return false;
    }
    switch (options.mode) {
        case CancelationOptions::CANCEL_ALL_EVENTS:
            return true;
        case CancelationOptions::CANCEL_POINTER_EVENTS:
            return memento.source & AINPUT_SOURCE_CLASS_POINTER;
        case CancelationOptions::CANCEL_NON_POINTER_EVENTS:
            return !(memento.source & AINPUT_SOURCE_CLASS_POINTER);
        default:
            return false;
    }
}
}
