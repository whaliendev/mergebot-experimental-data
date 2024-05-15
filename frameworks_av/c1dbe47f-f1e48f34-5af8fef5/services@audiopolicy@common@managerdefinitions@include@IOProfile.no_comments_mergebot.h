       
#include "AudioPort.h"
#include "DeviceDescriptor.h"
#include "PolicyAudioPort.h"
#include "policy.h"
#include <media/AudioContainers.h>
#include <utils/String8.h>
#include <system/audio.h>
namespace android {
class HwModule;
class IOProfile : public AudioPort, public PolicyAudioPort {
 public:
  IOProfile(const String8 &name, const std::string &name,
            audio_port_role_t role)
      : AudioPort(name, AUDIO_PORT_TYPE_MIX, role),
        maxOpenCount(1),
        curOpenCount(0),
        maxActiveCount(1),
        curActiveCount(0) {}
  ~IOProfile() = default;
  virtual const std::string getTagName() const { return getName(); }
  virtual void addAudioProfile(const sp<AudioProfile> &profile) {
    addAudioProfileAndSort(mProfiles, profile);
  }
  virtual sp<AudioPort> asAudioPort() const {
    return static_cast<AudioPort *>(const_cast<IOProfile *>(this));
  }
  void setFlags(uint32_t flags) override {
    PolicyAudioPort::setFlags(flags);
    if (getRole() == AUDIO_PORT_ROLE_SINK &&
        (flags & AUDIO_INPUT_FLAG_MMAP_NOIRQ) != 0) {
      maxActiveCount = 0;
    }
  }
  bool isCompatibleProfile(const DeviceVector &devices, uint32_t samplingRate,
                           uint32_t *updatedSamplingRate, audio_format_t format,
                           audio_format_t *updatedFormat,
                           audio_channel_mask_t channelMask,
                           audio_channel_mask_t *updatedChannelMask,
                           uint32_t flags,
                           bool exactMatchRequiredForInputFlags = false) const;
  void dump(String8 *dst) const;
  void log();
  bool hasSupportedDevices() const { return !mSupportedDevices.isEmpty(); }
  bool supportsDeviceTypes(const DeviceTypeSet &deviceTypes) const {
    const bool areOutputDevices =
        Intersection(deviceTypes, getAudioDeviceInAllSet()).empty();
    const bool devicesSupported =
        !mSupportedDevices.getDevicesFromTypes(deviceTypes).empty();
    return devicesSupported &&
           (!areOutputDevices || devicesSupportEncodedFormats(deviceTypes));
  }
  bool supportsDevice(const sp<DeviceDescriptor> &device,
                      bool forceCheckOnAddress = false) const {
    if (!device_distinguishes_on_address(device->type()) &&
        !forceCheckOnAddress) {
      return supportsDeviceTypes(DeviceTypeSet({device->type()}));
    }
    return mSupportedDevices.contains(device);
  }
  bool devicesSupportEncodedFormats(DeviceTypeSet deviceTypes) const {
    if (deviceTypes.empty()) {
      return true;
    }
    DeviceVector deviceList =
        mSupportedDevices.getDevicesFromTypes(deviceTypes);
    if (!deviceList.empty()) {
      return deviceList.itemAt(0)->hasCurrentEncodedFormat();
    }
    return false;
  }
  void clearSupportedDevices() { mSupportedDevices.clear(); }
  void addSupportedDevice(const sp<DeviceDescriptor> &device) {
    mSupportedDevices.add(device);
  }
  void removeSupportedDevice(const sp<DeviceDescriptor> &device) {
    mSupportedDevices.remove(device);
  }
  void setSupportedDevices(const DeviceVector &devices) {
    mSupportedDevices = devices;
  }
  const DeviceVector &getSupportedDevices() const { return mSupportedDevices; }
  bool canOpenNewIo() {
    if (maxOpenCount == 0 || curOpenCount < maxOpenCount) {
      return true;
    }
    return false;
  }
  bool canStartNewIo() {
    if (maxActiveCount == 0 || curActiveCount < maxActiveCount) {
      return true;
    }
    return false;
  }
  uint32_t maxOpenCount;
  uint32_t curOpenCount;
  uint32_t maxActiveCount;
  uint32_t curActiveCount;
 private:
  DeviceVector
      mSupportedDevices;
};
class InputProfile : public IOProfile {
 public:
  explicitInputProfile(const std::string &name)
      : IOProfile(name, AUDIO_PORT_ROLE_SINK) {}
};
class OutputProfile : public IOProfile {
 public:
  explicitOutputProfile(const std::string &name)
      : IOProfile(name, AUDIO_PORT_ROLE_SOURCE) {}
};
}
