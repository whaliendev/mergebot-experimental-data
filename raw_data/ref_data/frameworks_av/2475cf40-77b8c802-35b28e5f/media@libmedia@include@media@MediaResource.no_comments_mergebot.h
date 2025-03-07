#ifndef ANDROID_MEDIA_RESOURCE_H
#define ANDROID_MEDIA_RESOURCE_H 
#include <binder/Parcel.h>
#include <aidl/android/media/MediaResourceParcel.h>
#include <utils/String8.h>
#include <vector>
namespace android {
class MediaResource : public MediaResourceParcel {
 public:
  using Type = MediaResourceType;
  using SubType = MediaResourceSubType;
  MediaResource(Type type, int64_t value);
  MediaResource(Type type, SubType subType, int64_t value);
  MediaResource(Type type, const std::vector<uint8_t> &id, uint64_t value);
  std::vector<uint8_t> mId;
  MediaResource(Type type, const std::vector<int8_t> &id, int64_t value);
  static MediaResource CodecResource(bool secure, bool video);
  static MediaResource GraphicMemoryResource(int64_t value);
  static MediaResource CpuBoostResource();
  static MediaResource VideoBatteryResource();
  static MediaResource DrmSessionResource(const std::vector<int8_t> &id,
                                          int64_t value);
};
inline static const char *asString(MediaResource::Type i,
                                   const char *def = "??") {
  switch (i) {
<<<<<<< HEAD
    case MediaResource::Type::kUnspecified:
      return "unspecified";
    case MediaResource::Type::kSecureCodec:
      return "secure-codec";
    case MediaResource::Type::kNonSecureCodec:
      return "non-secure-codec";
    case MediaResource::Type::kGraphicMemory:
      return "graphic-memory";
    case MediaResource::Type::kCpuBoost:
      return "cpu-boost";
    case MediaResource::Type::kBattery:
      return "battery";
    case MediaResource::Type::kDrmSession:
      return "drm-session";
||||||| 35b28e5f80
    case MediaResource::kUnspecified:
      return "unspecified";
    case MediaResource::kSecureCodec:
      return "secure-codec";
    case MediaResource::kNonSecureCodec:
      return "non-secure-codec";
    case MediaResource::kGraphicMemory:
      return "graphic-memory";
    case MediaResource::kCpuBoost:
      return "cpu-boost";
    case MediaResource::kBattery:
      return "battery";
=======
    case MediaResource::kUnspecified:
      return "unspecified";
    case MediaResource::kSecureCodec:
      return "secure-codec";
    case MediaResource::kNonSecureCodec:
      return "non-secure-codec";
    case MediaResource::kGraphicMemory:
      return "graphic-memory";
    case MediaResource::kCpuBoost:
      return "cpu-boost";
    case MediaResource::kBattery:
      return "battery";
    case MediaResource::kDrmSession:
      return "drm-session";
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
    default:
      return def;
  }
}
inline static const char *asString(MediaResource::SubType i,
                                   const char *def = "??") {
  switch (i) {
    case MediaResource::SubType::kUnspecifiedSubType:
      return "unspecified";
    case MediaResource::SubType::kAudioCodec:
      return "audio-codec";
    case MediaResource::SubType::kVideoCodec:
      return "video-codec";
    default:
      return def;
  }
}
using aidl::android::media::MediaResourceSubType;
using aidl::android::media::MediaResourceType;
String8 toString(const MediaResourceParcel &resource);
}
#endif
