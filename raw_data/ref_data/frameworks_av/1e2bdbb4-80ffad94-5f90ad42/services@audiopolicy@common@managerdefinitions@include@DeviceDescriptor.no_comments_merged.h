       
#include "PolicyAudioPort.h"
#include <media/AudioContainers.h>
#include <media/DeviceDescriptorBase.h>
#include <utils/Errors.h>
#include <utils/String8.h>
#include <utils/SortedVector.h>
#include <cutils/config_utils.h>
#include <system/audio.h>
#include <system/audio_policy.h>
namespace android {
class DeviceDescriptor : public DeviceDescriptorBase,
                         public PolicyAudioPort, public PolicyAudioPortConfig
{
public:
    explicit DeviceDescriptor(audio_devices_t type);
    DeviceDescriptor(audio_devices_t type, const std::string &tagName,
            const FormatVector &encodedFormats = FormatVector{});
    DeviceDescriptor(audio_devices_t type, const std::string &tagName,
            const std::string &address, const FormatVector &encodedFormats = FormatVector{});
    DeviceDescriptor(const AudioDeviceTypeAddr &deviceTypeAddr, const std::string &tagName = "",
            const FormatVector &encodedFormats = FormatVector{});
    virtual ~DeviceDescriptor() {}
    virtual void addAudioProfile(const sp<AudioProfile> &profile) {
        addAudioProfileAndSort(mProfiles, profile);
    }
    virtual const std::string getTagName() const { return mTagName; }
    const FormatVector& encodedFormats() const { return mEncodedFormats; }
    audio_format_t getEncodedFormat() { return mCurrentEncodedFormat; }
    void setEncodedFormat(audio_format_t format) {
        mCurrentEncodedFormat = format;
    }
    bool equals(const sp<DeviceDescriptor>& other) const;
    bool hasCurrentEncodedFormat() const;
    bool supportsFormat(audio_format_t format);
    virtual sp<PolicyAudioPort> getPolicyAudioPort() const {
        return static_cast<PolicyAudioPort*>(const_cast<DeviceDescriptor*>(this));
    }
    virtual status_t applyAudioPortConfig(const struct audio_port_config *config,
                                          struct audio_port_config *backupConfig = NULL);
    virtual void toAudioPortConfig(struct audio_port_config *dstConfig,
            const struct audio_port_config *srcConfig = NULL) const;
    virtual sp<AudioPort> asAudioPort() const {
        return static_cast<AudioPort*>(const_cast<DeviceDescriptor*>(this));
    }
    virtual void attach(const sp<HwModule>& module);
    virtual void detach();
    virtual void toAudioPort(struct audio_port *port) const;
    void importAudioPortAndPickAudioProfile(const sp<PolicyAudioPort>& policyPort,
                                            bool force = false);
    void dump(String8 *dst, int spaces, int index, bool verbose = true) const;
private:
    std::string mTagName;
    FormatVector mEncodedFormats;
    audio_format_t mCurrentEncodedFormat;
};
class DeviceVector : public SortedVector<sp<DeviceDescriptor> >
{
public:
    DeviceVector() : SortedVector() {}
    explicit DeviceVector(const sp<DeviceDescriptor>& item) : DeviceVector()
    {
        add(item);
    }
    ssize_t add(const sp<DeviceDescriptor>& item);
    void add(const DeviceVector &devices);
    ssize_t remove(const sp<DeviceDescriptor>& item);
    void remove(const DeviceVector &devices);
    ssize_t indexOf(const sp<DeviceDescriptor>& item) const;
    DeviceTypeSet types() const { return mDeviceTypes; }
    sp<DeviceDescriptor> getDevice(audio_devices_t type, const String8 &address,
                                   audio_format_t codec) const;
    DeviceVector getDevicesFromTypes(const DeviceTypeSet& types) const;
    DeviceVector getDevicesFromType(audio_devices_t type) const {
        return getDevicesFromTypes({type});
    }
    sp<DeviceDescriptor> getDeviceFromId(audio_port_handle_t id) const;
    sp<DeviceDescriptor> getDeviceFromTagName(const std::string &tagName) const;
    DeviceVector getDevicesFromHwModule(audio_module_handle_t moduleHandle) const;
    DeviceVector getFirstDevicesFromTypes(std::vector<audio_devices_t> orderedTypes) const;
    sp<DeviceDescriptor> getFirstExistingDevice(std::vector<audio_devices_t> orderedTypes) const;
    sp<DeviceDescriptor> getDeviceForOpening() const;
    void replaceDevicesByType(audio_devices_t typeToRemove, const DeviceVector &devicesToAdd);
    bool containsDeviceAmongTypes(const DeviceTypeSet& deviceTypes) const {
        return !Intersection(mDeviceTypes, deviceTypes).empty();
    }
    bool containsDeviceWithType(audio_devices_t deviceType) const {
        return containsDeviceAmongTypes({deviceType});
    }
    bool onlyContainsDevicesWithType(audio_devices_t deviceType) const {
        return isSingleDeviceType(mDeviceTypes, deviceType);
    }
    bool contains(const sp<DeviceDescriptor>& item) const { return indexOf(item) >= 0; }
    bool containsAtLeastOne(const DeviceVector &devices) const;
    bool containsAllDevices(const DeviceVector &devices) const;
    DeviceVector filter(const DeviceVector &devices) const;
    DeviceVector filterForEngine() const;
    ssize_t merge(const DeviceVector &devices)
    {
        if (isEmpty()) {
            add(devices);
            return size();
        }
        return SortedVector::merge(devices);
    }
    bool operator==(const DeviceVector &right) const
    {
        if (size() != right.size()) {
            return false;
        }
        for (const auto &device : *this) {
            if (right.indexOf(device) < 0) {
                return false;
            }
        }
        return true;
    }
    bool operator!=(const DeviceVector &right) const
    {
        return !operator==(right);
    }
    String8 getFirstValidAddress() const
    {
        for (const auto &device : *this) {
            if (device->address() != "") {
                return String8(device->address().c_str());
            }
        }
        return String8("");
    }
    std::string toString() const;
    void dump(String8 *dst, const String8 &tag, int spaces = 0, bool verbose = true) const;
private:
    void refreshTypes();
    DeviceTypeSet mDeviceTypes;
};
}
