#define LOG_TAG "MediaResource"
#include <utils/Log.h>
#include <media/MediaResource.h>
#include <vector>
namespace android {
MediaResource::MediaResource(Type type, int64_t value) {
    this->type = type;
    this->subType = SubType::kUnspecifiedSubType;
    this->value = value;
}
MediaResource::MediaResource(Type type, SubType subType, int64_t value) {
    this->type = type;
    this->subType = subType;
    this->value = value;
}
MediaResource::MediaResource(Type type, const std::vector<int8_t> &id, int64_t value) {
    this->type = type;
    this->subType = SubType::kUnspecifiedSubType;
    this->id = id;
    this->value = value;
}
MediaResource MediaResource::CodecResource(bool secure, bool video) {
    return MediaResource(
            secure ? Type::kSecureCodec : Type::kNonSecureCodec,
            video ? SubType::kVideoCodec : SubType::kAudioCodec,
            1);
}
MediaResource MediaResource::GraphicMemoryResource(int64_t value) {
    return MediaResource(Type::kGraphicMemory, value);
}
MediaResource MediaResource::CpuBoostResource() {
    return MediaResource(Type::kCpuBoost, 1);
}
MediaResource MediaResource::VideoBatteryResource() {
    return MediaResource(Type::kBattery, SubType::kVideoCodec, 1);
}
MediaResource MediaResource::DrmSessionResource(const std::vector<int8_t> &id, int64_t value) {
    return MediaResource(Type::kDrmSession, id, value);
}
static String8 bytesToHexString(const std::vector<int8_t> &bytes) {
    String8 str;
    for (auto &b : bytes) {
        str.appendFormat("%02x", b);
    }
    return str;
}
String8 toString(const MediaResourceParcel& resource) {
    String8 str;
    str.appendFormat("%s/%s:[%s]:%lld",
            asString(resource.type), asString(resource.subType),
            bytesToHexString(resource.id).c_str(),
            (long long)resource.value);
    return str;
}
};
