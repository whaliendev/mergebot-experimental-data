/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include <iostream>
#include <string>
#include <gtest/gtest.h>
#include <media/AidlConversion.h>
#include <media/AudioCommonTypes.h>
using namespace android;
using namespace android::aidl_utils;
using media::AudioDirectMode;
using media::AudioPortConfigFw;
using media::AudioPortDeviceExtSys;
using media::AudioPortFw;
using media::AudioPortRole;
using media::AudioPortType;
using media::audio::common::AudioChannelLayout;
using media::audio::common::AudioDevice;
using media::audio::common::AudioDeviceAddress;
using media::audio::common::AudioDeviceDescription;
using media::audio::common::AudioDeviceType;
using media::audio::common::AudioEncapsulationMetadataType;
using media::audio::common::AudioEncapsulationType;
using media::audio::common::AudioFormatDescription;
using media::audio::common::AudioFormatType;
using media::audio::common::AudioGain;
using media::audio::common::AudioGainConfig;
using media::audio::common::AudioGainMode;
using media::audio::common::AudioIoFlags;
using media::audio::common::AudioPortDeviceExt;
using media::audio::common::AudioProfile;
using media::audio::common::AudioStandard;
using media::audio::common::ExtraAudioDescriptor;
using media::audio::common::Int;
using media::audio::common::MicrophoneDynamicInfo;
using media::audio::common::MicrophoneInfo;
using media::audio::common::PcmType;
// Provide value printers for types generated from AIDL
// They need to be in the same namespace as the types we intend to print

namespace {

template <typename T>
size_t hash(const T& t) {
    return std::hash<T>{}(t);
}

AudioChannelLayout make_ACL_None() {
    return AudioChannelLayout{};
}

AudioChannelLayout make_ACL_Invalid() {
    return AudioChannelLayout::make<AudioChannelLayout::Tag::invalid>(0);
}

AudioChannelLayout make_ACL_Stereo() {
    return AudioChannelLayout::make<AudioChannelLayout::Tag::layoutMask>(
            AudioChannelLayout::LAYOUT_STEREO);
}

AudioChannelLayout make_ACL_LayoutArbitrary() {
    return AudioChannelLayout::make<AudioChannelLayout::Tag::layoutMask>(
            // Use channels that exist both for input and output,
            // but doesn't form a known layout mask.
            AudioChannelLayout::CHANNEL_FRONT_LEFT | AudioChannelLayout::CHANNEL_FRONT_RIGHT |
            AudioChannelLayout::CHANNEL_TOP_SIDE_LEFT | AudioChannelLayout::CHANNEL_TOP_SIDE_RIGHT);
}

AudioChannelLayout make_ACL_ChannelIndex2() {
    return AudioChannelLayout::make<AudioChannelLayout::Tag::indexMask>(
            AudioChannelLayout::INDEX_MASK_2);
}

AudioChannelLayout make_ACL_ChannelIndexArbitrary() {
    // Use channels 1 and 3.
    return AudioChannelLayout::make<AudioChannelLayout::Tag::indexMask>(5);
}

AudioChannelLayout make_ACL_VoiceCall() {
    return AudioChannelLayout::make<AudioChannelLayout::Tag::voiceMask>(
            AudioChannelLayout::VOICE_CALL_MONO);
}

AudioDeviceDescription make_AudioDeviceDescription(AudioDeviceType type,
                                                   const std::string& connection = "") {
    AudioDeviceDescription result;
    result.type = type;
    result.connection = connection;
    return result;
}

AudioDeviceDescription make_ADD_None() {
    return AudioDeviceDescription{};
}

AudioDeviceDescription make_ADD_DefaultIn() {
    return make_AudioDeviceDescription(AudioDeviceType::IN_DEFAULT);
}

AudioDeviceDescription make_ADD_DefaultOut() {
    return make_AudioDeviceDescription(AudioDeviceType::OUT_DEFAULT);
}

AudioDeviceDescription make_ADD_WiredHeadset() {
    return make_AudioDeviceDescription(AudioDeviceType::OUT_HEADSET,
                                       AudioDeviceDescription::CONNECTION_ANALOG());
}

AudioDeviceDescription make_ADD_BtScoHeadset() {
    return make_AudioDeviceDescription(AudioDeviceType::OUT_HEADSET,
                                       AudioDeviceDescription::CONNECTION_BT_SCO());
}

AudioFormatDescription make_AudioFormatDescription(AudioFormatType type) {
    AudioFormatDescription result;
    result.type = type;
    return result;
}

AudioFormatDescription make_AudioFormatDescription(PcmType pcm) {
    auto result = make_AudioFormatDescription(AudioFormatType::PCM);
    result.pcm = pcm;
    return result;
}

AudioFormatDescription make_AudioFormatDescription(const std::string& encoding) {
    AudioFormatDescription result;
    result.encoding = encoding;
    return result;
}

AudioFormatDescription make_AudioFormatDescription(PcmType transport, const std::string& encoding) {
    auto result = make_AudioFormatDescription(encoding);
    result.pcm = transport;
    return result;
}

AudioFormatDescription make_AFD_Default() {
    return AudioFormatDescription{};
}

AudioFormatDescription make_AFD_Invalid() {
    return make_AudioFormatDescription(AudioFormatType::SYS_RESERVED_INVALID);
}

AudioFormatDescription make_AFD_Pcm16Bit() {
    return make_AudioFormatDescription(PcmType::INT_16_BIT);
}

AudioFormatDescription make_AFD_Bitstream() {
    return make_AudioFormatDescription("example");
}

AudioFormatDescription make_AFD_Encap() {
    return make_AudioFormatDescription(PcmType::INT_16_BIT, "example.encap");
}

AudioFormatDescription make_AFD_Encap_with_Enc() {
    auto afd = make_AFD_Encap();
    afd.encoding += "+example";
    return afd;
}

}  // namespace

class HashIdentityTest : public ::testing::Test {
  public:
    template <typename T>
    void verifyHashIdentity(const std::vector<std::function<T()>>& valueGens) {
        for (size_t i = 0; i < valueGens.size(); ++i) {
            for (size_t j = 0; j < valueGens.size(); ++j) {
                if (i == j) {
                    EXPECT_EQ(hash(valueGens[i]()), hash(valueGens[i]())) << i;
                } else {
                    EXPECT_NE(hash(valueGens[i]()), hash(valueGens[j]())) << i << ", " << j;
                }
            }
        }
    }
};

TEST_F(HashIdentityTest, AudioFormatDescriptionHashIdentity) {
    verifyHashIdentity<AudioFormatDescription>({make_AFD_Default, make_AFD_Invalid,
                                                make_AFD_Pcm16Bit, make_AFD_Bitstream,
                                                make_AFD_Encap, make_AFD_Encap_with_Enc});
}

TEST_F(HashIdentityTest, AudioFormatDescriptionHashIdentity) {
    verifyHashIdentity<AudioFormatDescription>({make_AFD_Default, make_AFD_Invalid,
                                                make_AFD_Pcm16Bit, make_AFD_Bitstream,
                                                make_AFD_Encap, make_AFD_Encap_with_Enc});
}

TEST_F(HashIdentityTest, AudioFormatDescriptionHashIdentity) {
    verifyHashIdentity<AudioFormatDescription>({make_AFD_Default, make_AFD_Invalid,
                                                make_AFD_Pcm16Bit, make_AFD_Bitstream,
                                                make_AFD_Encap, make_AFD_Encap_with_Enc});
}

using ChannelLayoutParam = std::tuple<AudioChannelLayout, bool /*isInput*/>;
class AudioChannelLayoutRoundTripTest : public testing::TestWithParam<ChannelLayoutParam> {};

class AudioGainTest : public testing::TestWithParam<bool> {};

TEST_P(AudioFormatDescriptionRoundTripTest, Aidl2Legacy2Aidl) {
    const auto initial = GetParam();
    auto conv = aidl2legacy_AudioFormatDescription_audio_format_t(initial);
    ASSERT_TRUE(conv.ok());
    auto convBack = legacy2aidl_audio_format_t_AudioFormatDescription(conv.value());
    ASSERT_TRUE(convBack.ok());
    EXPECT_EQ(initial, convBack.value());
}
TEST(AudioMicrophoneInfoFw, ChannelMapping) {
    audio_microphone_characteristic_t mic{};
    mic.channel_mapping[1] = AUDIO_MICROPHONE_CHANNEL_MAPPING_DIRECT;
    mic.channel_mapping[3] = AUDIO_MICROPHONE_CHANNEL_MAPPING_PROCESSED;
    auto conv = legacy2aidl_audio_microphone_characteristic_t_MicrophoneInfoFw(mic);
    ASSERT_TRUE(conv.ok());
    const auto& aidl = conv.value();
    EXPECT_EQ(4, aidl.dynamic.channelMapping.size());
    EXPECT_EQ(MicrophoneDynamicInfo::ChannelMapping::UNUSED, aidl.dynamic.channelMapping[0]);
    EXPECT_EQ(MicrophoneDynamicInfo::ChannelMapping::DIRECT, aidl.dynamic.channelMapping[1]);
    EXPECT_EQ(MicrophoneDynamicInfo::ChannelMapping::UNUSED, aidl.dynamic.channelMapping[2]);
    EXPECT_EQ(MicrophoneDynamicInfo::ChannelMapping::PROCESSED, aidl.dynamic.channelMapping[3]);
}
class AudioGainModeRoundTripTest : public testing::TestWithParam<AudioGainMode> {};

class AudioEncapsulationMetadataTypeRoundTripTest
    : public testing::TestWithParam<AudioEncapsulationMetadataType> {};

class AudioStandardRoundTripTest : public testing::TestWithParam<AudioStandard> {};

class AudioDirectModeRoundTripTest : public testing::TestWithParam<AudioDirectMode> {};

class AudioPortFwRoundTripTest : public testing::TestWithParam<AudioDeviceDescription> {
  public:
    AudioProfile createProfile(const AudioFormatDescription& format,
                               const std::vector<AudioChannelLayout>& channelMasks,
                               const std::vector<int32_t>& sampleRates) {
        AudioProfile profile;
        profile.format = format;
        profile.channelMasks = channelMasks;
        profile.sampleRates = sampleRates;
        return profile;
    }
};

AudioPortConfigFw createAudioPortConfigFw(const AudioChannelLayout& layout,
                                          const AudioFormatDescription& format,
                                          const AudioDeviceDescription& device) {
    const bool isInput = device.type < AudioDeviceType::OUT_DEFAULT;
    AudioPortConfigFw result;
    result.hal.id = 43;
    result.hal.portId = 42;
    Int sr44100;
    sr44100.value = 44100;
    result.hal.sampleRate = sr44100;
    result.hal.channelMask = layout;
    result.hal.format = format;
    AudioGainConfig gain;
    gain.mode = 1 << static_cast<int>(AudioGainMode::JOINT);
    gain.values = std::vector<int32_t>({100});
    result.hal.gain = gain;
    AudioPortDeviceExt ext;
    AudioDevice audioDevice;
    audioDevice.type = device;
    ext.device = audioDevice;
    result.hal.ext = ext;
    result.sys.role = isInput ? AudioPortRole::SOURCE : AudioPortRole::SINK;
    result.sys.type = AudioPortType::DEVICE;
    AudioPortDeviceExtSys sysDevice;
    sysDevice.hwModule = 1;
    result.sys.ext = sysDevice;
    return result;
}

using AudioPortConfigParam =
        std::tuple<AudioChannelLayout, AudioFormatDescription, AudioDeviceDescription>;
class AudioPortConfigRoundTripTest : public testing::TestWithParam<AudioPortConfigParam> {};

class AudioDeviceRoundTripTest : public testing::TestWithParam<AudioDevice> {};

using ChannelLayoutEdgeCaseParam = std::tuple<int /*legacy*/, bool /*isInput*/, bool /*isValid*/>;
class AudioChannelLayoutEdgeCaseTest : public testing::TestWithParam<ChannelLayoutEdgeCaseParam> {};

TEST_P(AudioFormatDescriptionRoundTripTest, Aidl2Legacy2Aidl) {
    const auto initial = GetParam();
    auto conv = aidl2legacy_AudioFormatDescription_audio_format_t(initial);
    ASSERT_TRUE(conv.ok());
    auto convBack = legacy2aidl_audio_format_t_AudioFormatDescription(conv.value());
    ASSERT_TRUE(convBack.ok());
    EXPECT_EQ(initial, convBack.value());
}
class AudioDeviceDescriptionRoundTripTest : public testing::TestWithParam<AudioDeviceDescription> {
};

TEST_P(AudioFormatDescriptionRoundTripTest, Aidl2Legacy2Aidl) {
    const auto initial = GetParam();
    auto conv = aidl2legacy_AudioFormatDescription_audio_format_t(initial);
    ASSERT_TRUE(conv.ok());
    auto convBack = legacy2aidl_audio_format_t_AudioFormatDescription(conv.value());
    ASSERT_TRUE(convBack.ok());
    EXPECT_EQ(initial, convBack.value());
}
class AudioFormatDescriptionRoundTripTest : public testing::TestWithParam<AudioFormatDescription> {
};

TEST_P(AudioFormatDescriptionRoundTripTest, Aidl2Legacy2Aidl) {
    const auto initial = GetParam();
    auto conv = aidl2legacy_AudioFormatDescription_audio_format_t(initial);
    ASSERT_TRUE(conv.ok());
    auto convBack = legacy2aidl_audio_format_t_AudioFormatDescription(conv.value());
    ASSERT_TRUE(convBack.ok());
    EXPECT_EQ(initial, convBack.value());
}
// Provide value printers for types generated from AIDL
// They need to be in the same namespace as the types we intend to print
namespace android::media {
namespace audio::common {}  // namespace audio::common

}  // namespace android::media

namespace {

template <typename T>
size_t hash(const T& t) {
    return std::hash<T>{}(t);
}

AudioChannelLayout make_ACL_None() {
    return AudioChannelLayout{};
}

AudioChannelLayout make_ACL_Invalid() {
    return AudioChannelLayout::make<AudioChannelLayout::Tag::invalid>(0);
}

AudioChannelLayout make_ACL_Stereo() {
    return AudioChannelLayout::make<AudioChannelLayout::Tag::layoutMask>(
            AudioChannelLayout::LAYOUT_STEREO);
}

AudioChannelLayout make_ACL_LayoutArbitrary() {
    return AudioChannelLayout::make<AudioChannelLayout::Tag::layoutMask>(
            // Use channels that exist both for input and output,
            // but doesn't form a known layout mask.
            AudioChannelLayout::CHANNEL_FRONT_LEFT | AudioChannelLayout::CHANNEL_FRONT_RIGHT |
            AudioChannelLayout::CHANNEL_TOP_SIDE_LEFT | AudioChannelLayout::CHANNEL_TOP_SIDE_RIGHT);
}

AudioChannelLayout make_ACL_ChannelIndex2() {
    return AudioChannelLayout::make<AudioChannelLayout::Tag::indexMask>(
            AudioChannelLayout::INDEX_MASK_2);
}

AudioChannelLayout make_ACL_ChannelIndexArbitrary() {
    // Use channels 1 and 3.
    return AudioChannelLayout::make<AudioChannelLayout::Tag::indexMask>(5);
}

AudioChannelLayout make_ACL_VoiceCall() {
    return AudioChannelLayout::make<AudioChannelLayout::Tag::voiceMask>(
            AudioChannelLayout::VOICE_CALL_MONO);
}

AudioDeviceDescription make_AudioDeviceDescription(AudioDeviceType type,
                                                   const std::string& connection = "") {
    AudioDeviceDescription result;
    result.type = type;
    result.connection = connection;
    return result;
}

AudioDeviceDescription make_ADD_None() {
    return AudioDeviceDescription{};
}

AudioDeviceDescription make_ADD_DefaultIn() {
    return make_AudioDeviceDescription(AudioDeviceType::IN_DEFAULT);
}

AudioDeviceDescription make_ADD_MicIn() {
    return make_AudioDeviceDescription(AudioDeviceType::IN_MICROPHONE);
}

AudioDeviceDescription make_ADD_RSubmixIn() {
    return make_AudioDeviceDescription(AudioDeviceType::IN_SUBMIX);
}

AudioDeviceDescription make_ADD_DefaultOut() {
    return make_AudioDeviceDescription(AudioDeviceType::OUT_DEFAULT);
}

AudioDeviceDescription make_ADD_WiredHeadset() {
    return make_AudioDeviceDescription(AudioDeviceType::OUT_HEADSET,
                                       AudioDeviceDescription::CONNECTION_ANALOG());
}

AudioDeviceDescription make_ADD_BtScoHeadset() {
    return make_AudioDeviceDescription(AudioDeviceType::OUT_HEADSET,
                                       AudioDeviceDescription::CONNECTION_BT_SCO());
}

AudioDeviceDescription make_ADD_BtA2dpHeadphone() {
    return make_AudioDeviceDescription(AudioDeviceType::OUT_HEADPHONE,
                                       AudioDeviceDescription::CONNECTION_BT_A2DP());
}

AudioDeviceDescription make_ADD_BtLeHeadset() {
    return make_AudioDeviceDescription(AudioDeviceType::OUT_HEADSET,
                                       AudioDeviceDescription::CONNECTION_BT_LE());
}

AudioDeviceDescription make_ADD_BtLeBroadcast() {
    return make_AudioDeviceDescription(AudioDeviceType::OUT_BROADCAST,
                                       AudioDeviceDescription::CONNECTION_BT_LE());
}

AudioDeviceDescription make_ADD_IpV4Device() {
    return make_AudioDeviceDescription(AudioDeviceType::OUT_DEVICE,
                                       AudioDeviceDescription::CONNECTION_IP_V4());
}

AudioDeviceDescription make_ADD_UsbHeadset() {
    return make_AudioDeviceDescription(AudioDeviceType::OUT_HEADSET,
                                       AudioDeviceDescription::CONNECTION_USB());
}

AudioDevice make_AudioDevice(const AudioDeviceDescription& type,
                             const AudioDeviceAddress& address) {
    AudioDevice result;
    result.type = type;
    result.address = address;
    return result;
}

AudioFormatDescription make_AudioFormatDescription(AudioFormatType type) {
    AudioFormatDescription result;
    result.type = type;
    return result;
}

AudioFormatDescription make_AudioFormatDescription(PcmType pcm) {
    auto result = make_AudioFormatDescription(AudioFormatType::PCM);
    result.pcm = pcm;
    return result;
}

AudioFormatDescription make_AudioFormatDescription(const std::string& encoding) {
    AudioFormatDescription result;
    result.encoding = encoding;
    return result;
}

AudioFormatDescription make_AudioFormatDescription(PcmType transport, const std::string& encoding) {
    auto result = make_AudioFormatDescription(encoding);
    result.pcm = transport;
    return result;
}

AudioFormatDescription make_AFD_Default() {
    return AudioFormatDescription{};
}

AudioFormatDescription make_AFD_Invalid() {
    return make_AudioFormatDescription(AudioFormatType::SYS_RESERVED_INVALID);
}

AudioFormatDescription make_AFD_Pcm16Bit() {
    return make_AudioFormatDescription(PcmType::INT_16_BIT);
}

AudioFormatDescription make_AFD_Bitstream() {
    return make_AudioFormatDescription("example");
}

AudioFormatDescription make_AFD_Encap() {
    return make_AudioFormatDescription(PcmType::INT_16_BIT, "example.encap");
}

AudioFormatDescription make_AFD_Encap_with_Enc() {
    auto afd = make_AFD_Encap();
    afd.encoding += "+example";
    return afd;
}

android::media::TrackSecondaryOutputInfo make_TrackSecondaryOutputInfo() {
    android::media::TrackSecondaryOutputInfo result;
    result.portId = 1;
    result.secondaryOutputIds = {0, 5, 7};
    return result;
}

ExtraAudioDescriptor make_ExtraAudioDescriptor(AudioStandard audioStandard,
                                               AudioEncapsulationType audioEncapsulationType) {
    ExtraAudioDescriptor result;
    result.standard = audioStandard;
    result.audioDescriptor = {0xb4, 0xaf, 0x98, 0x1a};
    result.encapsulationType = audioEncapsulationType;
    return result;
}

}  // namespace

using ExtraAudioDescriptorParam = std::tuple<AudioStandard, AudioEncapsulationType>;
class ExtraAudioDescriptorRoundTripTest : public testing::TestWithParam<ExtraAudioDescriptorParam> {
};