       
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_set>
#include <stdint.h>
#include <sys/types.h>
#include <cutils/config_utils.h>
#include <cutils/misc.h>
#include <utils/Timers.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/SortedVector.h>
#include <media/AudioParameter.h>
#include <media/AudioPolicy.h>
#include <media/AudioProfile.h>
#include <media/PatchBuilder.h>
#include "AudioPolicyInterface.h"
#include <android/media/DeviceConnectedState.h>
#include <android/media/audio/common/AudioPort.h>
#include <AudioPolicyManagerObserver.h>
#include <AudioPolicyConfig.h>
#include <PolicyAudioPort.h>
#include <AudioPatch.h>
#include <DeviceDescriptor.h>
#include <IOProfile.h>
#include <HwModule.h>
#include <AudioInputDescriptor.h>
#include <AudioOutputDescriptor.h>
#include <AudioPolicyMix.h>
#include <EffectDescriptor.h>
#include <PreferredMixerAttributesInfo.h>
#include <SoundTriggerSession.h>
#include "EngineLibrary.h"
#include "TypeConverter.h"
namespace android {
using content::AttributionSourceState;
#define SONIFICATION_HEADSET_VOLUME_FACTOR_DB (-6)
#define SONIFICATION_HEADSET_VOLUME_MIN_DB (-36)
#define SONIFICATION_A2DP_MAX_MEDIA_DIFF_DB (12)
#define SONIFICATION_HEADSET_MUSIC_DELAY 5000
#define MUTE_TIME_MS 2000
#define LATENCY_MUTE_FACTOR 4
#define NUM_TEST_OUTPUTS 5
#define NUM_VOL_CURVE_KNEES 2
#define OFFLOAD_DEFAULT_MIN_DURATION_SECS 60
class AudioPolicyManager : public AudioPolicyInterface, public AudioPolicyManagerObserver {
  public:
    AudioPolicyManager(const sp<const AudioPolicyConfig>& config, EngineInstance&& engine,
                       AudioPolicyClientInterface* clientInterface);
    virtual ~AudioPolicyManager();
    virtual status_t setDeviceConnectionState(audio_policy_dev_state_t state,
                                              const android::media::audio::common::AudioPort& port,
                                              audio_format_t encodedFormat);
    virtual audio_policy_dev_state_t getDeviceConnectionState(audio_devices_t device,
                                                              const char* device_address);
    virtual status_t handleDeviceConfigChange(audio_devices_t device, const char* device_address,
                                              const char* device_name,
                                              audio_format_t encodedFormat);
    virtual void setPhoneState(audio_mode_t state);
    virtual void setForceUse(audio_policy_force_use_t usage, audio_policy_forced_cfg_t config);
    virtual audio_policy_forced_cfg_t getForceUse(audio_policy_force_use_t usage);
    virtual void setSystemProperty(const char* property, const char* value);
    virtual status_t initCheck();
    virtual audio_io_handle_t getOutput(audio_stream_type_t stream);
    status_t getOutputForAttr(const audio_attributes_t* attr, audio_io_handle_t* output,
                              audio_session_t session, audio_stream_type_t* stream,
                              const AttributionSourceState& attributionSource,
                              audio_config_t* config, audio_output_flags_t* flags,
                              audio_port_handle_t* selectedDeviceId, audio_port_handle_t* portId,
                              std::vector<audio_io_handle_t>* secondaryOutputs,
                              output_type_t* outputType, bool* isSpatialized,
                              bool* isBitPerfect) override;
    virtual status_t startOutput(audio_port_handle_t portId);
    virtual status_t stopOutput(audio_port_handle_t portId);
    virtual bool releaseOutput(audio_port_handle_t portId);
    virtual status_t getInputForAttr(const audio_attributes_t* attr, audio_io_handle_t* input,
                                     audio_unique_id_t riid, audio_session_t session,
                                     const AttributionSourceState& attributionSource,
                                     audio_config_base_t* config, audio_input_flags_t flags,
                                     audio_port_handle_t* selectedDeviceId, input_type_t* inputType,
                                     audio_port_handle_t* portId, uint32_t* virtualDeviceId);
    virtual status_t startInput(audio_port_handle_t portId);
    virtual status_t stopInput(audio_port_handle_t portId);
    virtual void releaseInput(audio_port_handle_t portId);
    virtual void checkCloseInputs();
    virtual status_t setDeviceAbsoluteVolumeEnabled(audio_devices_t deviceType, const char* address,
                                                    bool enabled,
                                                    audio_stream_type_t streamToDriveAbs);
    virtual void initStreamVolume(audio_stream_type_t stream, int indexMin, int indexMax);
    virtual status_t setStreamVolumeIndex(audio_stream_type_t stream, int index,
                                          audio_devices_t device);
    virtual status_t getStreamVolumeIndex(audio_stream_type_t stream, int* index,
                                          audio_devices_t device);
    virtual status_t setVolumeIndexForAttributes(const audio_attributes_t& attr, int index,
                                                 audio_devices_t device);
    virtual status_t getVolumeIndexForAttributes(const audio_attributes_t& attr, int& index,
                                                 audio_devices_t device);
    virtual status_t getMaxVolumeIndexForAttributes(const audio_attributes_t& attr, int& index);
    virtual status_t getMinVolumeIndexForAttributes(const audio_attributes_t& attr, int& index);
    status_t setVolumeCurveIndex(int index, audio_devices_t device, IVolumeCurves& volumeCurves);
    status_t getVolumeIndex(const IVolumeCurves& curves, int& index,
                            const DeviceTypeSet& deviceTypes) const;
    virtual product_strategy_t getStrategyForStream(audio_stream_type_t stream) {
        return streamToStrategy(stream);
    }
    product_strategy_t streamToStrategy(audio_stream_type_t stream) const {
        auto attributes = mEngine->getAttributesForStreamType(stream);
        return mEngine->getProductStrategyForAttributes(attributes);
    }
    virtual status_t getDevicesForAttributes(const audio_attributes_t& attributes,
                                             AudioDeviceTypeAddrVector* devices, bool forVolume);
    virtual audio_io_handle_t getOutputForEffect(const effect_descriptor_t* desc = NULL);
    virtual status_t registerEffect(const effect_descriptor_t* desc, audio_io_handle_t io,
                                    product_strategy_t strategy, int session, int id);
    virtual status_t unregisterEffect(int id);
    virtual status_t setEffectEnabled(int id, bool enabled);
    status_t moveEffectsToIo(const std::vector<int>& ids, audio_io_handle_t io) override;
    virtual bool isStreamActive(audio_stream_type_t stream, uint32_t inPastMs = 0) const;
    virtual bool isStreamActiveRemotely(audio_stream_type_t stream, uint32_t inPastMs = 0) const;
    virtual bool isSourceActive(audio_source_t source) const;
    void dumpManualSurroundFormats(String8* dst) const;
    void dump(String8* dst) const;
    status_t dump(int fd) override;
    status_t setAllowedCapturePolicy(uid_t uid, audio_flags_mask_t capturePolicy) override;
    virtual audio_offload_mode_t getOffloadSupport(const audio_offload_info_t& offloadInfo);
    virtual bool isDirectOutputSupported(const audio_config_base_t& config,
                                         const audio_attributes_t& attributes);
    virtual status_t listAudioPorts(audio_port_role_t role, audio_port_type_t type,
                                    unsigned int* num_ports, struct audio_port_v7* ports,
                                    unsigned int* generation);
    status_t listDeclaredDevicePorts(media::AudioPortRole role,
                                     std::vector<media::AudioPortFw>* result) override;
    virtual status_t getAudioPort(struct audio_port_v7* port);
    virtual status_t createAudioPatch(const struct audio_patch* patch, audio_patch_handle_t* handle,
                                      uid_t uid);
    virtual status_t releaseAudioPatch(audio_patch_handle_t handle, uid_t uid);
    virtual status_t listAudioPatches(unsigned int* num_patches, struct audio_patch* patches,
                                      unsigned int* generation);
    virtual status_t setAudioPortConfig(const struct audio_port_config* config);
    virtual void releaseResourcesForUid(uid_t uid);
    virtual status_t acquireSoundTriggerSession(audio_session_t* session,
                                                audio_io_handle_t* ioHandle,
                                                audio_devices_t* device);
    virtual status_t releaseSoundTriggerSession(audio_session_t session) {
        return mSoundTriggerSessions.releaseSession(session);
    }
    virtual status_t registerPolicyMixes(const Vector<AudioMix>& mixes);
    virtual status_t unregisterPolicyMixes(Vector<AudioMix> mixes);
    virtual status_t getRegisteredPolicyMixes(std::vector<AudioMix>& mixes) override;
    virtual status_t updatePolicyMix(
            const AudioMix& mix,
            const std::vector<AudioMixMatchCriterion>& updatedCriteria) override;
    virtual status_t setUidDeviceAffinities(uid_t uid, const AudioDeviceTypeAddrVector& devices);
    virtual status_t removeUidDeviceAffinities(uid_t uid);
    virtual status_t setUserIdDeviceAffinities(int userId,
                                               const AudioDeviceTypeAddrVector& devices);
    virtual status_t removeUserIdDeviceAffinities(int userId);
    virtual status_t setDevicesRoleForStrategy(product_strategy_t strategy, device_role_t role,
                                               const AudioDeviceTypeAddrVector& devices);
    virtual status_t removeDevicesRoleForStrategy(product_strategy_t strategy, device_role_t role,
                                                  const AudioDeviceTypeAddrVector& devices);
    virtual status_t clearDevicesRoleForStrategy(product_strategy_t strategy, device_role_t role);
    virtual status_t getDevicesForRoleAndStrategy(product_strategy_t strategy, device_role_t role,
                                                  AudioDeviceTypeAddrVector& devices);
    virtual status_t setDevicesRoleForCapturePreset(audio_source_t audioSource, device_role_t role,
                                                    const AudioDeviceTypeAddrVector& devices);
    virtual status_t addDevicesRoleForCapturePreset(audio_source_t audioSource, device_role_t role,
                                                    const AudioDeviceTypeAddrVector& devices);
    virtual status_t removeDevicesRoleForCapturePreset(audio_source_t audioSource,
                                                       device_role_t role,
                                                       const AudioDeviceTypeAddrVector& devices);
    virtual status_t clearDevicesRoleForCapturePreset(audio_source_t audioSource,
                                                      device_role_t role);
    virtual status_t getDevicesForRoleAndCapturePreset(audio_source_t audioSource,
                                                       device_role_t role,
                                                       AudioDeviceTypeAddrVector& devices);
    virtual status_t startAudioSource(const struct audio_port_config* source,
                                      const audio_attributes_t* attributes,
                                      audio_port_handle_t* portId, uid_t uid,
                                      bool internal = false);
    virtual status_t stopAudioSource(audio_port_handle_t portId);
    virtual status_t setMasterMono(bool mono);
    virtual status_t getMasterMono(bool* mono);
    virtual float getStreamVolumeDB(audio_stream_type_t stream, int index, audio_devices_t device);
    virtual status_t getSurroundFormats(unsigned int* numSurroundFormats,
                                        audio_format_t* surroundFormats,
                                        bool* surroundFormatsEnabled);
    virtual status_t getReportedSurroundFormats(unsigned int* numSurroundFormats,
                                                audio_format_t* surroundFormats);
    virtual status_t setSurroundFormatEnabled(audio_format_t audioFormat, bool enabled);
    virtual status_t getHwOffloadFormatsSupportedForBluetoothMedia(
            audio_devices_t device, std::vector<audio_format_t>* formats);
    virtual void setAppState(audio_port_handle_t portId, app_state_t state);
    virtual bool isHapticPlaybackSupported();
    virtual bool isUltrasoundSupported();
    bool isHotwordStreamSupported(bool lookbackAudio) override;
    virtual status_t listAudioProductStrategies(AudioProductStrategyVector& strategies) {
        return mEngine->listAudioProductStrategies(strategies);
    }
    virtual status_t getProductStrategyFromAudioAttributes(const audio_attributes_t& aa,
                                                           product_strategy_t& productStrategy,
                                                           bool fallbackOnDefault) {
        productStrategy = mEngine->getProductStrategyForAttributes(aa, fallbackOnDefault);
        return (fallbackOnDefault && productStrategy == PRODUCT_STRATEGY_NONE) ? BAD_VALUE
                                                                               : NO_ERROR;
    }
    virtual status_t listAudioVolumeGroups(AudioVolumeGroupVector& groups) {
        return mEngine->listAudioVolumeGroups(groups);
    }
    virtual status_t getVolumeGroupFromAudioAttributes(const audio_attributes_t& aa,
                                                       volume_group_t& volumeGroup,
                                                       bool fallbackOnDefault) {
        volumeGroup = mEngine->getVolumeGroupForAttributes(aa, fallbackOnDefault);
        return (fallbackOnDefault && volumeGroup == VOLUME_GROUP_NONE) ? BAD_VALUE : NO_ERROR;
    }
    virtual bool canBeSpatialized(const audio_attributes_t* attr, const audio_config_t* config,
                                  const AudioDeviceTypeAddrVector& devices) const {
        return canBeSpatializedInt(attr, config, devices);
    }
    virtual status_t getSpatializerOutput(const audio_config_base_t* config,
                                          const audio_attributes_t* attr,
                                          audio_io_handle_t* output);
    virtual status_t releaseSpatializerOutput(audio_io_handle_t output);
    virtual audio_direct_mode_t getDirectPlaybackSupport(const audio_attributes_t* attr,
                                                         const audio_config_t* config);
    virtual status_t getDirectProfilesForAttributes(const audio_attributes_t* attr,
                                                    AudioProfileVector& audioProfiles);
    status_t getSupportedMixerAttributes(
            audio_port_handle_t portId, std::vector<audio_mixer_attributes_t>& mixerAttrs) override;
    status_t setPreferredMixerAttributes(const audio_attributes_t* attr, audio_port_handle_t portId,
                                         uid_t uid,
                                         const audio_mixer_attributes_t* mixerAttributes) override;
    status_t getPreferredMixerAttributes(const audio_attributes_t* attr, audio_port_handle_t portId,
                                         audio_mixer_attributes_t* mixerAttributes) override;
    status_t clearPreferredMixerAttributes(const audio_attributes_t* attr,
                                           audio_port_handle_t portId, uid_t uid) override;
    bool isCallScreenModeSupported() override;
    void onNewAudioModulesAvailable() override;
    status_t initialize();
  protected:
    const AudioPolicyConfig& getConfig() const { return *(mConfig.get()); }
    virtual const AudioPatchCollection& getAudioPatches() const { return mAudioPatches; }
    virtual const SoundTriggerSessionCollection& getSoundTriggerSessionCollection() const {
        return mSoundTriggerSessions;
    }
    virtual const AudioPolicyMixCollection& getAudioPolicyMixCollection() const {
        return mPolicyMixes;
    }
    virtual const SwAudioOutputCollection& getOutputs() const { return mOutputs; }
    virtual const AudioInputCollection& getInputs() const { return mInputs; }
    virtual const DeviceVector getAvailableOutputDevices() const {
        return mAvailableOutputDevices.filterForEngine();
    }
    virtual const DeviceVector getAvailableInputDevices() const {
        return mAvailableInputDevices;
    }
    virtual const sp<DeviceDescriptor>& getDefaultOutputDevice() const {
        return mConfig->getDefaultOutputDevice();
    }
    std::vector<volume_group_t> getVolumeGroups() const { return mEngine->getVolumeGroups(); }
    VolumeSource toVolumeSource(volume_group_t volumeGroup) const {
        return static_cast<VolumeSource>(volumeGroup);
    }
    VolumeSource toVolumeSource(const audio_attributes_t& attributes,
                                bool fallbackOnDefault = true) const {
        return toVolumeSource(mEngine->getVolumeGroupForAttributes(attributes, fallbackOnDefault));
    }
    VolumeSource toVolumeSource(audio_stream_type_t stream, bool fallbackOnDefault = true) const {
        return toVolumeSource(mEngine->getVolumeGroupForStreamType(stream, fallbackOnDefault));
    }
    IVolumeCurves& getVolumeCurves(VolumeSource volumeSource) {
        auto* curves =
                mEngine->getVolumeCurvesForVolumeGroup(static_cast<volume_group_t>(volumeSource));
        ALOG_ASSERT(curves != nullptr, "No curves for volume source %d", volumeSource);
        return *curves;
    }
    IVolumeCurves& getVolumeCurves(const audio_attributes_t& attr) {
        auto* curves = mEngine->getVolumeCurvesForAttributes(attr);
        ALOG_ASSERT(curves != nullptr, "No curves for attributes %s", toString(attr).c_str());
        return *curves;
    }
    IVolumeCurves& getVolumeCurves(audio_stream_type_t stream) {
        auto* curves = mEngine->getVolumeCurvesForStreamType(stream);
        ALOG_ASSERT(curves != nullptr, "No curves for stream %s", toString(stream).c_str());
        return *curves;
    }
    void addOutput(audio_io_handle_t output, const sp<SwAudioOutputDescriptor>& outputDesc);
    void removeOutput(audio_io_handle_t output);
    void addInput(audio_io_handle_t input, const sp<AudioInputDescriptor>& inputDesc);
    bool checkCloseInput(const sp<AudioInputDescriptor>& input);
    uint32_t setOutputDevices(const char* caller, const sp<SwAudioOutputDescriptor>& outputDesc,
                              const DeviceVector& device, bool force = false, int delayMs = 0,
                              audio_patch_handle_t* patchHandle = NULL,
                              bool requiresMuteCheck = true, bool requiresVolumeCheck = false,
                              bool skipMuteDelay = false);
    status_t resetOutputDevice(const sp<AudioOutputDescriptor>& outputDesc, int delayMs = 0,
                               audio_patch_handle_t* patchHandle = NULL);
    status_t setInputDevice(audio_io_handle_t input, const sp<DeviceDescriptor>& device,
                            bool force = false, audio_patch_handle_t* patchHandle = NULL);
    status_t resetInputDevice(audio_io_handle_t input, audio_patch_handle_t* patchHandle = NULL);
    virtual float computeVolume(IVolumeCurves& curves, VolumeSource volumeSource, int index,
                                const DeviceTypeSet& deviceTypes,
                                bool computeInternalInteraction = true);
    int rescaleVolumeIndex(int srcIndex, VolumeSource fromVolumeSource,
                           VolumeSource toVolumeSource);
    virtual status_t checkAndSetVolume(IVolumeCurves& curves, VolumeSource volumeSource, int index,
                                       const sp<AudioOutputDescriptor>& outputDesc,
                                       DeviceTypeSet deviceTypes, int delayMs = 0,
                                       bool force = false);
    void setVoiceVolume(int index, IVolumeCurves& curves, bool isVoiceVolSrc, int delayMs);
    bool isVolumeConsistentForCalls(VolumeSource volumeSource, const DeviceTypeSet& deviceTypes,
                                    bool& isVoiceVolSrc, bool& isBtScoVolSrc, const char* caller);
    void applyStreamVolumes(const sp<AudioOutputDescriptor>& outputDesc,
                            const DeviceTypeSet& deviceTypes, int delayMs = 0, bool force = false);
    void setStrategyMute(product_strategy_t strategy, bool on,
                         const sp<AudioOutputDescriptor>& outputDesc, int delayMs = 0,
                         DeviceTypeSet deviceTypes = DeviceTypeSet());
    void setVolumeSourceMute(VolumeSource volumeSource, bool on,
                             const sp<AudioOutputDescriptor>& outputDesc, int delayMs = 0,
                             DeviceTypeSet deviceTypes = DeviceTypeSet());
    audio_mode_t getPhoneState();
    virtual bool isInCall() const;
    virtual bool isStateInCall(int state) const;
    bool isCallAudioAccessible() const;
    bool isInCallOrScreening() const;
    status_t checkOutputsForDevice(const sp<DeviceDescriptor>& device,
                                   audio_policy_dev_state_t state,
                                   SortedVector<audio_io_handle_t>& outputs);
    status_t checkInputsForDevice(const sp<DeviceDescriptor>& device,
                                  audio_policy_dev_state_t state);
    void closeOutput(audio_io_handle_t output);
    void closeInput(audio_io_handle_t input);
    void checkForDeviceAndOutputChanges(std::function<bool()> onOutputsChecked = nullptr);
    void updateCallAndOutputRouting(bool forceVolumeReeval = true, uint32_t delayMs = 0,
                                    bool skipDelays = false);
    bool isCallRxAudioSource(const sp<SourceClientDescriptor>& source) {
        return mCallRxSourceClient != nullptr && source == mCallRxSourceClient;
    }
    bool isCallTxAudioSource(const sp<SourceClientDescriptor>& source) {
        return mCallTxSourceClient != nullptr && source == mCallTxSourceClient;
    }
    void connectTelephonyRxAudioSource();
    void disconnectTelephonyAudioSource(sp<SourceClientDescriptor>& clientDesc);
    void connectTelephonyTxAudioSource(const sp<DeviceDescriptor>& srcdevice,
                                       const sp<DeviceDescriptor>& sinkDevice, uint32_t delayMs);
    bool isTelephonyRxOrTx(const sp<SwAudioOutputDescriptor>& desc) const {
        return (mCallRxSourceClient != nullptr && mCallRxSourceClient->belongsToOutput(desc)) ||
               (mCallTxSourceClient != nullptr && mCallTxSourceClient->belongsToOutput(desc));
    }
    void updateInputRouting();
    void checkOutputForAttributes(const audio_attributes_t& attr);
    void checkAudioSourceForAttributes(const audio_attributes_t& attr);
    bool followsSameRouting(const audio_attributes_t& lAttr, const audio_attributes_t& rAttr) const;
    void checkOutputForAllStrategies();
    void checkSecondaryOutputs();
    void checkA2dpSuspend();
    DeviceVector getNewOutputDevices(const sp<SwAudioOutputDescriptor>& outputDesc, bool fromCache);
    void updateDevicesAndOutputs();
    sp<DeviceDescriptor> getNewInputDevice(const sp<AudioInputDescriptor>& inputDesc);
    virtual uint32_t getMaxEffectsCpuLoad() { return mEffects.getMaxEffectsCpuLoad(); }
    virtual uint32_t getMaxEffectsMemory() { return mEffects.getMaxEffectsMemory(); }
    SortedVector<audio_io_handle_t> getOutputsForDevices(
            const DeviceVector& devices, const SwAudioOutputCollection& openOutputs);
    virtual uint32_t checkDeviceMuteStrategies(const sp<AudioOutputDescriptor>& outputDesc,
                                               const DeviceVector& prevDevices, uint32_t delayMs);
    audio_io_handle_t selectOutput(const SortedVector<audio_io_handle_t>& outputs,
                                   audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE,
                                   audio_format_t format = AUDIO_FORMAT_INVALID,
                                   audio_channel_mask_t channelMask = AUDIO_CHANNEL_NONE,
                                   uint32_t samplingRate = 0,
                                   audio_session_t sessionId = AUDIO_SESSION_NONE);
    sp<IOProfile> getInputProfile(const sp<DeviceDescriptor>& device, uint32_t& samplingRate,
                                  audio_format_t& format, audio_channel_mask_t& channelMask,
                                  audio_input_flags_t flags);
    sp<IOProfile> getProfileForOutput(const DeviceVector& devices, uint32_t samplingRate,
                                      audio_format_t format, audio_channel_mask_t channelMask,
                                      audio_output_flags_t flags, bool directOnly);
    sp<IOProfile> getMsdProfileForOutput(const DeviceVector& devices, uint32_t samplingRate,
                                         audio_format_t format, audio_channel_mask_t channelMask,
                                         audio_output_flags_t flags, bool directOnly);
    audio_io_handle_t selectOutputForMusicEffects();
    virtual status_t addAudioPatch(audio_patch_handle_t handle, const sp<AudioPatch>& patch) {
        return mAudioPatches.addAudioPatch(handle, patch);
    }
    virtual status_t removeAudioPatch(audio_patch_handle_t handle) {
        return mAudioPatches.removeAudioPatch(handle);
    }
    bool isPrimaryModule(const sp<HwModule>& module) const {
        if (module == nullptr || mPrimaryModuleHandle == AUDIO_MODULE_HANDLE_NONE) {
            return false;
        }
        return module->getHandle() == mPrimaryModuleHandle;
    }
    DeviceVector availablePrimaryOutputDevices() const {
        if (!hasPrimaryOutput()) {
            return DeviceVector();
        }
        return mAvailableOutputDevices.filter(mPrimaryOutput->supportedDevices());
    }
    DeviceVector availablePrimaryModuleInputDevices() const {
        if (!hasPrimaryOutput()) {
            return DeviceVector();
        }
        return mAvailableInputDevices.getDevicesFromHwModule(mPrimaryOutput->getModuleHandle());
    }
    audio_port_handle_t getFirstDeviceId(const DeviceVector& devices) const {
        return (devices.size() > 0) ? devices.itemAt(0)->getId() : AUDIO_PORT_HANDLE_NONE;
    }
    String8 getFirstDeviceAddress(const DeviceVector& devices) const {
        return (devices.size() > 0) ? String8(devices.itemAt(0)->address().c_str()) : String8("");
    }
    status_t updateCallRouting(bool fromCache, uint32_t delayMs = 0, uint32_t* waitMs = nullptr);
    status_t updateCallRoutingInternal(const DeviceVector& rxDevices, uint32_t delayMs,
                                       uint32_t* waitMs);
    sp<AudioPatch> createTelephonyPatch(bool isRx, const sp<DeviceDescriptor>& device,
                                        uint32_t delayMs);
    DeviceVector selectBestRxSinkDevicesForCall(bool fromCache);
    bool isDeviceOfModule(const sp<DeviceDescriptor>& devDesc, const char* moduleId) const;
    status_t startSource(const sp<SwAudioOutputDescriptor>& outputDesc,
                         const sp<TrackClientDescriptor>& client, uint32_t* delayMs);
    status_t stopSource(const sp<SwAudioOutputDescriptor>& outputDesc,
                        const sp<TrackClientDescriptor>& client);
    void clearAudioPatches(uid_t uid);
    void clearSessionRoutes(uid_t uid);
    void checkStrategyRoute(product_strategy_t ps, audio_io_handle_t ouptutToSkip);
    status_t hasPrimaryOutput() const { return mPrimaryOutput != 0; }
    status_t connectAudioSource(const sp<SourceClientDescriptor>& sourceDesc);
    status_t disconnectAudioSource(const sp<SourceClientDescriptor>& sourceDesc);
    status_t connectAudioSourceToSink(const sp<SourceClientDescriptor>& sourceDesc,
                                      const sp<DeviceDescriptor>& sinkDevice,
                                      const struct audio_patch* patch, audio_patch_handle_t& handle,
                                      uid_t uid, uint32_t delayMs);
    sp<SourceClientDescriptor> getSourceForAttributesOnOutput(audio_io_handle_t output,
                                                              const audio_attributes_t& attr);
    void clearAudioSourcesForOutput(audio_io_handle_t output);
    void cleanUpForDevice(const sp<DeviceDescriptor>& deviceDesc);
    void clearAudioSources(uid_t uid);
    static bool streamsMatchForvolume(audio_stream_type_t stream1, audio_stream_type_t stream2);
    void closeActiveClients(const sp<AudioInputDescriptor>& input);
    void closeClient(audio_port_handle_t portId);
    bool isAnyDeviceTypeActive(const DeviceTypeSet& deviceTypes) const;
    bool isLeUnicastActive() const;
    void checkLeBroadcastRoutes(bool wasUnicastActive, sp<SwAudioOutputDescriptor> ignoredOutput,
                                uint32_t delayMs);
    const uid_t mUidCached;
    sp<const AudioPolicyConfig> mConfig;
    EngineInstance mEngine;
    AudioPolicyClientInterface* mpClientInterface;
    sp<SwAudioOutputDescriptor>
            mPrimaryOutput;
    audio_module_handle_t mPrimaryModuleHandle = AUDIO_MODULE_HANDLE_NONE;
    sp<SwAudioOutputDescriptor> mSpatializerOutput;
    SwAudioOutputCollection mOutputs;
    SwAudioOutputCollection mPreviousOutputs;
    AudioInputCollection mInputs;
    DeviceVector mAvailableOutputDevices;
    DeviceVector mAvailableInputDevices;
    bool mLimitRingtoneVolume;
    float mLastVoiceVolume;
    bool mA2dpSuspended;
    EffectDescriptorCollection mEffects;
    HwModuleCollection mHwModules;
    std::atomic<uint32_t> mAudioPortGeneration;
    AudioPatchCollection mAudioPatches;
    SoundTriggerSessionCollection mSoundTriggerSessions;
    HwAudioOutputCollection mHwOutputs;
    SourceClientCollection mAudioSources;
    enum { STARTING_OUTPUT, STARTING_BEACON, STOPPING_OUTPUT, STOPPING_BEACON };
    uint32_t mBeaconMuteRefCount;
    uint32_t mBeaconPlayingRefCount;
    bool mBeaconMuted;
    bool mTtsOutputAvailable;
    bool mMasterMono;
    AudioPolicyMixCollection mPolicyMixes;
    audio_io_handle_t mMusicEffectOutput;
    uint32_t nextAudioPortGeneration();
    std::unordered_set<audio_format_t> mManualSurroundFormats;
    std::unordered_map<uid_t, audio_flags_mask_t> mAllowedCapturePolicies;
    std::map<wp<DeviceDescriptor>, FormatVector> mReportedFormatsMap;
    product_strategy_t mCommunnicationStrategy;
    sp<SourceClientDescriptor> mCallRxSourceClient;
    sp<SourceClientDescriptor> mCallTxSourceClient;
    std::map<audio_port_handle_t, std::map<product_strategy_t, sp<PreferredMixerAttributesInfo>>>
            mPreferredMixerAttrInfos;
    sp<DeviceDescriptor> getMsdAudioInDevice() const;
    DeviceVector getMsdAudioOutDevices() const;
    const AudioPatchCollection getMsdOutputPatches() const;
    status_t getMsdProfiles(bool hwAvSync, const InputProfileCollection& inputProfiles,
                            const OutputProfileCollection& outputProfiles,
                            const sp<DeviceDescriptor>& sourceDevice,
                            const sp<DeviceDescriptor>& sinkDevice,
                            AudioProfileVector& sourceProfiles,
                            AudioProfileVector& sinkProfiles) const;
    status_t getBestMsdConfig(bool hwAvSync, const AudioProfileVector& sourceProfiles,
                              const AudioProfileVector& sinkProfiles,
                              audio_port_config* sourceConfig, audio_port_config* sinkConfig) const;
    PatchBuilder buildMsdPatch(bool msdIsSource, const sp<DeviceDescriptor>& device) const;
    status_t setMsdOutputPatches(const DeviceVector* outputDevices = nullptr);
    void releaseMsdOutputPatches(const DeviceVector& devices);
    bool msdHasPatchesToAllDevices(const AudioDeviceTypeAddrVector& devices);
    status_t setDeviceConnectionState(audio_devices_t deviceType, audio_policy_dev_state_t state,
                                      const char* device_address, const char* device_name,
                                      audio_format_t encodedFormat);
    status_t deviceToAudioPort(audio_devices_t deviceType, const char* device_address,
                               const char* device_name, media::AudioPortFw* aidPort);
    bool isMsdPatch(const audio_patch_handle_t& handle) const;
  private:
    void onNewAudioModulesAvailableInt(DeviceVector* newDevices);
    void modifySurroundFormats(const sp<DeviceDescriptor>& devDesc, FormatVector* formatsPtr);
    void modifySurroundChannelMasks(ChannelMaskSet* channelMasksPtr);
    void updateAudioProfiles(const sp<DeviceDescriptor>& devDesc, audio_io_handle_t ioHandle,
                             const sp<IOProfile>& profiles);
    void prepareToDisconnectExternalDevice(const sp<DeviceDescriptor>& device);
    void broadcastDeviceConnectionState(const sp<DeviceDescriptor>& device,
                                        media::DeviceConnectedState state);
    void handleNotificationRoutingForStream(audio_stream_type_t stream);
    uint32_t curAudioPortGeneration() const { return mAudioPortGeneration; }
    status_t getAudioAttributes(audio_attributes_t* dstAttr, const audio_attributes_t* srcAttr,
                                audio_stream_type_t srcStream);
    status_t getOutputForAttrInt(audio_attributes_t* resultAttr, audio_io_handle_t* output,
                                 audio_session_t session, const audio_attributes_t* attr,
                                 audio_stream_type_t* stream, uid_t uid, audio_config_t* config,
                                 audio_output_flags_t* flags, audio_port_handle_t* selectedDeviceId,
                                 bool* isRequestedDeviceForExclusiveUse,
                                 std::vector<sp<AudioPolicyMix>>* secondaryMixes,
                                 output_type_t* outputType, bool* isSpatialized,
                                 bool* isBitPerfect);
    audio_io_handle_t getOutputForDevices(
            const DeviceVector& devices, audio_session_t session, const audio_attributes_t* attr,
            const audio_config_t* config, audio_output_flags_t* flags, bool* isSpatialized,
            sp<PreferredMixerAttributesInfo> prefMixerAttrInfo = nullptr,
            bool forceMutingHaptic = false);
    status_t openDirectOutput(audio_stream_type_t stream, audio_session_t session,
                              const audio_config_t* config, audio_output_flags_t flags,
                              const DeviceVector& devices, audio_io_handle_t* output);
    virtual bool canBeSpatializedInt(const audio_attributes_t* attr, const audio_config_t* config,
                                     const AudioDeviceTypeAddrVector& devices) const;
    sp<IOProfile> getSpatializerOutputProfile(const audio_config_t* config,
                                              const AudioDeviceTypeAddrVector& devices) const;
    void checkVirtualizerClientRoutes();
    bool isOutputOnlyAvailableRouteToSomeDevice(const sp<SwAudioOutputDescriptor>& outputDesc);
    audio_io_handle_t getInputForDevice(const sp<DeviceDescriptor>& device, audio_session_t session,
                                        const audio_attributes_t& attributes,
                                        audio_config_base_t* config, audio_input_flags_t flags,
                                        const sp<AudioPolicyMix>& policyMix);
    uint32_t handleEventForBeacon(int event);
    uint32_t setBeaconMute(bool mute);
    bool isValidAttributes(const audio_attributes_t* paa);
    status_t setDeviceConnectionStateInt(audio_policy_dev_state_t state,
                                         const android::media::audio::common::AudioPort& port,
                                         audio_format_t encodedFormat);
    status_t setDeviceConnectionStateInt(audio_devices_t deviceType, audio_policy_dev_state_t state,
                                         const char* device_address, const char* device_name,
                                         audio_format_t encodedFormat);
    status_t setDeviceConnectionStateInt(const sp<DeviceDescriptor>& device,
                                         audio_policy_dev_state_t state);
    void setEngineDeviceConnectionState(const sp<DeviceDescriptor> device,
                                        audio_policy_dev_state_t state);
    void updateMono(audio_io_handle_t output) {
        AudioParameter param;
        param.addInt(String8(AudioParameter::keyMonoOutput), (int)mMasterMono);
        mpClientInterface->setParameters(output, param.toString());
    }
    status_t createAudioPatchInternal(const struct audio_patch* patch, audio_patch_handle_t* handle,
                                      uid_t uid, uint32_t delayMs,
                                      const sp<SourceClientDescriptor>& sourceDesc);
    status_t releaseAudioPatchInternal(audio_patch_handle_t handle, uint32_t delayMs = 0,
                                       const sp<SourceClientDescriptor>& sourceDesc = nullptr);
    status_t installPatch(const char* caller, audio_patch_handle_t* patchHandle,
                          AudioIODescriptorInterface* ioDescriptor, const struct audio_patch* patch,
                          int delayMs);
    status_t installPatch(const char* caller, ssize_t index, audio_patch_handle_t* patchHandle,
                          const struct audio_patch* patch, int delayMs, uid_t uid,
                          sp<AudioPatch>* patchDescPtr);
    bool areAllDevicesSupported(const AudioDeviceTypeAddrVector& devices,
                                std::function<bool(audio_devices_t)> predicate, const char* context,
                                bool matchAddress = true);
    void changeOutputDevicesMuteState(const AudioDeviceTypeAddrVector& devices);
    std::vector<sp<SwAudioOutputDescriptor>> getSoftwareOutputsForDevices(
            const AudioDeviceTypeAddrVector& devices) const;
    bool isScoRequestedForComm() const;
    bool isHearingAidUsedForComm() const;
    bool areAllActiveTracksRerouted(const sp<SwAudioOutputDescriptor>& output);
    sp<SwAudioOutputDescriptor> openOutputWithProfileAndDevice(
            const sp<IOProfile>& profile, const DeviceVector& devices,
            const audio_config_base_t* mixerConfig = nullptr,
            const audio_config_t* halConfig = nullptr,
            audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE);
    bool isOffloadPossible(const audio_offload_info_t& offloadInfo, bool durationIgnored = false);
    void addPortProfilesToVector(sp<IOProfile> outputProfile,
                                 AudioProfileVector& audioProfilesVector);
    sp<IOProfile> searchCompatibleProfileHwModules(const HwModuleCollection& hwModules,
                                                   const DeviceVector& devices,
                                                   uint32_t samplingRate, audio_format_t format,
                                                   audio_channel_mask_t channelMask,
                                                   audio_output_flags_t flags, bool directOnly);
    audio_output_flags_t getRelevantFlags(audio_output_flags_t flags, bool directOnly);
    status_t getDevicesForAttributes(const audio_attributes_t& attr, DeviceVector& devices,
                                     bool forVolume);
    status_t getProfilesForDevices(const DeviceVector& devices, AudioProfileVector& audioProfiles,
                                   uint32_t flags, bool isInput);
    sp<PreferredMixerAttributesInfo> getPreferredMixerAttributesInfo(
            audio_port_handle_t devicePortId, product_strategy_t strategy,
            bool activeBitPerfectPreferred = false);
    sp<SwAudioOutputDescriptor> reopenOutput(sp<SwAudioOutputDescriptor> outputDesc,
                                             const audio_config_t* config,
                                             audio_output_flags_t flags, const char* caller);
    void reopenOutputsWithDevices(const std::map<audio_io_handle_t, DeviceVector>& outputsToReopen);
    PortHandleVector getClientsForStream(audio_stream_type_t streamType) const;
    void invalidateStreams(StreamTypeVector streams) const;
    bool checkHapticCompatibilityOnSpatializerOutput(const audio_config_t* config,
                                                     audio_session_t sessionId) const;
    void updateClientsInternalMute(const sp<SwAudioOutputDescriptor>& desc);
    float adjustDeviceAttenuationForAbsVolume(IVolumeCurves& curves, VolumeSource volumeSource,
                                              int index, const DeviceTypeSet& deviceTypes);
    std::unordered_map<audio_devices_t, audio_attributes_t> mAbsoluteVolumeDrivingStreams;
};
}
