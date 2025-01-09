       
#include <algorithm> #include <functional> #include <iterator>
#include <set>
#include <vector>
#include <media/TypeConverter.h>
#include <system/audio.h>
namespace android {
using ChannelMaskSet = std::set<audio_channel_mask_t>;
using DeviceTypeSet = std::set<audio_devices_t>;
using FormatSet = std::set<audio_format_t>;
using SampleRateSet = std::set<uint32_t>;
using FormatVector = std::vector<audio_format_t>;
const DeviceTypeSet& getAudioDeviceOutAllSet(); const DeviceTypeSet& getAudioDeviceOutAllA2dpSet(); const DeviceTypeSet& getAudioDeviceOutAllScoSet(); const DeviceTypeSet& getAudioDeviceOutAllUsbSet(); const DeviceTypeSet& getAudioDeviceInAllSet(); const DeviceTypeSet& getAudioDeviceInAllUsbSet(); template<typename T> static std::vector<T> Intersection(const std::set<T>& a, const std::set<T>& b) { std::vector<T> intersection; std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(intersection)); return intersection; }
static inline ChannelMaskSet asInMask(const ChannelMaskSet& channelMasks) {
    ChannelMaskSet inMaskSet;
    for (const auto &channel : channelMasks) {
        if (audio_channel_mask_out_to_in(channel) != AUDIO_CHANNEL_INVALID) {
            inMaskSet.insert(audio_channel_mask_out_to_in(channel));
        }
    }
    return inMaskSet;
}
static inline ChannelMaskSet asOutMask(const ChannelMaskSet& channelMasks) {
    ChannelMaskSet outMaskSet;
    for (const auto &channel : channelMasks) {
        if (audio_channel_mask_in_to_out(channel) != AUDIO_CHANNEL_INVALID) {
            outMaskSet.insert(audio_channel_mask_in_to_out(channel));
        }
    }
    return outMaskSet;
}
static inline bool isSingleDeviceType(const DeviceTypeSet& deviceTypes, audio_devices_t deviceType) { return deviceTypes.size() == 1 && *(deviceTypes.begin()) == deviceType; } typedef bool (*DeviceTypeUnaryPredicate)(audio_devices_t); static inline bool isSingleDeviceType(const DeviceTypeSet& deviceTypes, DeviceTypeUnaryPredicate p) { return deviceTypes.size() == 1 && p(*(deviceTypes.begin())); } static inline bool areAllOfSameDeviceType(const DeviceTypeSet& deviceTypes, std::function<bool(audio_devices_t)> p) { return std::all_of(deviceTypes.begin(), deviceTypes.end(), p); } static inline void resetDeviceTypes(DeviceTypeSet& deviceTypes, audio_devices_t typeToAdd) { deviceTypes.clear(); deviceTypes.insert(typeToAdd); }
}
