/*
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaResource"
#include <utils/Log.h>
#include <media/MediaResource.h>

#include <vector>

namespace android {

<<<<<<< HEAD
MediaResource::MediaResource(Type type, int64_t value) {
    this->type = type;
    this->subType = SubType::kUnspecifiedSubType;
    this->value = value;
||||||| 35b28e5f80
MediaResource::MediaResource()
        : mType(kUnspecified),
          mSubType(kUnspecifiedSubType),
          mValue(0) {}

MediaResource::MediaResource(Type type, uint64_t value)
        : mType(type),
          mSubType(kUnspecifiedSubType),
          mValue(value) {}

MediaResource::MediaResource(Type type, SubType subType, uint64_t value)
        : mType(type),
          mSubType(subType),
          mValue(value) {}

void MediaResource::readFromParcel(const Parcel &parcel) {
    mType = static_cast<Type>(parcel.readInt32());
    mSubType = static_cast<SubType>(parcel.readInt32());
    mValue = parcel.readUint64();
=======
MediaResource::MediaResource()
        : mType(kUnspecified),
          mSubType(kUnspecifiedSubType),
          mValue(0) {}

MediaResource::MediaResource(Type type, uint64_t value)
        : mType(type),
          mSubType(kUnspecifiedSubType),
          mValue(value) {}

MediaResource::MediaResource(Type type, SubType subType, uint64_t value)
        : mType(type),
          mSubType(subType),
          mValue(value) {}

MediaResource::MediaResource(Type type, const std::vector<uint8_t> &id, uint64_t value)
        : mType(type),
          mSubType(kUnspecifiedSubType),
          mValue(value),
          mId(id) {}

void MediaResource::readFromParcel(const Parcel &parcel) {
    mType = static_cast<Type>(parcel.readInt32());
    mSubType = static_cast<SubType>(parcel.readInt32());
    mValue = parcel.readUint64();
    parcel.readByteVector(&mId);
>>>>>>> 77b8c802
}

<<<<<<< HEAD
MediaResource::MediaResource(Type type, SubType subType, int64_t value) {
    this->type = type;
    this->subType = subType;
    this->value = value;
||||||| 35b28e5f80
void MediaResource::writeToParcel(Parcel *parcel) const {
    parcel->writeInt32(static_cast<int32_t>(mType));
    parcel->writeInt32(static_cast<int32_t>(mSubType));
    parcel->writeUint64(mValue);
=======
void MediaResource::writeToParcel(Parcel *parcel) const {
    parcel->writeInt32(static_cast<int32_t>(mType));
    parcel->writeInt32(static_cast<int32_t>(mSubType));
    parcel->writeUint64(mValue);
    parcel->writeByteVector(mId);
}

static String8 bytesToHexString(const std::vector<uint8_t> &bytes) {
    String8 str;
    for (auto &b : bytes) {
        str.appendFormat("%02x", b);
    }
    return str;
>>>>>>> 77b8c802
}

MediaResource::MediaResource(Type type, const std::vector<int8_t> &id, int64_t value) {
    this->type = type;
    this->subType = SubType::kUnspecifiedSubType;
    this->id = id;
    this->value = value;
}

//static
MediaResource MediaResource::CodecResource(bool secure, bool video) {
    return MediaResource(
            secure ? Type::kSecureCodec : Type::kNonSecureCodec,
            video ? SubType::kVideoCodec : SubType::kAudioCodec,
            1);
}

//static
MediaResource MediaResource::GraphicMemoryResource(int64_t value) {
    return MediaResource(Type::kGraphicMemory, value);
}

//static
MediaResource MediaResource::CpuBoostResource() {
    return MediaResource(Type::kCpuBoost, 1);
}

//static
MediaResource MediaResource::VideoBatteryResource() {
    return MediaResource(Type::kBattery, SubType::kVideoCodec, 1);
}

//static
MediaResource MediaResource::DrmSessionResource(const std::vector<int8_t> &id, int64_t value) {
    return MediaResource(Type::kDrmSession, id, value);
}

static String8 bytesToHexString(const std::vector<int8_t> &bytes) {
    String8 str;
<<<<<<< HEAD
    for (auto &b : bytes) {
        str.appendFormat("%02x", b);
    }
||||||| 35b28e5f80
    str.appendFormat("%s/%s:%llu", asString(mType), asString(mSubType), (unsigned long long)mValue);
=======
    str.appendFormat("%s/%s:[%s]:%llu",
        asString(mType), asString(mSubType),
        bytesToHexString(mId).c_str(),
        (unsigned long long)mValue);
>>>>>>> 77b8c802
    return str;
}

<<<<<<< HEAD
String8 toString(const MediaResourceParcel& resource) {
    String8 str;
||||||| 35b28e5f80
bool MediaResource::operator==(const MediaResource &other) const {
    return (other.mType == mType) && (other.mSubType == mSubType) && (other.mValue == mValue);
}
=======
bool MediaResource::operator==(const MediaResource &other) const {
    return (other.mType == mType)
      && (other.mSubType == mSubType)
      && (other.mValue == mValue)
      && (other.mId == mId);
}
>>>>>>> 77b8c802

    str.appendFormat("%s/%s:[%s]:%lld",
            asString(resource.type), asString(resource.subType),
            bytesToHexString(resource.id).c_str(),
            (long long)resource.value);
    return str;
}

}; // namespace android
