#ifndef _LIBINPUT_KEY_LAYOUT_MAP_H
#define _LIBINPUT_KEY_LAYOUT_MAP_H 
#include <android-base/result.h>
#include <stdint.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/RefBase.h>
#include <utils/Tokenizer.h>
#include <set>
#include <input/InputDevice.h>
namespace android {
struct AxisInfo {
    enum Mode {
        MODE_NORMAL = 0,
        MODE_INVERT = 1,
        MODE_SPLIT = 2,
    };
    Mode mode;
    int32_t axis;
    int32_t highAxis;
    int32_t splitValue;
    int32_t flatOverride;
    AxisInfo() : mode(MODE_NORMAL), axis(-1), highAxis(-1), splitValue(0), flatOverride(-1) {}
};
class KeyLayoutMap {
public:
    static base::Result<std::shared_ptr<KeyLayoutMap>> load(const std::string& filename,
                                                            const char* contents = nullptr);
    static base::Result<std::shared_ptr<KeyLayoutMap>> loadContents(const std::string& filename,
                                                                    const char* contents);
    status_t mapKey(int32_t scanCode, int32_t usageCode, int32_t* outKeyCode,
                    uint32_t* outFlags) const;
    std::vector<int32_t> findScanCodesForKey(int32_t keyCode) const;
    std::optional<int32_t> findScanCodeForLed(int32_t ledCode) const;
    const std::string getLoadFileName() const;
    base::Result<std::pair<InputDeviceSensorType, int32_t>> mapSensor(int32_t absCode);
    virtual ~KeyLayoutMap();
private:
    static base::Result<std::shared_ptr<KeyLayoutMap>> load(Tokenizer* tokenizer);
    struct Key {
        int32_t keyCode;
        uint32_t flags;
    };
    struct Led {
        int32_t ledCode;
    };
    struct Sensor {
        InputDeviceSensorType sensorType;
        int32_t sensorDataIndex;
    };
    std::unordered_map<int32_t, Key> mKeysByScanCode;
    std::unordered_map<int32_t, Key> mKeysByUsageCode;
    std::unordered_map<int32_t, AxisInfo> mAxes;
    std::unordered_map<int32_t, Led> mLedsByScanCode;
    std::unordered_map<int32_t, Led> mLedsByUsageCode;
    std::unordered_map<int32_t, Sensor> mSensorsByAbsCode;
    std::set<std::string> mRequiredKernelConfigs;
    std::string mLoadFileName;
    KeyLayoutMap();
    const Key* getKey(int32_t scanCode, int32_t usageCode) const;
    class Parser {
        KeyLayoutMap* mMap;
        Tokenizer* mTokenizer;
    public:
        Parser(KeyLayoutMap* map, Tokenizer* tokenizer);
        ~Parser();
        status_t parse();
    private:
        status_t parseKey();
        status_t parseAxis();
        status_t parseLed();
        status_t parseSensor();
        status_t parseRequiredKernelConfig();
    };
public:
    std::optional<int32_t> findUsageCodeForLed(int32_t ledCode) const;
    std::optional<AxisInfo> mapAxis(int32_t scanCode) const;
};
}
#endif
