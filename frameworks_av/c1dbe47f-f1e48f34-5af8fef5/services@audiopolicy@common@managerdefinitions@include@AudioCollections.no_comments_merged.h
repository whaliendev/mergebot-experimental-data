       
#include <utils/String8.h>
#include <utils/Vector.h>
#include <utils/RefBase.h>
#include <utils/Errors.h>
#include <system/audio.h>
#include <cutils/config_utils.h>
namespace android {
class PolicyAudioPort;
class AudioRoute;
using PolicyAudioPortVector = Vector<sp<PolicyAudioPort>>;
using AudioRouteVector = Vector<sp<AudioRoute>>;
sp<PolicyAudioPort> findByTagName(const PolicyAudioPortVector& policyAudioPortVector,
                                  const std::string &tagName);
void dumpAudioRouteVector(const AudioRouteVector& audioRouteVector, String8 *dst, int spaces);
}
