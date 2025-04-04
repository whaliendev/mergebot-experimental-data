       
#include <map>
#include <set>
#include <vector>
#include <aidl/android/hardware/audio/core/BpModule.h>
#include <aidl/android/hardware/audio/core/sounddose/BpSoundDose.h>
#include <android-base/thread_annotations.h>
#include <media/audiohal/DeviceHalInterface.h>
#include <media/audiohal/EffectHalInterface.h>
#include <media/audiohal/StreamHalInterface.h>
#include "ConversionHelperAidl.h"
namespace android {
class StreamOutHalInterfaceCallback;
class StreamOutHalInterfaceEventCallback;
class StreamOutHalInterfaceLatencyModeCallback;
class CallbackBroker : public virtual RefBase {
public:
    virtual ~CallbackBroker()
    virtual void clearCallbacks(void* cookie) = 0;
    virtual sp<StreamOutHalInterfaceCallback> getStreamOutCallback(void* cookie) = 0;
    virtual void setStreamOutCallback(void* cookie, const sp<StreamOutHalInterfaceCallback>&) = 0;
    virtual sp<StreamOutHalInterfaceEventCallback> getStreamOutEventCallback(void* cookie) = 0;
    virtual void setStreamOutEventCallback(void* cookie,
            const sp<StreamOutHalInterfaceEventCallback>&) = 0;
    virtual sp<StreamOutHalInterfaceLatencyModeCallback> getStreamOutLatencyModeCallback(
            void* cookie) = 0;
    virtual void setStreamOutLatencyModeCallback(
            void* cookie, const sp<StreamOutHalInterfaceLatencyModeCallback>&) = 0;
};
class MicrophoneInfoProvider : public virtual RefBase {
public:
    using Info = std::vector<::aidl::android::media::audio::common::MicrophoneInfo>;
    virtual ~MicrophoneInfoProvider()
    virtual Info const* getMicrophoneInfo() = 0;
};
class DeviceHalAidl : public DeviceHalInterface, public ConversionHelperAidl,
                      public CallbackBroker, public MicrophoneInfoProvider {
public:
    status_t getAudioPorts(std::vector<media::audio::common::AudioPort> *ports) override;
    status_t getAudioRoutes(std::vector<media::AudioRoute> *routes) override;
    status_t getSupportedDevices(uint32_t *devices) override;
    status_t initCheck() override;
    status_t setVoiceVolume(float volume) override;
    status_t setMasterVolume(float volume) override;
    status_t getMasterVolume(float *volume) override;
    status_t setMode(audio_mode_t mode) override;
    status_t setMicMute(bool state) override;
    status_t getMicMute(bool* state) override;
    status_t setMasterMute(bool state) override;
    status_t getMasterMute(bool *state) override;
    status_t setParameters(const String8& kvPairs) override;
    status_t getParameters(const String8& keys, String8 *values) override;
    status_t getInputBufferSize(const struct audio_config* config, size_t* size) override;
    status_t openOutputStream(audio_io_handle_t handle, audio_devices_t devices,
                              audio_output_flags_t flags, struct audio_config* config,
                              const char* address, sp<StreamOutHalInterface>* outStream) override;
    status_t openInputStream(audio_io_handle_t handle, audio_devices_t devices,
                             struct audio_config* config, audio_input_flags_t flags,
                             const char* address, audio_source_t source,
                             audio_devices_t outputDevice, const char* outputDeviceAddress,
                             sp<StreamInHalInterface>* inStream) override;
    status_t supportsAudioPatches(bool* supportsPatches) override;
    status_t createAudioPatch(unsigned int num_sources, const struct audio_port_config* sources,
                              unsigned int num_sinks, const struct audio_port_config* sinks,
                              audio_patch_handle_t* patch) override;
    status_t releaseAudioPatch(audio_patch_handle_t patch) override;
    status_t getAudioPort(struct audio_port* port) override;
    status_t getAudioPort(struct audio_port_v7 *port) override;
    status_t setAudioPortConfig(const struct audio_port_config* config) override;
    status_t getMicrophones(std::vector<audio_microphone_characteristic_t>* microphones) override;
    status_t addDeviceEffect(audio_port_handle_t device, sp<EffectHalInterface> effect) override;
    status_t removeDeviceEffect(audio_port_handle_t device, sp<EffectHalInterface> effect) override;
    status_t getMmapPolicyInfos(media::audio::common::AudioMMapPolicyType policyType __unused,
                                std::vector<media::audio::common::AudioMMapPolicyInfo>* policyInfos
                                        __unused) override;
    int32_t getAAudioMixerBurstCount() override;
    int32_t getAAudioHardwareBurstMinUsec() override;
    error::Result<audio_hw_sync_t> getHwAvSync() override;
    int32_t supportsBluetoothVariableLatency(bool* supports __unused) override;
    status_t getSoundDoseInterface(const std::string& module,
                                   ::ndk::SpAIBinder* soundDoseBinder) override;
    status_t prepareToDisconnectExternalDevice(const struct audio_port_v7 *port) override;
    status_t setConnectedState(const struct audio_port_v7 *port, bool connected) override;
    status_t setSimulateDeviceConnections(bool enabled) override;
    status_t dump(int __unused, const Vector<String16>& __unused) override;
private:
    friend class sp<DeviceHalAidl>;
    struct Callbacks {
        wp<StreamOutHalInterfaceCallback> out;
        wp<StreamOutHalInterfaceEventCallback> event;
        wp<StreamOutHalInterfaceLatencyModeCallback> latency;
    };
    struct Microphones {
        enum Status { UNKNOWN, NOT_SUPPORTED, QUERIED };
        Status status = Status::UNKNOWN;
        MicrophoneInfoProvider::Info info;
    };
    using Patches = std::map<int32_t ,
            ::aidl::android::hardware::audio::core::AudioPatch>;
    using PortConfigs = std::map<int32_t ,
            ::aidl::android::media::audio::common::AudioPortConfig>;
    using Ports = std::map<int32_t , ::aidl::android::media::audio::common::AudioPort>;
    using Routes = std::vector<::aidl::android::hardware::audio::core::AudioRoute>;
    using RoutingMatrix = std::set<std::pair<int32_t, int32_t>>;
    using Streams = std::map<wp<StreamHalInterface>, int32_t >;
    class Cleanups;
    DeviceHalAidl(
            const std::string& instance,
            const std::shared_ptr<::aidl::android::hardware::audio::core::IModule>& module)
            : ConversionHelperAidl("DeviceHalAidl"), mInstance(instance), mModule(module) {}
    () = delete;
    () = delete;
    bool audioDeviceMatches(const ::aidl::android::media::audio::common::AudioDevice& device,
            const ::aidl::android::media::audio::common::AudioPortConfig& p);
    bool audioDeviceMatches(const ::aidl::android::media::audio::common::AudioDevice& device,
            const ::aidl::android::media::audio::common::AudioPortConfig& p);
    status_t createOrUpdatePortConfig(
            const ::aidl::android::media::audio::common::AudioPortConfig& requestedPortConfig,
            PortConfigs::iterator* result, bool *created);
    status_t findOrCreatePatch(
        const std::set<int32_t>& sourcePortConfigIds,
        const std::set<int32_t>& sinkPortConfigIds,
        ::aidl::android::hardware::audio::core::AudioPatch* patch, bool* created);
    status_t findOrCreatePatch(
        const ::aidl::android::hardware::audio::core::AudioPatch& requestedPatch,
        ::aidl::android::hardware::audio::core::AudioPatch* patch, bool* created);
    status_t findOrCreatePortConfig(
            const ::aidl::android::media::audio::common::AudioDevice& device,
            const ::aidl::android::media::audio::common::AudioConfig* config,
            ::aidl::android::media::audio::common::AudioPortConfig* portConfig,
            bool* created);
    status_t findOrCreatePortConfig(
            const ::aidl::android::media::audio::common::AudioConfig& config,
            const std::optional<::aidl::android::media::audio::common::AudioIoFlags>& flags,
            int32_t ioHandle,
            ::aidl::android::media::audio::common::AudioSource aidlSource,
            const std::set<int32_t>& destinationPortIds,
            ::aidl::android::media::audio::common::AudioPortConfig* portConfig, bool* created);
    status_t findOrCreatePortConfig(
        const ::aidl::android::media::audio::common::AudioPortConfig& requestedPortConfig,
        const std::set<int32_t>& destinationPortIds,
        ::aidl::android::media::audio::common::AudioPortConfig* portConfig, bool* created);
    Patches::iterator findPatch(const std::set<int32_t>& sourcePortConfigIds,
            const std::set<int32_t>& sinkPortConfigIds);
    Ports::iterator findPort(const ::aidl::android::media::audio::common::AudioDevice& device);
    Ports::iterator findPort(
            const ::aidl::android::media::audio::common::AudioConfig& config,
            const ::aidl::android::media::audio::common::AudioIoFlags& flags,
            const std::set<int32_t>& destinationPortIds);
    PortConfigs::iterator findPortConfig(
            const ::aidl::android::media::audio::common::AudioDevice& device);
    PortConfigs::iterator findPortConfig(
            const ::aidl::android::media::audio::common::AudioConfig& config,
            const std::optional<::aidl::android::media::audio::common::AudioIoFlags>& flags,
            int32_t ioHandle);
    status_t prepareToOpenStream(
        int32_t aidlHandle,
        const ::aidl::android::media::audio::common::AudioDevice& aidlDevice,
        const ::aidl::android::media::audio::common::AudioIoFlags& aidlFlags,
        ::aidl::android::media::audio::common::AudioSource aidlSource,
        struct audio_config* config,
        Cleanups* cleanups,
        ::aidl::android::media::audio::common::AudioConfig* aidlConfig,
        ::aidl::android::media::audio::common::AudioPortConfig* mixPortConfig,
        ::aidl::android::hardware::audio::core::AudioPatch* aidlPatch);
    void resetPatch(int32_t patchId);
    void resetPortConfig(int32_t portConfigId);
    void resetUnusedPatches();
    void resetUnusedPatchesAndPortConfigs();
    void resetUnusedPortConfigs();
    status_t updateRoutes();
    void clearCallbacks(void* cookie) override;
    sp<StreamOutHalInterfaceCallback> getStreamOutCallback(void* cookie) override;
    void setStreamOutCallback(void* cookie, const sp<StreamOutHalInterfaceCallback>& cb) override;
    sp<StreamOutHalInterfaceEventCallback> getStreamOutEventCallback(void* cookie) override;
    void setStreamOutEventCallback(void* cookie,
            const sp<StreamOutHalInterfaceEventCallback>& cb) override;
    sp<StreamOutHalInterfaceLatencyModeCallback> getStreamOutLatencyModeCallback(
            void* cookie) override;
    void setStreamOutLatencyModeCallback(
            void* cookie, const sp<StreamOutHalInterfaceLatencyModeCallback>& cb) override;
    MicrophoneInfoProvider::Info const* getMicrophoneInfo() override;
    const std::string mInstance;
    const std::shared_ptr<::aidl::android::hardware::audio::core::IModule> mModule;
    std::shared_ptr<::aidl::android::hardware::audio::core::sounddose::ISoundDose>
        mSoundDose = nullptr;
    Ports mPorts;
    int32_t mDefaultInputPortId = -1;
    int32_t mDefaultOutputPortId = -1;
    PortConfigs mPortConfigs;
    std::set<int32_t> mInitialPortConfigIds;
    Patches mPatches;
    Routes mRoutes;
    RoutingMatrix mRoutingMatrix;
    Streams mStreams;
    Microphones mMicrophones;
    std::mutex mLock;
    std::map<void*, Callbacks> mCallbacks GUARDED_BY(mLock);
    std::set<audio_port_handle_t> mDeviceDisconnectionNotified;
};
}
