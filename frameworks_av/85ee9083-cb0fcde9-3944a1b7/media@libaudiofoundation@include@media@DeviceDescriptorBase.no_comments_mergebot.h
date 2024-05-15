       
#include <vector>
#include <binder/Parcel.h>
#include <binder/Parcelable.h>
#include <media/AudioContainers.h>
#include <media/AudioPort.h>
#include <media/AudioDeviceTypeAddr.h>
#include <utils/Errors.h>
#include <cutils/config_utils.h>
#include <system/audio.h>
#include <system/audio_policy.h>
namespace android {
class DeviceDescriptorBase : public AudioPort, public AudioPortConfig {
 protected:
  AudioDeviceTypeAddr mDeviceTypeAddr;
 public:
  explicit DeviceDescriptorBase(audio_devices_t type);
  DeviceDescriptorBase(audio_devices_t type, const std::string& address);
  explicit DeviceDescriptorBase(const AudioDeviceTypeAddr& deviceTypeAddr);
  virtual ~DeviceDescriptorBase() {}
  audio_devices_t type() const { return mDeviceTypeAddr.mType; }
  std::string address() const { return mDeviceTypeAddr.mAddress; }
  void setAddress(const std::string& address) {
    mDeviceTypeAddr.mAddress = address;
  }
  const AudioDeviceTypeAddr& getDeviceTypeAddr() const {
    return mDeviceTypeAddr;
  }
  virtual sp<AudioPort> getAudioPort() const {
    return static_cast<AudioPort*>(const_cast<DeviceDescriptorBase*>(this));
  }
  virtual void toAudioPortConfig(
      struct audio_port_config* dstConfig,
      const struct audio_port_config* srcConfig = NULL) const;
  virtual void toAudioPort(struct audio_port* port) const;
  void dump(std::string* dst, int spaces, int index,
            const char* extraInfo = nullptr, bool verbose = true) const;
  void log() const;
  std::string toString() const;
  bool equals(const sp<DeviceDescriptorBase>& other) const;
  status_t writeToParcel(Parcel* parcel) const override;
  status_t readFromParcel(const Parcel* parcel) override;
};
using DeviceDescriptorBaseVector = std::vector<sp<DeviceDescriptorBase>>;
std::string toString(const DeviceDescriptorBaseVector& devices);
AudioDeviceTypeAddrVector deviceTypeAddrsFromDescriptors(
    const DeviceDescriptorBaseVector& devices);
}
