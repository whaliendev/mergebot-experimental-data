/*
**
** Copyright 2012, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "AudioFlinger"
//#define LOG_NDEBUG 0

#define ATRACE_TAG ATRACE_TAG_AUDIO
#include "Configuration.h"
#include <math.h>
#include <fcntl.h>
#include <memory>
#include <string>
#include <linux/futex.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <cutils/properties.h>
#include <media/AudioContainers.h>
#include <media/AudioDeviceTypeAddr.h>
#include <media/AudioParameter.h>
#include <media/AudioResamplerPublic.h>
#include <media/RecordBufferConverter.h>
#include <media/TypeConverter.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <private/media/AudioTrackShared.h>
#include <private/android_filesystem_config.h>
#include <audio_utils/Balance.h>
#include <audio_utils/channels.h>
#include <audio_utils/mono_blend.h>
#include <audio_utils/primitives.h>
#include <audio_utils/format.h>
#include <audio_utils/minifloat.h>
#include <audio_utils/safe_math.h>
#include <system/audio_effects/effect_ns.h>
#include <system/audio_effects/effect_aec.h>
#include <system/audio.h>
// NBAIO implementations

#include <media/nbaio/AudioStreamInSource.h>
#include <media/nbaio/AudioStreamOutSink.h>
#include <media/nbaio/MonoPipe.h>
#include <media/nbaio/MonoPipeReader.h>
#include <media/nbaio/Pipe.h>
#include <media/nbaio/PipeReader.h>
#include <media/nbaio/SourceAudioBufferProvider.h>
#include <mediautils/BatteryNotifier.h>
#include <audiomanager/AudioManager.h>
#include <powermanager/PowerManager.h>
#include <media/audiohal/EffectsFactoryHalInterface.h>
#include <media/audiohal/StreamHalInterface.h>
#include "AudioFlinger.h"
#include "FastMixer.h"
#include "FastCapture.h"
#include <mediautils/SchedulingPolicyService.h>
#include <mediautils/ServiceUtilities.h>
#ifdef ADD_BATTERY_DATA
#include <media/IMediaPlayerService.h>
#include <media/IMediaDeathNotifier.h>
#endif
#ifdef DEBUG_CPU_USAGE
#include <audio_utils/Statistics.h>
#include <cpustats/ThreadCpuUsage.h>
#endif
#include "AutoPark.h"
#include <pthread.h>
#include "TypedLogger.h"
// ----------------------------------------------------------------------------
// Note: the following macro is used for extremely verbose logging message.  In
// order to run with ALOG_ASSERT turned on, we need to have LOG_NDEBUG set to
// 0; but one side effect of this is to turn all LOGV's as well.  Some messages
// are so verbose that we want to suppress them even when we have ALOG_ASSERT
// turned on.  Do not uncomment the #def below unless you really know what you
// are doing and want to see all of the extremely verbose messages.
//#define VERY_VERY_VERBOSE_LOGGING

#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) \
  do {               \
  } while (0)
#endif
// TODO: Move these macro/inlines to a header file.

#define max(a, b) ((a) > (b) ? (a) : (b))

template <typename T>
static inline T min(const T& a, const T& b) {
  return a < b ? a : b;
}

// namespace android