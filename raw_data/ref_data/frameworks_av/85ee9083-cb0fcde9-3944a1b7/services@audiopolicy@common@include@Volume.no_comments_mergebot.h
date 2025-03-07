       
#include <media/AudioCommonTypes.h>
#include <media/AudioContainers.h>
#include <system/audio.h>
#include <utils/Log.h>
#include <math.h>
#include "policy.h"
namespace android {
enum VolumeSource : std::underlying_type<volume_group_t>::type;
static const VolumeSource VOLUME_SOURCE_NONE =
    static_cast<VolumeSource>(VOLUME_GROUP_NONE);
}
#define VOLUME_MIN_DB (-758)
class VolumeCurvePoint {
 public:
  int mIndex;
  float mDBAttenuation;
};
enum device_category {
  DEVICE_CATEGORY_HEADSET,
  DEVICE_CATEGORY_SPEAKER,
  DEVICE_CATEGORY_EARPIECE,
  DEVICE_CATEGORY_EXT_MEDIA,
  DEVICE_CATEGORY_HEARING_AID,
  DEVICE_CATEGORY_CNT
};
class Volume {
 public:
  enum {
    VOLMIN = 0,
    VOLKNEE1 = 1,
    VOLKNEE2 = 2,
    VOLMAX = 3,
    VOLCNT = 4
  };
  static audio_devices_t getDeviceForVolume(
      const android::DeviceTypeSet& deviceTypes) {
    audio_devices_t deviceType = AUDIO_DEVICE_NONE;
    if (deviceTypes.empty()) {
      deviceType = AUDIO_DEVICE_OUT_SPEAKER;
    } else if (deviceTypes.size() > 1) {
      if (deviceTypes.count(AUDIO_DEVICE_OUT_SPEAKER) != 0) {
        deviceType = AUDIO_DEVICE_OUT_SPEAKER;
      } else if (deviceTypes.count(AUDIO_DEVICE_OUT_SPEAKER_SAFE) != 0) {
        deviceType = AUDIO_DEVICE_OUT_SPEAKER_SAFE;
      } else if (deviceTypes.count(AUDIO_DEVICE_OUT_HDMI_ARC) != 0) {
        deviceType = AUDIO_DEVICE_OUT_HDMI_ARC;
      } else if (deviceTypes.count(AUDIO_DEVICE_OUT_AUX_LINE) != 0) {
        deviceType = AUDIO_DEVICE_OUT_AUX_LINE;
      } else if (deviceTypes.count(AUDIO_DEVICE_OUT_SPDIF) != 0) {
        deviceType = AUDIO_DEVICE_OUT_SPDIF;
      } else {
        std::vector<audio_devices_t> a2dpDevices = android::Intersection(
            deviceTypes, android::getAudioDeviceOutAllA2dpSet());
        if (a2dpDevices.size() > 1) {
          ALOGW("getDeviceForVolume() invalid device combination: %s",
                android::dumpDeviceTypes(deviceTypes).c_str());
        }
        if (!a2dpDevices.empty()) {
          deviceType = a2dpDevices[0];
        }
      }
    } else {
      deviceType = *(deviceTypes.begin());
    }
    if (deviceType == AUDIO_DEVICE_OUT_SPEAKER_SAFE) {
      deviceType = AUDIO_DEVICE_OUT_SPEAKER;
    }
    ALOGW_IF(deviceType == AUDIO_DEVICE_NONE,
             "getDeviceForVolume() invalid device combination: %s, returning "
             "AUDIO_DEVICE_NONE",
             android::dumpDeviceTypes(deviceTypes).c_str());
    return deviceType;
  }
  static device_category getDeviceCategory(
      const android::DeviceTypeSet& deviceTypes) {
    switch (getDeviceForVolume(deviceTypes)) {
      case AUDIO_DEVICE_OUT_EARPIECE:
        return DEVICE_CATEGORY_EARPIECE;
      case AUDIO_DEVICE_OUT_WIRED_HEADSET:
      case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
      case AUDIO_DEVICE_OUT_BLUETOOTH_SCO:
      case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
      case AUDIO_DEVICE_OUT_BLUETOOTH_A2DP:
      case AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES:
      case AUDIO_DEVICE_OUT_USB_HEADSET:
        return DEVICE_CATEGORY_HEADSET;
      case AUDIO_DEVICE_OUT_HEARING_AID:
        return DEVICE_CATEGORY_HEARING_AID;
      case AUDIO_DEVICE_OUT_LINE:
      case AUDIO_DEVICE_OUT_AUX_DIGITAL:
      case AUDIO_DEVICE_OUT_USB_DEVICE:
        return DEVICE_CATEGORY_EXT_MEDIA;
      case AUDIO_DEVICE_OUT_SPEAKER:
      case AUDIO_DEVICE_OUT_SPEAKER_SAFE:
      case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
      case AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER:
      case AUDIO_DEVICE_OUT_USB_ACCESSORY:
      case AUDIO_DEVICE_OUT_REMOTE_SUBMIX:
      default:
        return DEVICE_CATEGORY_SPEAKER;
    }
  }
  static inline float DbToAmpl(float decibels) {
    if (decibels <= VOLUME_MIN_DB) {
      return 0.0f;
    }
    return exp(decibels * 0.115129f);
  }
  static inline float AmplToDb(float amplification) {
    if (amplification == 0) {
      return VOLUME_MIN_DB;
    }
    return 20 * log10(amplification);
  }
};
