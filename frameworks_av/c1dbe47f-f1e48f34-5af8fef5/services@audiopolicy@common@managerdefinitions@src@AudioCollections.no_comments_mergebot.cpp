#define LOG_TAG "APM::AudioCollections"
#include "AudioCollections.h"
#include "AudioPort.h"
#include "AudioRoute.h"
#include "HwModule.h"
#include "PolicyAudioPort.h"
namespace android {
sp<PolicyAudioPort> findByTagName(
    const PolicyAudioPortVector& policyAudioPortVector,
    const std::string& tagName) {
  for (const auto& port : policyAudioPortVector) {
    if (port->getTagName() == tagName) {
      return port;
    }
  }
  return nullptr;
}
void dumpAudioRouteVector(const AudioRouteVector& audioRouteVector,
                          String8* dst, int spaces) {
  if (audioRouteVector.isEmpty()) {
    return;
  }
  dst->appendFormat("\n%*sAudio Routes (%zu):\n", spaces, "",
                    audioRouteVector.size());
  for (size_t i = 0; i < audioRouteVector.size(); i++) {
    dst->appendFormat("%*s- Route %zu:\n", spaces, "", i + 1);
    audioRouteVector.itemAt(i)->dump(dst, 4);
  }
}
}
