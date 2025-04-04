#ifndef _UI_INPUT_INPUTDISPATCHER_ENTRY_H
#define _UI_INPUT_INPUTDISPATCHER_ENTRY_H 
#include "InjectionState.h"
#include "InputTarget.h"
#include <input/Input.h>
#include <input/InputApplication.h>
#include <stdint.h>
#include <utils/Timers.h>
#include <functional>
#include <string>
namespace android::inputdispatcher {
<<<<<<< HEAD
||||||| f7bda296de
constexpr uint32_t SYNTHESIZED_EVENT_SEQUENCE_NUM = 0;
=======
constexpr int32_t SYNTHESIZED_EVENT_ID = 0;
>>>>>>> eaf88010
struct EventEntry {
    enum class Type {
        CONFIGURATION_CHANGED,
        DEVICE_RESET,
        FOCUS,
        KEY,
        MOTION,
    };
    static const char* typeToString(Type type) {
        switch (type) {
            case Type::CONFIGURATION_CHANGED:
                return "CONFIGURATION_CHANGED";
            case Type::DEVICE_RESET:
                return "DEVICE_RESET";
            case Type::FOCUS:
                return "FOCUS";
            case Type::KEY:
                return "KEY";
            case Type::MOTION:
                return "MOTION";
        }
    }
    int32_t id;
    mutable int32_t refCount;
    Type type;
    nsecs_t eventTime;
    uint32_t policyFlags;
    InjectionState* injectionState;
    bool dispatchInProgress;
    inline bool isInjected() const { return injectionState != nullptr; }
<<<<<<< HEAD
    inline bool isSynthesized() const {
        return isInjected() || IdGenerator::getSource(id) != IdGenerator::Source::INPUT_READER;
    }
||||||| f7bda296de
    inline bool isSynthesized() const {
        return isInjected() || sequenceNum == SYNTHESIZED_EVENT_SEQUENCE_NUM;
    }
=======
    inline bool isSynthesized() const { return isInjected() || id == SYNTHESIZED_EVENT_ID; }
>>>>>>> eaf88010
    void release();
    virtual void appendDescription(std::string& msg) const = 0;
protected:
    EventEntry(int32_t id, Type type, nsecs_t eventTime, uint32_t policyFlags);
    virtual ~EventEntry();
    void releaseInjectionState();
};
struct ConfigurationChangedEntry : EventEntry {
    explicit ConfigurationChangedEntry(int32_t id, nsecs_t eventTime);
    virtual void appendDescription(std::string& msg) const;
protected:
    virtual ~ConfigurationChangedEntry();
};
struct DeviceResetEntry : EventEntry {
    int32_t deviceId;
    DeviceResetEntry(int32_t id, nsecs_t eventTime, int32_t deviceId);
    virtual void appendDescription(std::string& msg) const;
protected:
    virtual ~DeviceResetEntry();
};
struct FocusEntry : EventEntry {
    sp<IBinder> connectionToken;
    bool hasFocus;
    FocusEntry(int32_t id, nsecs_t eventTime, sp<IBinder> connectionToken, bool hasFocus);
    virtual void appendDescription(std::string& msg) const;
protected:
    virtual ~FocusEntry();
};
struct KeyEntry : EventEntry {
    int32_t deviceId;
    uint32_t source;
    int32_t displayId;
    int32_t action;
    int32_t flags;
    int32_t keyCode;
    int32_t scanCode;
    int32_t metaState;
    int32_t repeatCount;
    nsecs_t downTime;
    bool syntheticRepeat;
    enum InterceptKeyResult {
        INTERCEPT_KEY_RESULT_UNKNOWN,
        INTERCEPT_KEY_RESULT_SKIP,
        INTERCEPT_KEY_RESULT_CONTINUE,
        INTERCEPT_KEY_RESULT_TRY_AGAIN_LATER,
    };
    InterceptKeyResult interceptKeyResult;
    nsecs_t interceptKeyWakeupTime;
    KeyEntry(int32_t id, nsecs_t eventTime, int32_t deviceId, uint32_t source, int32_t displayId,
             uint32_t policyFlags, int32_t action, int32_t flags, int32_t keyCode, int32_t scanCode,
             int32_t metaState, int32_t repeatCount, nsecs_t downTime);
    virtual void appendDescription(std::string& msg) const;
    void recycle();
protected:
    virtual ~KeyEntry();
};
struct MotionEntry : EventEntry {
    nsecs_t eventTime;
    int32_t deviceId;
    uint32_t source;
    int32_t displayId;
    int32_t action;
    int32_t actionButton;
    int32_t flags;
    int32_t metaState;
    int32_t buttonState;
    MotionClassification classification;
    int32_t edgeFlags;
    float xPrecision;
    float yPrecision;
    float xCursorPosition;
    float yCursorPosition;
    nsecs_t downTime;
    uint32_t pointerCount;
    PointerProperties pointerProperties[MAX_POINTERS];
    PointerCoords pointerCoords[MAX_POINTERS];
    MotionEntry(int32_t id, nsecs_t eventTime, int32_t deviceId, uint32_t source, int32_t displayId,
                uint32_t policyFlags, int32_t action, int32_t actionButton, int32_t flags,
                int32_t metaState, int32_t buttonState, MotionClassification classification,
                int32_t edgeFlags, float xPrecision, float yPrecision, float xCursorPosition,
                float yCursorPosition, nsecs_t downTime, uint32_t pointerCount,
                const PointerProperties* pointerProperties, const PointerCoords* pointerCoords,
                float xOffset, float yOffset);
    virtual void appendDescription(std::string& msg) const;
protected:
    virtual ~MotionEntry();
};
struct DispatchEntry {
    const uint32_t seq;
    EventEntry* eventEntry;
    int32_t targetFlags;
    float xOffset;
    float yOffset;
    float globalScaleFactor;
    float windowXScale = 1.0f;
    float windowYScale = 1.0f;
    nsecs_t deliveryTime;
    int32_t resolvedEventId;
    int32_t resolvedAction;
    int32_t resolvedFlags;
    DispatchEntry(EventEntry* eventEntry, int32_t targetFlags, float xOffset, float yOffset,
                  float globalScaleFactor, float windowXScale, float windowYScale);
    ~DispatchEntry();
    inline bool hasForegroundTarget() const { return targetFlags & InputTarget::FLAG_FOREGROUND; }
    inline bool isSplit() const { return targetFlags & InputTarget::FLAG_SPLIT; }
private:
    static volatile int32_t sNextSeqAtomic;
    static uint32_t nextSeq();
};
VerifiedKeyEvent verifiedKeyEventFromKeyEntry(const KeyEntry& entry);
VerifiedMotionEvent verifiedMotionEventFromMotionEntry(const MotionEntry& entry);
class InputDispatcher;
struct CommandEntry;
typedef std::function<void(InputDispatcher&, CommandEntry*)> Command;
class Connection;
struct CommandEntry {
    explicit CommandEntry(Command command);
    ~CommandEntry();
    Command command;
    sp<Connection> connection;
    nsecs_t eventTime;
    KeyEntry* keyEntry;
    sp<InputApplicationHandle> inputApplicationHandle;
    std::string reason;
    int32_t userActivityEventType;
    uint32_t seq;
    bool handled;
    sp<InputChannel> inputChannel;
    sp<IBinder> oldToken;
    sp<IBinder> newToken;
};
}
#endif
