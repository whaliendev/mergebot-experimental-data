/*
 * Copyright (C) 2022 The Android Open Source Project
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

#define LOG_TAG "DeviceHalAidl"
// #define LOG_NDEBUG 0

#include <algorithm>
#include <forward_list>
#include <aidl/android/hardware/audio/core/BnStreamCallback.h>
#include <aidl/android/hardware/audio/core/BnStreamOutEventCallback.h>
#include <aidl/android/hardware/audio/core/StreamDescriptor.h>
#include <error/expected_utils.h>
#include <media/AidlConversionCppNdk.h>
#include <media/AidlConversionNdkCpp.h>
#include <media/AidlConversionUtil.h>
#include <mediautils/TimeCheck.h>
#include <Utils.h>
#include <utils/Log.h>
#include "DeviceHalAidl.h"
#include "StreamHalAidl.h"
using aidl::android::aidl_utils::statusTFromBinderStatus;
using aidl::android::media::audio::common::AudioChannelLayout;
using aidl::android::media::audio::common::AudioConfig;
using aidl::android::media::audio::common::AudioDevice;
using aidl::android::media::audio::common::AudioDeviceAddress;
using aidl::android::media::audio::common::AudioDeviceType;
using aidl::android::media::audio::common::AudioFormatType;
using aidl::android::media::audio::common::AudioInputFlags;
using aidl::android::media::audio::common::AudioIoFlags;
using aidl::android::media::audio::common::AudioLatencyMode;
using aidl::android::media::audio::common::AudioMMapPolicy;
using aidl::android::media::audio::common::AudioMMapPolicyInfo;
using aidl::android::media::audio::common::AudioMMapPolicyType;
using aidl::android::media::audio::common::AudioMode;
using aidl::android::media::audio::common::AudioOutputFlags;
using aidl::android::media::audio::common::AudioPort;
using aidl::android::media::audio::common::AudioPortConfig;
using aidl::android::media::audio::common::AudioPortDeviceExt;
using aidl::android::media::audio::common::AudioPortExt;
using aidl::android::media::audio::common::AudioPortMixExt;
using aidl::android::media::audio::common::AudioPortMixExtUseCase;
using aidl::android::media::audio::common::AudioProfile;
using aidl::android::media::audio::common::AudioSource;
using aidl::android::media::audio::common::Float;
using aidl::android::media::audio::common::Int;
using aidl::android::media::audio::common::MicrophoneDynamicInfo;
using aidl::android::media::audio::common::MicrophoneInfo;
using aidl::android::hardware::audio::common::getFrameSizeInBytes;
using aidl::android::hardware::audio::common::isBitPositionFlagSet;
using aidl::android::hardware::audio::common::isDefaultAudioFormat;
using aidl::android::hardware::audio::common::makeBitPositionFlagMask;
using aidl::android::hardware::audio::common::RecordTrackMetadata;
using aidl::android::hardware::audio::core::AudioPatch;
using aidl::android::hardware::audio::core::AudioRoute;
using aidl::android::hardware::audio::core::IModule;
using aidl::android::hardware::audio::core::ITelephony;
using aidl::android::hardware::audio::core::ModuleDebug;
using aidl::android::hardware::audio::core::StreamDescriptor;

namespace android {

namespace {

bool isConfigEqualToPortConfig(const AudioConfig& config, const AudioPortConfig& portConfig) {
    return portConfig.sampleRate.value().value == config.base.sampleRate &&
            portConfig.channelMask.value() == config.base.channelMask &&
            portConfig.format.value() == config.base.format;
}

void setConfigFromPortConfig(AudioConfig* config, const AudioPortConfig& portConfig) {
    config->base.sampleRate = portConfig.sampleRate.value().value;
    config->base.channelMask = portConfig.channelMask.value();
    config->base.format = portConfig.format.value();
}

void setPortConfigFromConfig(AudioPortConfig* portConfig, const AudioConfig& config) {
    portConfig->sampleRate = Int{ .value = config.base.sampleRate };
    portConfig->channelMask = config.base.channelMask;
    portConfig->format = config.base.format;
}

// Note: these converters are for types defined in different AIDL files. Although these
// AIDL files are copies of each other, however formally these are different types
// thus we don't use a conversion via a parcelable.
ConversionResult<media::AudioRoute> ndk2cpp_AudioRoute(const AudioRoute& ndk) {
    media::AudioRoute cpp;
    cpp.sourcePortIds.insert(
            cpp.sourcePortIds.end(), ndk.sourcePortIds.begin(), ndk.sourcePortIds.end());
    cpp.sinkPortId = ndk.sinkPortId;
    cpp.isExclusive = ndk.isExclusive;
    return cpp;
}

} // namespace

namespace {

bool isConfigEqualToPortConfig(const AudioConfig& config, const AudioPortConfig& portConfig) {
    return portConfig.sampleRate.value().value == config.base.sampleRate &&
            portConfig.channelMask.value() == config.base.channelMask &&
            portConfig.format.value() == config.base.format;
}

void setConfigFromPortConfig(AudioConfig* config, const AudioPortConfig& portConfig) {
    config->base.sampleRate = portConfig.sampleRate.value().value;
    config->base.channelMask = portConfig.channelMask.value();
    config->base.format = portConfig.format.value();
}

void setPortConfigFromConfig(AudioPortConfig* portConfig, const AudioConfig& config) {
    portConfig->sampleRate = Int{ .value = config.base.sampleRate };
    portConfig->channelMask = config.base.channelMask;
    portConfig->format = config.base.format;
}

// Note: these converters are for types defined in different AIDL files. Although these
// AIDL files are copies of each other, however formally these are different types
// thus we don't use a conversion via a parcelable.
ConversionResult<media::AudioRoute> ndk2cpp_AudioRoute(const AudioRoute& ndk) {
    media::AudioRoute cpp;
    cpp.sourcePortIds.insert(
            cpp.sourcePortIds.end(), ndk.sourcePortIds.begin(), ndk.sourcePortIds.end());
    cpp.sinkPortId = ndk.sinkPortId;
    cpp.isExclusive = ndk.isExclusive;
    return cpp;
}

} // namespace

status_t DeviceHalAidl::getAudioPorts(std::vector<media::audio::common::AudioPort> *ports) {
    auto convertAudioPortFromMap = [](const Ports::value_type& pair) {
        return ndk2cpp_AudioPort(pair.second);
    };
    return ::aidl::android::convertRange(mPorts.begin(), mPorts.end(), ports->begin(),
            convertAudioPortFromMap);
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

namespace {

class Cleanup {
public:
    typedef void (DeviceHalAidl::*Cleaner)(int32_t);
    
    Cleanup(DeviceHalAidl* device, Cleaner cleaner, int32_t id): mDevice(device), mCleaner(cleaner), mId(id) {}
    ~Cleanup(){ clean(); }
    void clean() {
        if (mDevice != nullptr) (mDevice->*mCleaner)(mId);
        disarm();
    }
    void disarm() { mDevice = nullptr; }
    
private:
    DeviceHalAidl* mDevice;
    const Cleaner mCleaner;
    const int32_t mId;
};

} // namespace

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

namespace {

class Cleanup {
public:
    typedef void (DeviceHalAidl::*Cleaner)(int32_t);
    
    Cleanup(DeviceHalAidl* device, Cleaner cleaner, int32_t id): mDevice(device), mCleaner(cleaner), mId(id) {}
    ~Cleanup(){ clean(); }
    void clean() {
        if (mDevice != nullptr) (mDevice->*mCleaner)(mId);
        disarm();
    }
    void disarm() { mDevice = nullptr; }
    
private:
    DeviceHalAidl* mDevice;
    const Cleaner mCleaner;
    const int32_t mId;
};

} // namespace

class DeviceHalAidl::Cleanups : public std::forward_list<Cleanup> {
public:
    ~Cleanups() { for (auto& c : *this) c.clean(); }
template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

};

status_t DeviceHalAidl::getInputBufferSize(const struct audio_config* config, size_t* size) {
    ALOGD("%p %s::%s", this, getClassName().c_str(), __func__);
    if (size == nullptr) return BAD_VALUE;
    TIME_CHECK();
    if (!mModule) return NO_INIT;
    AudioConfig aidlConfig = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_config_t_AudioConfig(*config, true /*isInput*/));
    AudioDevice aidlDevice;
    aidlDevice.type.type = AudioDeviceType::IN_DEFAULT;
    AudioSource aidlSource = AudioSource::DEFAULT;
    AudioIoFlags aidlFlags = AudioIoFlags::make<AudioIoFlags::Tag::input>(0);
    AudioPortConfig mixPortConfig;
    Cleanups cleanups;
    audio_config writableConfig = *config;
    AudioPatch aidlPatch;
    RETURN_STATUS_IF_ERROR(prepareToOpenStream(0 /*handle*/, aidlDevice, aidlFlags, aidlSource,
                    &writableConfig, &cleanups, &aidlConfig, &mixPortConfig, &aidlPatch));
    *size = aidlConfig.frameCount *
            getFrameSizeInBytes(aidlConfig.base.format, aidlConfig.base.channelMask);
    // Do not disarm cleanups to release temporary port configs.
    return OK;
}

status_t DeviceHalAidl::prepareToOpenStream(
        int32_t aidlHandle, const AudioDevice& aidlDevice, const AudioIoFlags& aidlFlags,
        AudioSource aidlSource, struct audio_config* config,
        Cleanups* cleanups, AudioConfig* aidlConfig, AudioPortConfig* mixPortConfig,
        AudioPatch* aidlPatch) {
    ALOGD("%p %s::%s: handle %d, device %s, flags %s, source %s, config %s, mix port config %s",
            this, getClassName().c_str(), __func__, aidlHandle, aidlDevice.toString().c_str(),
            aidlFlags.toString().c_str(), toString(aidlSource).c_str(),
            aidlConfig->toString().c_str(), mixPortConfig->toString().c_str());
    resetUnusedPatchesAndPortConfigs();
    const bool isInput = aidlFlags.getTag() == AudioIoFlags::Tag::input;
    // Find / create AudioPortConfigs for the device port and the mix port,
    // then find / create a patch between them, and open a stream on the mix port.
    AudioPortConfig devicePortConfig;
    bool created = false;
    RETURN_STATUS_IF_ERROR(findOrCreatePortConfig(aidlDevice, aidlConfig,
                                                  &devicePortConfig, &created));
    if (created) {
        cleanups->emplace_front(this, &DeviceHalAidl::resetPortConfig, devicePortConfig.id);
    }
    RETURN_STATUS_IF_ERROR(findOrCreatePortConfig(*aidlConfig, aidlFlags, aidlHandle, aidlSource,
                    std::set<int32_t>{devicePortConfig.portId}, mixPortConfig, &created));
    if (created) {
        cleanups->emplace_front(this, &DeviceHalAidl::resetPortConfig, mixPortConfig->id);
    }
    setConfigFromPortConfig(aidlConfig, *mixPortConfig);
    if (isInput) {
        RETURN_STATUS_IF_ERROR(findOrCreatePatch(
                        {devicePortConfig.id}, {mixPortConfig->id}, aidlPatch, &created));
    } else {
        RETURN_STATUS_IF_ERROR(findOrCreatePatch(
                        {mixPortConfig->id}, {devicePortConfig.id}, aidlPatch, &created));
    }
    if (created) {
        cleanups->emplace_front(this, &DeviceHalAidl::resetPatch, aidlPatch->id);
    }
    if (aidlConfig->frameCount <= 0) {
        aidlConfig->frameCount = aidlPatch->minimumStreamBufferSizeFrames;
    }
    *config = VALUE_OR_RETURN_STATUS(
            ::aidl::android::aidl2legacy_AudioConfig_audio_config_t(*aidlConfig, isInput));
    return OK;
}

namespace {

class StreamCallbackBase {
protected:
    explicit StreamCallbackBase(const sp<CallbackBroker>& broker) : mBroker(broker) {}
public:
    void* getCookie() const { return mCookie; }
    void setCookie(void* cookie) { mCookie = cookie; }
    sp<CallbackBroker> getBroker() const {
        if (void* cookie = mCookie; cookie != nullptr) return mBroker.promote();
        return nullptr;
    }
private:
    const wp<CallbackBroker> mBroker;
    std::atomic<void*> mCookie;
};

template<class C>
class StreamCallbackBaseHelper {
protected:
    explicit StreamCallbackBaseHelper(const StreamCallbackBase& base) : mBase(base) {}
    sp<C> getCb(const sp<CallbackBroker>& broker, void* cookie);
    using CbRef = const sp<C>&;
    ndk::ScopedAStatus runCb(const std::function<void(CbRef cb)>& f) {
        if (auto cb = getCb(mBase.getBroker(), mBase.getCookie()); cb != nullptr) f(cb);
        return ndk::ScopedAStatus::ok();
    }
private:
    const StreamCallbackBase& mBase;
};

public:
    ndk::ScopedAStatus onDrainReady() override {
        return runCb([](CbRef cb) { cb->onDrainReady(); });
    }
    ndk::ScopedAStatus onDrainReady() override {
        return runCb([](CbRef cb) { cb->onDrainReady(); });
    }
    ndk::ScopedAStatus onDrainReady() override {
        return runCb([](CbRef cb) { cb->onDrainReady(); });
    }
/*
Note on the callback ownership.

In the Binder ownership model, the server implementation is kept alive
as long as there is any client (proxy object) alive. This is done by
incrementing the refcount of the server-side object by the Binder framework.
When it detects that the last client is gone, it decrements the refcount back.

Thus, it is not needed to keep any references to StreamCallback on our
side (after we have sent an instance to the client), because we are
the server-side. The callback object will be kept alive as long as the HAL server
holds a strong ref to IStreamCallback proxy.
*/

/*
Note on the callback ownership.

In the Binder ownership model, the server implementation is kept alive
as long as there is any client (proxy object) alive. This is done by
incrementing the refcount of the server-side object by the Binder framework.
When it detects that the last client is gone, it decrements the refcount back.

Thus, it is not needed to keep any references to StreamCallback on our
side (after we have sent an instance to the client), because we are
the server-side. The callback object will be kept alive as long as the HAL server
holds a strong ref to IStreamCallback proxy.
*/
class OutputStreamCallbackAidl : public StreamCallbackBase,
                             public StreamCallbackBaseHelper<StreamOutHalInterfaceCallback>,
                             public ::aidl::android::hardware::audio::core::BnStreamCallback {


public:
    explicit OutputStreamCallbackAidl(const sp<CallbackBroker>& broker)
            : StreamCallbackBase(broker),
              StreamCallbackBaseHelper<StreamOutHalInterfaceCallback>(
                      *static_cast<StreamCallbackBase*>(this)) {}
    ndk::ScopedAStatus onDrainReady() override {
        return runCb([](CbRef cb) { cb->onDrainReady(); });
    }
    ndk::ScopedAStatus onDrainReady() override {
        return runCb([](CbRef cb) { cb->onDrainReady(); });
    }
    ndk::ScopedAStatus onDrainReady() override {
        return runCb([](CbRef cb) { cb->onDrainReady(); });
    }
};

class OutputStreamEventCallbackAidl :
            public StreamCallbackBase,
            public StreamCallbackBaseHelper<StreamOutHalInterfaceEventCallback>,
            public StreamCallbackBaseHelper<StreamOutHalInterfaceLatencyModeCallback>,
            public ::aidl::android::hardware::audio::core::BnStreamOutEventCallback {




public:
    explicit OutputStreamEventCallbackAidl(const sp<CallbackBroker>& broker)
            : StreamCallbackBase(broker),
              StreamCallbackBaseHelper<StreamOutHalInterfaceEventCallback>(
                      *static_cast<StreamCallbackBase*>(this)),
              StreamCallbackBaseHelper<StreamOutHalInterfaceLatencyModeCallback>(
                      *static_cast<StreamCallbackBase*>(this)) {}
    ndk::ScopedAStatus onCodecFormatChanged(const std::vector<uint8_t>& in_audioMetadata) override {
        std::basic_string<uint8_t> halMetadata(in_audioMetadata.begin(), in_audioMetadata.end());
        return StreamCallbackBaseHelper<StreamOutHalInterfaceEventCallback>::runCb(
                [&halMetadata](auto cb) { cb->onCodecFormatChanged(halMetadata); });
    }
    ndk::ScopedAStatus onRecommendedLatencyModeChanged(
            const std::vector<AudioLatencyMode>& in_modes) override {
        auto halModes = VALUE_OR_FATAL(
                ::aidl::android::convertContainer<std::vector<audio_latency_mode_t>>(
                        in_modes,
                        ::aidl::android::aidl2legacy_AudioLatencyMode_audio_latency_mode_t));
        return StreamCallbackBaseHelper<StreamOutHalInterfaceLatencyModeCallback>::runCb(
                [&halModes](auto cb) { cb->onRecommendedLatencyModeChanged(halModes); });
    }
};

} // namespace

status_t DeviceHalAidl::openOutputStream(
        audio_io_handle_t handle, audio_devices_t devices,
        audio_output_flags_t flags, struct audio_config* config,
        const char* address,
        sp<StreamOutHalInterface>* outStream) {
    ALOGD("%p %s::%s", this, getClassName().c_str(), __func__);
    if (!outStream || !config) {
        return BAD_VALUE;
    }
    TIME_CHECK();
    if (!mModule) return NO_INIT;
    int32_t aidlHandle = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_io_handle_t_int32_t(handle));
    AudioConfig aidlConfig = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_config_t_AudioConfig(*config, false /*isInput*/));
    AudioDevice aidlDevice = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_device_AudioDevice(devices, address));
    int32_t aidlOutputFlags = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_output_flags_t_int32_t_mask(flags));
    AudioIoFlags aidlFlags = AudioIoFlags::make<AudioIoFlags::Tag::output>(aidlOutputFlags);
    AudioPortConfig mixPortConfig;
    Cleanups cleanups;
    AudioPatch aidlPatch;
    RETURN_STATUS_IF_ERROR(prepareToOpenStream(aidlHandle, aidlDevice, aidlFlags,
                    AudioSource::SYS_RESERVED_INVALID /*only needed for input*/,
                    config, &cleanups, &aidlConfig, &mixPortConfig, &aidlPatch));
    ::aidl::android::hardware::audio::core::IModule::OpenOutputStreamArguments args;
    args.portConfigId = mixPortConfig.id;
    const bool isOffload = isBitPositionFlagSet(
            aidlOutputFlags, AudioOutputFlags::COMPRESS_OFFLOAD);
    std::shared_ptr<OutputStreamCallbackAidl> streamCb;
    if (isOffload) {
        streamCb = ndk::SharedRefBase::make<OutputStreamCallbackAidl>(this);
    }
    auto eventCb = ndk::SharedRefBase::make<OutputStreamEventCallbackAidl>(this);
    if (isOffload) {
        args.offloadInfo = aidlConfig.offloadInfo;
        args.callback = streamCb;
    }
    args.bufferSizeFrames = aidlConfig.frameCount;
    args.eventCallback = eventCb;
    ::aidl::android::hardware::audio::core::IModule::OpenOutputStreamReturn ret;
    RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->openOutputStream(args, &ret)));
    StreamContextAidl context(ret.desc, isOffload);
    if (!context.isValid()) {
        ALOGE("%s: Failed to created a valid stream context from the descriptor: %s",
                __func__, ret.desc.toString().c_str());
        return NO_INIT;
    }
    *outStream = sp<StreamOutHalAidl>::make(*config, std::move(context), aidlPatch.latenciesMs[0],
            std::move(ret.stream), this /*callbackBroker*/);
    mStreams.insert(std::pair(*outStream, aidlPatch.id));
    void* cbCookie = (*outStream).get();
    {
        std::lock_guard l(mLock);
        mCallbacks.emplace(cbCookie, Callbacks{});
    }
    if (streamCb) streamCb->setCookie(cbCookie);
    eventCb->setCookie(cbCookie);
    cleanups.disarmAll();
    return OK;
}

status_t DeviceHalAidl::openInputStream(
        audio_io_handle_t handle, audio_devices_t devices,
        struct audio_config* config, audio_input_flags_t flags,
        const char* address, audio_source_t source,
        audio_devices_t outputDevice, const char* outputDeviceAddress,
        sp<StreamInHalInterface>* inStream) {
    ALOGD("%p %s::%s", this, getClassName().c_str(), __func__);
    if (!inStream || !config) {
        return BAD_VALUE;
    }
    TIME_CHECK();
    if (!mModule) return NO_INIT;
    int32_t aidlHandle = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_io_handle_t_int32_t(handle));
    AudioConfig aidlConfig = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_config_t_AudioConfig(*config, true /*isInput*/));
    AudioDevice aidlDevice = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_device_AudioDevice(devices, address));
    int32_t aidlInputFlags = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_input_flags_t_int32_t_mask(flags));
    AudioIoFlags aidlFlags = AudioIoFlags::make<AudioIoFlags::Tag::input>(aidlInputFlags);
    AudioSource aidlSource = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_source_t_AudioSource(source));
    AudioPortConfig mixPortConfig;
    Cleanups cleanups;
    AudioPatch aidlPatch;
    RETURN_STATUS_IF_ERROR(prepareToOpenStream(aidlHandle, aidlDevice, aidlFlags, aidlSource,
                    config, &cleanups, &aidlConfig, &mixPortConfig, &aidlPatch));
    ::aidl::android::hardware::audio::core::IModule::OpenInputStreamArguments args;
    args.portConfigId = mixPortConfig.id;
    RecordTrackMetadata aidlTrackMetadata{
        .source = aidlSource, .gain = 1, .channelMask = aidlConfig.base.channelMask };
    if (outputDevice != AUDIO_DEVICE_NONE) {
        aidlTrackMetadata.destinationDevice = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_device_AudioDevice(
                    outputDevice, outputDeviceAddress));
    }
    args.sinkMetadata.tracks.push_back(std::move(aidlTrackMetadata));
    args.bufferSizeFrames = aidlConfig.frameCount;
    ::aidl::android::hardware::audio::core::IModule::OpenInputStreamReturn ret;
    RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->openInputStream(args, &ret)));
    StreamContextAidl context(ret.desc, false /*isAsynchronous*/);
    if (!context.isValid()) {
        ALOGE("%s: Failed to created a valid stream context from the descriptor: %s",
                __func__, ret.desc.toString().c_str());
        return NO_INIT;
    }
    *inStream = sp<StreamInHalAidl>::make(*config, std::move(context), aidlPatch.latenciesMs[0],
            std::move(ret.stream), this /*micInfoProvider*/);
    mStreams.insert(std::pair(*inStream, aidlPatch.id));
    cleanups.disarmAll();
    return OK;
}

status_t DeviceHalAidl::supportsAudioPatches(bool* supportsPatches) {
    *supportsPatches = true;
    return OK;
}

status_t DeviceHalAidl::createAudioPatch(unsigned int num_sources,
                                         const struct audio_port_config* sources,
                                         unsigned int num_sinks,
                                         const struct audio_port_config* sinks,
                                         audio_patch_handle_t* patch) {
    ALOGD("%p %s::%s", this, getClassName().c_str(), __func__);
    TIME_CHECK();
    if (!mModule) return NO_INIT;
    if (num_sinks > AUDIO_PATCH_PORTS_MAX || num_sources > AUDIO_PATCH_PORTS_MAX ||
        sources == nullptr || sinks == nullptr || patch == nullptr) {
        return BAD_VALUE;
    }
    // When the patch handle (*patch) is AUDIO_PATCH_HANDLE_NONE, it means
    // the framework wants to create a new patch. The handle has to be generated
    // by the HAL. Since handles generated this way can only be unique within
    // a HAL module, the framework generates a globally unique handle, and maps
    // it on the <HAL module, patch handle> pair.
    // When the patch handle is set, it meant the framework intends to update
    // an existing patch.
    //
    // This behavior corresponds to HAL module behavior, with the only difference
    // that the HAL module uses `int32_t` for patch IDs. The following assert ensures
    // that both the framework and the HAL use the same value for "no ID":
    static_assert(AUDIO_PATCH_HANDLE_NONE == 0);
    int32_t halPatchId = static_cast<int32_t>(*patch);

    // Upon conversion, mix port configs contain audio configuration, while
    // device port configs contain device address. This data is used to find
    // or create HAL configs.
    std::vector<AudioPortConfig> aidlSources, aidlSinks;
    for (unsigned int i = 0; i < num_sources; ++i) {
        bool isInput = VALUE_OR_RETURN_STATUS(::aidl::android::portDirection(
                        sources[i].role, sources[i].type)) ==
                ::aidl::android::AudioPortDirection::INPUT;
        aidlSources.push_back(VALUE_OR_RETURN_STATUS(
                        ::aidl::android::legacy2aidl_audio_port_config_AudioPortConfig(
                                sources[i], isInput, 0)));
    }
    for (unsigned int i = 0; i < num_sinks; ++i) {
        bool isInput = VALUE_OR_RETURN_STATUS(::aidl::android::portDirection(
                        sinks[i].role, sinks[i].type)) ==
                ::aidl::android::AudioPortDirection::INPUT;
        aidlSinks.push_back(VALUE_OR_RETURN_STATUS(
                        ::aidl::android::legacy2aidl_audio_port_config_AudioPortConfig(
                                sinks[i], isInput, 0)));
    }
    Cleanups cleanups;
    auto existingPatchIt = halPatchId != 0 ? mPatches.find(halPatchId): mPatches.end();
    AudioPatch aidlPatch;
    if (existingPatchIt != mPatches.end()) {
        aidlPatch = existingPatchIt->second;
        aidlPatch.sourcePortConfigIds.clear();
        aidlPatch.sinkPortConfigIds.clear();
    }
    ALOGD("%s: sources: %s, sinks: %s",
            __func__, ::android::internal::ToString(aidlSources).c_str(),
            ::android::internal::ToString(aidlSinks).c_str());
    auto fillPortConfigs = [&](
            const std::vector<AudioPortConfig>& configs,
            const std::set<int32_t>& destinationPortIds,
            std::vector<int32_t>* ids, std::set<int32_t>* portIds) -> status_t {
        for (const auto& s : configs) {
            AudioPortConfig portConfig;
            bool created = false;
            RETURN_STATUS_IF_ERROR(findOrCreatePortConfig(
                            s, destinationPortIds, &portConfig, &created));
            if (created) {
                cleanups.emplace_front(this, &DeviceHalAidl::resetPortConfig, portConfig.id);
            }
            ids->push_back(portConfig.id);
            if (portIds != nullptr) {
                portIds->insert(portConfig.portId);
            }
        }
        return OK;
    };
    // When looking up port configs, the destinationPortId is only used for mix ports.
    // Thus, we process device port configs first, and look up the destination port ID from them.
    bool sourceIsDevice = std::any_of(aidlSources.begin(), aidlSources.end(),
            [](const auto& config) { return config.ext.getTag() == AudioPortExt::device; });
    const std::vector<AudioPortConfig>& devicePortConfigs =
            sourceIsDevice ? aidlSources : aidlSinks;
    std::vector<int32_t>* devicePortConfigIds =
            sourceIsDevice ? &aidlPatch.sourcePortConfigIds : &aidlPatch.sinkPortConfigIds;
    const std::vector<AudioPortConfig>& mixPortConfigs =
            sourceIsDevice ? aidlSinks : aidlSources;
    std::vector<int32_t>* mixPortConfigIds =
            sourceIsDevice ? &aidlPatch.sinkPortConfigIds : &aidlPatch.sourcePortConfigIds;
    std::set<int32_t> devicePortIds;
    RETURN_STATUS_IF_ERROR(fillPortConfigs(
                    devicePortConfigs, std::set<int32_t>(), devicePortConfigIds, &devicePortIds));
    RETURN_STATUS_IF_ERROR(fillPortConfigs(
                    mixPortConfigs, devicePortIds, mixPortConfigIds, nullptr));
    if (existingPatchIt != mPatches.end()) {
        RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(
                        mModule->setAudioPatch(aidlPatch, &aidlPatch)));
        existingPatchIt->second = aidlPatch;
    } else {
        bool created = false;
        RETURN_STATUS_IF_ERROR(findOrCreatePatch(aidlPatch, &aidlPatch, &created));
        // Since no cleanup of the patch is needed, 'created' is ignored.
        halPatchId = aidlPatch.id;
        *patch = static_cast<audio_patch_handle_t>(halPatchId);
    }
    cleanups.disarmAll();
    return OK;
}

status_t DeviceHalAidl::releaseAudioPatch(audio_patch_handle_t patch) {
    ALOGD("%p %s::%s", this, getClassName().c_str(), __func__);
    TIME_CHECK();
    if (!mModule) return NO_INIT;
    static_assert(AUDIO_PATCH_HANDLE_NONE == 0);
    if (patch == AUDIO_PATCH_HANDLE_NONE) {
        return BAD_VALUE;
    }
    int32_t halPatchId = static_cast<int32_t>(patch);
    auto patchIt = mPatches.find(halPatchId);
    if (patchIt == mPatches.end()) {
        ALOGE("%s: patch with id %d not found", __func__, halPatchId);
        return BAD_VALUE;
    }
    RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->resetAudioPatch(halPatchId)));
    mPatches.erase(patchIt);
    return OK;
}

status_t DeviceHalAidl::getAudioPort(struct audio_port* port) {
    ALOGD("%p %s::%s", this, getClassName().c_str(), __func__);
    TIME_CHECK();
    if (!mModule) return NO_INIT;
    if (port == nullptr) {
        return BAD_VALUE;
    }
    audio_port_v7 portV7;
    audio_populate_audio_port_v7(port, &portV7);
    RETURN_STATUS_IF_ERROR(getAudioPort(&portV7));
    return audio_populate_audio_port(&portV7, port) ? OK : BAD_VALUE;
}

status_t DeviceHalAidl::getAudioPort(struct audio_port_v7 *port) {
    ALOGD("%p %s::%s", this, getClassName().c_str(), __func__);
    TIME_CHECK();
    if (!mModule) return NO_INIT;
    if (port == nullptr) {
        return BAD_VALUE;
    }
    bool isInput = VALUE_OR_RETURN_STATUS(::aidl::android::portDirection(port->role, port->type)) ==
            ::aidl::android::AudioPortDirection::INPUT;
    auto aidlPort = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_port_v7_AudioPort(*port, isInput));
    if (aidlPort.ext.getTag() != AudioPortExt::device) {
        ALOGE("%s: provided port is not a device port (module %s): %s",
                __func__, mInstance.c_str(), aidlPort.toString().c_str());
        return BAD_VALUE;
    }
    const auto& matchDevice = aidlPort.ext.get<AudioPortExt::device>().device;
    // It seems that we don't have to call HAL since all valid ports have been added either
    // during initialization, or while handling connection of an external device.
    auto portsIt = findPort(matchDevice);
    if (portsIt == mPorts.end()) {
        ALOGE("%s: device port for device %s is not found in the module %s",
                __func__, matchDevice.toString().c_str(), mInstance.c_str());
        return BAD_VALUE;
    }
    const int32_t fwkId = aidlPort.id;
    aidlPort = portsIt->second;
    aidlPort.id = fwkId;
    *port = VALUE_OR_RETURN_STATUS(::aidl::android::aidl2legacy_AudioPort_audio_port_v7(
                    aidlPort, isInput));
    return OK;
}

status_t DeviceHalAidl::setAudioPortConfig(const struct audio_port_config* config) {
    ALOGD("%p %s::%s", this, getClassName().c_str(), __func__);
    TIME_CHECK();
    if (!mModule) return NO_INIT;
    if (config == nullptr) {
        return BAD_VALUE;
    }
    bool isInput = VALUE_OR_RETURN_STATUS(::aidl::android::portDirection(
                    config->role, config->type)) == ::aidl::android::AudioPortDirection::INPUT;
    AudioPortConfig requestedPortConfig = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_port_config_AudioPortConfig(
                    *config, isInput, 0 /*portId*/));
    AudioPortConfig portConfig;
    bool created = false;
    RETURN_STATUS_IF_ERROR(findOrCreatePortConfig(
                    requestedPortConfig, std::set<int32_t>(), &portConfig, &created));
    return OK;
}

MicrophoneInfoProvider::Info const* DeviceHalAidl::getMicrophoneInfo() {
    if (mMicrophones.status == Microphones::Status::UNKNOWN) {
        TIME_CHECK();
        std::vector<MicrophoneInfo> aidlInfo;
        status_t status = statusTFromBinderStatus(mModule->getMicrophones(&aidlInfo));
        if (status == OK) {
            mMicrophones.status = Microphones::Status::QUERIED;
            mMicrophones.info = std::move(aidlInfo);
        } else if (status == INVALID_OPERATION) {
            mMicrophones.status = Microphones::Status::NOT_SUPPORTED;
        } else {
            ALOGE("%s: Unexpected status from 'IModule.getMicrophones': %d", __func__, status);
            return {};
        }
    }
    if (mMicrophones.status == Microphones::Status::QUERIED) {
        return &mMicrophones.info;
    }
    return {};  // NOT_SUPPORTED
}

status_t DeviceHalAidl::getMicrophones(
        std::vector<audio_microphone_characteristic_t>* microphones) {
    if (!microphones) {
        return BAD_VALUE;
    }
    TIME_CHECK();
    if (!mModule) return NO_INIT;
    auto staticInfo = getMicrophoneInfo();
    if (!staticInfo) return INVALID_OPERATION;
    std::vector<MicrophoneDynamicInfo> emptyDynamicInfo;
    emptyDynamicInfo.reserve(staticInfo->size());
    std::transform(staticInfo->begin(), staticInfo->end(), std::back_inserter(emptyDynamicInfo),
            [](const auto& info) { return MicrophoneDynamicInfo{ .id = info.id }; });
    *microphones = VALUE_OR_RETURN_STATUS(
            ::aidl::android::convertContainers<std::vector<audio_microphone_characteristic_t>>(
                    *staticInfo, emptyDynamicInfo,
                    ::aidl::android::aidl2legacy_MicrophoneInfos_audio_microphone_characteristic_t)
    );
    return OK;
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

} // namespace android
