#define LOG_TAG "APM::Devices"
#include <audio_utils/string.h>
#include <media/TypeConverter.h>
#include <set>
#include "DeviceDescriptor.h"
#include "TypeConverter.h"
#include "HwModule.h"
namespace android {
void DeviceDescriptor::attach(const sp<HwModule> &module) {
  PolicyAudioPort::attach(module);
  mId = getNextUniqueId();
}
void DeviceDescriptor::detach() {
  mId = AUDIO_PORT_HANDLE_NONE;
  PolicyAudioPort::detach();
}
template <typename T>
bool checkEqual(const T &f1, const T &f2) {
  std::set<typename T::value_type> s1(f1.begin(), f1.end());
  std::set<typename T::value_type> s2(f2.begin(), f2.end());
  return s1 == s2;
}
bool DeviceDescriptor::equals(const sp<DeviceDescriptor> &other) const {
  if (other == 0) {
    return false;
  }
  return mDeviceTypeAddr.equals(other->mDeviceTypeAddr) &&
         checkEqual(mEncodedFormats, other->mEncodedFormats);
}
bool DeviceDescriptor::hasCurrentEncodedFormat() const {
  if (!device_has_encoding_capability(type())) {
    return true;
  }
  if (mEncodedFormats.empty()) {
    return true;
  }
  return (mCurrentEncodedFormat != AUDIO_FORMAT_DEFAULT);
}
bool DeviceDescriptor::supportsFormat(audio_format_t format) {
  if (mEncodedFormats.empty()) {
    return true;
  }
  for (const auto &devFormat : mEncodedFormats) {
    if (devFormat == format) {
      return true;
    }
  }
  return false;
}
status_t DeviceDescriptor::applyAudioPortConfig(
    const struct audio_port_config *config, audio_port_config *backupConfig) {
  struct audio_port_config localBackupConfig = {.config_mask =
                                                    config->config_mask};
  status_t status = NO_ERROR;
  toAudioPortConfig(&localBackupConfig);
  if ((status = validationBeforeApplyConfig(config)) == NO_ERROR) {
    AudioPortConfig::applyAudioPortConfig(config, backupConfig);
    applyPolicyAudioPortConfig(config);
  }
  if (backupConfig != NULL) {
    *backupConfig = localBackupConfig;
  }
  return status;
}
void DeviceDescriptor::toAudioPortConfig(
    struct audio_port_config *dstConfig,
    const struct audio_port_config *srcConfig) const {
  DeviceDescriptorBase::toAudioPortConfig(dstConfig, srcConfig);
  toPolicyAudioPortConfig(dstConfig, srcConfig);
  dstConfig->ext.device.hw_module = getModuleHandle();
}
void DeviceDescriptor::toAudioPort(struct audio_port *port) const {
<<<<<<< HEAD
  ALOGV("DeviceDescriptor::toAudioPort() handle %d type %08x", mId,
        mDeviceTypeAddr.mType);
  DeviceDescriptorBase::toAudioPort(port);
||||||| 5f90ad4290
  ALOGV("DeviceDescriptor::toAudioPort() handle %d type %08x", mId,
        mDeviceType);
  AudioPort::toAudioPort(port);
  port->id = mId;
  toAudioPortConfig(&port->active_config);
  port->ext.device.type = mDeviceType;
=======
  ALOGV("DeviceDescriptor::toAudioPort() handle %d type %08x", mId,
        mDeviceType);
  DeviceDescriptorBase::toAudioPort(port);
>>>>>>> 80ffad94f2b41098bad4f35f2399eb05e8c5ee34
  port->ext.device.hw_module = getModuleHandle();
}
void DeviceDescriptor::importAudioPortAndPickAudioProfile(
    const sp<PolicyAudioPort> &policyPort, bool force) {
  if (!force && !policyPort->asAudioPort()->hasDynamicAudioProfile()) {
    return;
  }
  AudioPort::importAudioPort(policyPort->asAudioPort());
  policyPort->pickAudioProfile(mSamplingRate, mChannelMask, mFormat);
}
void DeviceDescriptor::dump(String8 *dst, int spaces, int index,
                            bool verbose) const {
  String8 extraInfo;
  if (!mTagName.empty()) {
    extraInfo.appendFormat("%*s- tag name: %s\n", spaces, "", mTagName.c_str());
  }
  std::string descBaseDumpStr;
  DeviceDescriptorBase::dump(&descBaseDumpStr, spaces, index,
                             extraInfo.string(), verbose);
  dst->append(descBaseDumpStr.c_str());
}
void DeviceVector::refreshTypes() {
  mDeviceTypes.clear();
  for (size_t i = 0; i < size(); i++) {
    mDeviceTypes.insert(itemAt(i)->type());
  }
  ALOGV("DeviceVector::refreshTypes() mDeviceTypes %s",
        dumpDeviceTypes(mDeviceTypes).c_str());
}
ssize_t DeviceVector::indexOf(const sp<DeviceDescriptor> &item) const {
  for (size_t i = 0; i < size(); i++) {
    if (itemAt(i)->equals(
            item)) {
      return i;
    }
  }
  return -1;
}
void DeviceVector::add(const DeviceVector &devices) {
  bool added = false;
  for (const auto &device : devices) {
    if (indexOf(device) < 0 && SortedVector::add(device) >= 0) {
      added = true;
    }
  }
  if (added) {
    refreshTypes();
  }
}
ssize_t DeviceVector::add(const sp<DeviceDescriptor> &item) {
  ssize_t ret = indexOf(item);
  if (ret < 0) {
    ret = SortedVector::add(item);
    if (ret >= 0) {
      refreshTypes();
    }
  } else {
    ALOGW("DeviceVector::add device %08x already in", item->type());
    ret = -1;
  }
  return ret;
}
ssize_t DeviceVector::remove(const sp<DeviceDescriptor> &item) {
  ssize_t ret = indexOf(item);
  if (ret < 0) {
    ALOGW("DeviceVector::remove device %08x not in", item->type());
  } else {
    ret = SortedVector::removeAt(ret);
    if (ret >= 0) {
      refreshTypes();
    }
  }
  return ret;
}
void DeviceVector::remove(const DeviceVector &devices) {
  for (const auto &device : devices) {
    remove(device);
  }
}
DeviceVector DeviceVector::getDevicesFromHwModule(
    audio_module_handle_t moduleHandle) const {
  DeviceVector devices;
  for (const auto &device : *this) {
    if (device->getModuleHandle() == moduleHandle) {
      devices.add(device);
    }
  }
  return devices;
}
sp<DeviceDescriptor> DeviceVector::getDevice(audio_devices_t type,
                                             const String8 &address,
                                             audio_format_t format) const {
  sp<DeviceDescriptor> device;
  for (size_t i = 0; i < size(); i++) {
    if (itemAt(i)->type() == type) {
      if (((address == "" ||
            (itemAt(i)->address().compare(address.c_str()) == 0)) &&
           format == AUDIO_FORMAT_DEFAULT) ||
          (itemAt(i)->supportsFormat(format) &&
           format != AUDIO_FORMAT_DEFAULT)) {
        device = itemAt(i);
        if (itemAt(i)->address().compare(address.c_str()) == 0) {
          break;
        }
      }
    }
  }
  ALOGV("DeviceVector::%s() for type %08x address \"%s\" found %p format %08x",
        __func__, type, address.string(), device.get(), format);
  return device;
}
sp<DeviceDescriptor> DeviceVector::getDeviceFromId(
    audio_port_handle_t id) const {
  if (id != AUDIO_PORT_HANDLE_NONE) {
    for (const auto &device : *this) {
      if (device->getId() == id) {
        return device;
      }
    }
  }
  return nullptr;
}
DeviceVector DeviceVector::getDevicesFromTypes(
    const DeviceTypeSet &types) const {
  DeviceVector devices;
  if (types.empty()) {
    return devices;
  }
  for (size_t i = 0; i < size(); i++) {
    if (types.count(itemAt(i)->type()) != 0) {
      devices.add(itemAt(i));
      ALOGV("DeviceVector::%s() for type %08x found %p", __func__,
            itemAt(i)->type(), itemAt(i).get());
    }
  }
  return devices;
}
sp<DeviceDescriptor> DeviceVector::getDeviceFromTagName(
    const std::string &tagName) const {
  for (const auto &device : *this) {
    if (device->getTagName() == tagName) {
      return device;
    }
  }
  return nullptr;
}
DeviceVector DeviceVector::getFirstDevicesFromTypes(
    std::vector<audio_devices_t> orderedTypes) const {
  DeviceVector devices;
  for (auto deviceType : orderedTypes) {
    if (!(devices = getDevicesFromType(deviceType)).isEmpty()) {
      break;
    }
  }
  return devices;
}
sp<DeviceDescriptor> DeviceVector::getFirstExistingDevice(
    std::vector<audio_devices_t> orderedTypes) const {
  sp<DeviceDescriptor> device;
  for (auto deviceType : orderedTypes) {
    if ((device = getDevice(deviceType, String8(""), AUDIO_FORMAT_DEFAULT)) !=
        nullptr) {
      break;
    }
  }
  return device;
}
sp<DeviceDescriptor> DeviceVector::getDeviceForOpening() const {
  if (isEmpty()) {
    return nullptr;
  } else if (areAllOfSameDeviceType(types(), audio_is_input_device)) {
    return size() > 1 ? nullptr : *begin();
  } else if (areAllOfSameDeviceType(types(), audio_is_output_device)) {
    audio_devices_t deviceType = apm_extract_one_audio_device(types());
    return deviceType == AUDIO_DEVICE_NONE
               ? nullptr
               : getDevice(deviceType, String8(""), AUDIO_FORMAT_DEFAULT);
  }
  return nullptr;
}
void DeviceVector::replaceDevicesByType(audio_devices_t typeToRemove,
                                        const DeviceVector &devicesToAdd) {
  DeviceVector devicesToRemove = getDevicesFromType(typeToRemove);
  if (!devicesToRemove.isEmpty() && !devicesToAdd.isEmpty()) {
    remove(devicesToRemove);
    add(devicesToAdd);
  }
}
void DeviceVector::dump(String8 *dst, const String8 &tag, int spaces,
                        bool verbose) const {
  if (isEmpty()) {
    return;
  }
  dst->appendFormat("%*s- %s devices:\n", spaces, "", tag.string());
  for (size_t i = 0; i < size(); i++) {
    itemAt(i)->dump(dst, spaces + 2, i, verbose);
  }
}
std::string DeviceVector::toString() const {
  if (isEmpty()) {
    return {"AUDIO_DEVICE_NONE"};
  }
  std::string result = {"{"};
  for (const auto &device : *this) {
    if (device != *begin()) {
      result += ";";
    }
    result += device->toString();
  }
  return result + "}";
}
DeviceVector DeviceVector::filter(const DeviceVector &devices) const {
  DeviceVector filteredDevices;
  for (const auto &device : *this) {
    if (devices.contains(device)) {
      filteredDevices.add(device);
    }
  }
  return filteredDevices;
}
bool DeviceVector::containsAtLeastOne(const DeviceVector &devices) const {
  return !filter(devices).isEmpty();
}
bool DeviceVector::containsAllDevices(const DeviceVector &devices) const {
  return filter(devices).size() == devices.size();
}
DeviceVector DeviceVector::filterForEngine() const {
  DeviceVector filteredDevices;
  for (const auto &device : *this) {
    if (audio_is_remote_submix_device(device->type()) &&
        device->address() != "0") {
      continue;
    }
    filteredDevices.add(device);
  }
  return filteredDevices;
}
DeviceDescriptor::DeviceDescriptor(audio_devices_t type)
    : DeviceDescriptor(type, "" ) {}
DeviceDescriptor::DeviceDescriptor(audio_devices_t type,
                                   const std::string &tagName,
                                   const FormatVector &encodedFormats)
    : DeviceDescriptor(type, tagName, "" , encodedFormats) {}
DeviceDescriptor::DeviceDescriptor(audio_devices_t type,
                                   const std::string &tagName,
                                   const std::string &address,
                                   const FormatVector &encodedFormats)
    : DeviceDescriptor(AudioDeviceTypeAddr(type, address), tagName,
                       encodedFormats) {}
DeviceDescriptor::DeviceDescriptor(const AudioDeviceTypeAddr &deviceTypeAddr,
                                   const std::string &tagName,
                                   const FormatVector &encodedFormats)
    : DeviceDescriptorBase(deviceTypeAddr),
      mTagName(tagName),
      mEncodedFormats(encodedFormats) {
  mCurrentEncodedFormat = AUDIO_FORMAT_DEFAULT;
  if (mDeviceTypeAddr.mType == AUDIO_DEVICE_OUT_HDMI &&
      mEncodedFormats.empty()) {
    mEncodedFormats.push_back(AUDIO_FORMAT_AC3);
    mEncodedFormats.push_back(AUDIO_FORMAT_IEC61937);
  }
}
}
