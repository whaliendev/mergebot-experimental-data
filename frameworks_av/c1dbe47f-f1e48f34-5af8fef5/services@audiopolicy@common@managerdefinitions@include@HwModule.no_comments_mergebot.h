       
#include "DeviceDescriptor.h"
#include "AudioRoute.h"
#include <utils/RefBase.h>
#include <utils/String8.h>
#include <utils/Errors.h>
#include <utils/Vector.h>
#include <system/audio.h>
#include <cutils/config_utils.h>
#include <string>
namespace android {
class IOProfile;
class InputProfile;
class OutputProfile;
typedef Vector<sp<IOProfile> > InputProfileCollection;
typedef Vector<sp<IOProfile> > OutputProfileCollection;
typedef Vector<sp<IOProfile> > IOProfileCollection;
class HwModule : public RefBase {
 public:
  explicit HwModule(const char *name, uint32_t halVersionMajor = 0,
                    uint32_t halVersionMinor = 0);
  ~HwModule();
  const char *getName() const { return mName.string(); }
  const DeviceVector &getDeclaredDevices() const { return mDeclaredDevices; }
  void setDeclaredDevices(const DeviceVector &devices);
  DeviceVector getAllDevices() const {
    DeviceVector devices = mDeclaredDevices;
    devices.merge(mDynamicDevices);
    return devices;
  }
  void addDynamicDevice(const sp<DeviceDescriptor> &device) {
    mDynamicDevices.add(device);
  }
  bool removeDynamicDevice(const sp<DeviceDescriptor> &device) {
    return mDynamicDevices.remove(device) >= 0;
  }
  DeviceVector getDynamicDevices() const { return mDynamicDevices; }
  const InputProfileCollection &getInputProfiles() const {
    return mInputProfiles;
  }
  const OutputProfileCollection &getOutputProfiles() const {
    return mOutputProfiles;
  }
  void setProfiles(const IOProfileCollection &profiles);
  void setHalVersion(uint32_t major, uint32_t minor) {
    mHalVersion = (major << 8) | (minor & 0xff);
  }
  uint32_t getHalVersionMajor() const { return mHalVersion >> 8; }
  uint32_t getHalVersionMinor() const { return mHalVersion & 0xff; }
  sp<DeviceDescriptor> getRouteSinkDevice(const sp<AudioRoute> &route) const;
  DeviceVector getRouteSourceDevices(const sp<AudioRoute> &route) const;
  void setRoutes(const AudioRouteVector &routes);
  status_t addOutputProfile(const sp<IOProfile> &profile);
  status_t addInputProfile(const sp<IOProfile> &profile);
  status_t addProfile(const sp<IOProfile> &profile);
  status_t addOutputProfile(const std::string &name,
                            const audio_config_t *config,
                            audio_devices_t device, const String8 &address);
  status_t removeOutputProfile(const std::string &name);
  status_t addInputProfile(const std::string &name,
                           const audio_config_t *config, audio_devices_t device,
                           const String8 &address);
  status_t removeInputProfile(const std::string &name);
  audio_module_handle_t getHandle() const { return mHandle; }
  void setHandle(audio_module_handle_t handle);
  sp<AudioPort> findPortByTagName(const std::string &tagName) const {
    return mPorts.findByTagName(tagName);
  }
  bool supportsPatch(const sp<PolicyAudioPort> &srcPort,
                     const sp<PolicyAudioPort> &dstPort) const;
  void dump(String8 *dst) const;
 private:
  void refreshSupportedDevices();
  const String8 mName;
  audio_module_handle_t mHandle;
  OutputProfileCollection
      mOutputProfiles;
  InputProfileCollection
      mInputProfiles;
  uint32_t mHalVersion;
  DeviceVector
      mDeclaredDevices;
  DeviceVector
      mDynamicDevices;
  AudioRouteVector mRoutes;
  PolicyAudioPortVector mPorts;
};
class HwModuleCollection : public Vector<sp<HwModule> > {
 public:
  sp<HwModule> getModuleFromName(const char *name) const;
  sp<HwModule> getModuleForDeviceType(audio_devices_t device,
                                      audio_format_t encodedFormat) const;
  sp<HwModule> getModuleForDevice(const sp<DeviceDescriptor> &device,
                                  audio_format_t encodedFormat) const;
  DeviceVector getAvailableDevicesFromModuleName(
      const char *name, const DeviceVector &availableDevices) const;
  sp<DeviceDescriptor> getDeviceDescriptor(const audio_devices_t type,
                                           const char *address,
                                           const char *name,
                                           audio_format_t encodedFormat,
                                           bool allowToCreate = false,
                                           bool matchAddress = true) const;
  sp<DeviceDescriptor> createDevice(const audio_devices_t type,
                                    const char *address, const char *name,
                                    const audio_format_t encodedFormat) const;
  void cleanUpForDevice(const sp<DeviceDescriptor> &device);
  void dump(String8 *dst) const;
};
}
