#define LOG_TAG "AudioFlinger"
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
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) \
  do { \
  } while (0)
#endif
#define max(a,b) ((a) > (b) ? (a) : (b))
template <typename T>
static inline T min(const T& a, const T& b) {
  return a < b ? a : b;
}
