#include <stdio.h>
#define LOG_TAG "DeviceHalHidl"
#include <cutils/native_handle.h>
#include <hwbinder/IPCThreadState.h>
#include <media/AudioContainers.h>
#include <utils/Log.h>
#include PATH(android/hardware/audio/FILE_VERSION/IPrimaryDevice.h)
#include <HidlUtils.h>
#include <common/all-versions/VersionUtils.h>
#include <util/CoreUtils.h>
#include "DeviceHalHidl.h"
#include "EffectHalHidl.h"
using ::android::hardware::audio::common::CPP_VERSION::implementation::HidlUtils;
#include "ParameterUtils.h"
using ::android::hardware::audio::CPP_VERSION::implementation::CoreUtils;
#include "StreamHalHidl.h"
using ::android::hardware::audio::common::COMMON_TYPES_CPP_VERSION::implementation::HidlUtils;
using ::android::hardware::audio::common::utils::EnumBitfield;
using ::android::hardware::audio::CORE_TYPES_CPP_VERSION::implementation::CoreUtils;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
namespace android {
using namespace ::android::hardware::audio::common::COMMON_TYPES_CPP_VERSION;
using namespace ::android::hardware::audio::CORE_TYPES_CPP_VERSION;
DeviceHalHidl::DeviceHalHidl(const sp<::android::hardware::audio::CPP_VERSION::IPrimaryDevice>& device): ConversionHelperHidl("Device"), #if MAJOR_VERSION <= 6 || (MAJOR_VERSION == 7 && MINOR_VERSION == 0)
          mDevice(device), #endif
          mPrimaryDevice(device) {
        return status;
    }
    AudioConfig hidlConfig;
    CoreUtils::AudioOutputFlags hidlFlags;
    Result retval = Result::NOT_INITIALIZED;
}
