#define LOG_TAG "AudioFlinger"
#define AUDIO_ARRAYS_STATIC_CHECK 1
#include "Configuration.h"
#include <dirent.h>
#include "EffectConfiguration.h"
#include <math.h>
#include <signal.h>
#include <string>
#include <sys/time.h>
#include <media/AudioResamplerPublic.h>
#include <sys/resource.h>
#include "AudioFlinger.h"
#include <afutils/BufLog.h>
#include <android/os/IExternalVibratorService.h>
#include <afutils/DumpTryLock.h>
#include <afutils/Permission.h>
#include <afutils/PropertyUtils.h>
#include <media/audiohal/AudioHalVersionInfo.h>
#include <system/audio.h>
#include <media/nbaio/Pipe.h>
#include <media/nbaio/PipeReader.h>
#include <media/audiohal/DeviceHalInterface.h>
#include <media/audiohal/DevicesFactoryHalInterface.h>
#include <media/audiohal/EffectsFactoryHalInterface.h>
#include <afutils/TypedLogger.h>
#include <android-base/stringprintf.h>
#include <android/media/IAudioPolicyService.h>
#include <private/android_filesystem_config.h>
#include <audiomanager/IAudioManager.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <cutils/properties.h>
#include <system/audio_effects/effect_hapticgenerator.h>
#include <media/AidlConversion.h>
#include <audio_utils/primitives.h>
#include <powermanager/PowerManager.h>
#include <utils/String16.h>
#include <utils/threads.h>
#include <cutils/atomic.h>
#include <media/AudioParameter.h>
#include <media/AudioValidator.h>
#include <media/IMediaLogService.h>
#include <media/MediaMetricsItem.h>
#include <media/TypeConverter.h>
#include <mediautils/BatteryNotifier.h>
#include <mediautils/MemoryLeakTrackUtil.h>
#include <mediautils/MethodStatistics.h>
#include <mediautils/ServiceUtilities.h>
#include <mediautils/TimeCheck.h>
#include <utils/Trace.h>
#include <memunreachable/memunreachable.h>
#include <system/audio_effects/effect_aec.h>
#include <system/audio_effects/effect_ns.h>
#include <system/audio_effects/effect_spatializer.h>
#include <system/audio_effects/effect_visualizer.h>
#include <utils/Log.h>
#include <chrono>
#include <thread>
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif
namespace android {
using ::android::base::StringPrintf;
using media::IEffectClient;
using media::audio::common::AudioMMapPolicyInfo;
using media::audio::common::AudioMMapPolicyType;
using media::audio::common::AudioMode;
using android::content::AttributionSourceState;
using android::detail::AudioHalVersionInfo;
static const AudioHalVersionInfo kMaxAAudioPropertyDeviceHalVersion =
        AudioHalVersionInfo(AudioHalVersionInfo::Type::HIDL, 7, 1);
static constexpr char kDeadlockedString[] = "AudioFlinger may be deadlocked\n";
static constexpr char kHardwareLockedString[] = "Hardware lock is taken\n";
static constexpr char kClientLockedString[] = "Client lock is taken\n";
static constexpr char kNoEffectsFactory[] = "Effects Factory is absent\n";
static constexpr char kAudioServiceName[] = "audio";
static const nsecs_t kMinGlobalEffectEnabletimeNs = seconds(7200);
static sp<IBinder> sMediaLogServiceAsBinder;
static sp<IMediaLogService> sMediaLogService;
static pthread_once_t sMediaLogOnce = PTHREAD_ONCE_INIT;
static void sMediaLogInit()
{
    sMediaLogServiceAsBinder = defaultServiceManager()->getService(String16("media.log"));
    if (sMediaLogServiceAsBinder != 0) {
        sMediaLogService = interface_cast<IMediaLogService>(sMediaLogServiceAsBinder);
    }
}
#define IAUDIOFLINGER_BINDER_METHOD_MACRO_LIST \
BINDER_METHOD_ENTRY(createTrack) \
BINDER_METHOD_ENTRY(createRecord) \
BINDER_METHOD_ENTRY(sampleRate) \
BINDER_METHOD_ENTRY(format) \
BINDER_METHOD_ENTRY(frameCount) \
BINDER_METHOD_ENTRY(latency) \
BINDER_METHOD_ENTRY(setMasterVolume) \
BINDER_METHOD_ENTRY(setMasterMute) \
BINDER_METHOD_ENTRY(masterVolume) \
BINDER_METHOD_ENTRY(masterMute) \
BINDER_METHOD_ENTRY(setStreamVolume) \
BINDER_METHOD_ENTRY(setStreamMute) \
BINDER_METHOD_ENTRY(streamVolume) \
BINDER_METHOD_ENTRY(streamMute) \
BINDER_METHOD_ENTRY(setMode) \
BINDER_METHOD_ENTRY(setMicMute) \
BINDER_METHOD_ENTRY(getMicMute) \
BINDER_METHOD_ENTRY(setRecordSilenced) \
BINDER_METHOD_ENTRY(setParameters) \
BINDER_METHOD_ENTRY(getParameters) \
BINDER_METHOD_ENTRY(registerClient) \
BINDER_METHOD_ENTRY(getInputBufferSize) \
BINDER_METHOD_ENTRY(openOutput) \
BINDER_METHOD_ENTRY(openDuplicateOutput) \
BINDER_METHOD_ENTRY(closeOutput) \
BINDER_METHOD_ENTRY(suspendOutput) \
BINDER_METHOD_ENTRY(restoreOutput) \
BINDER_METHOD_ENTRY(openInput) \
BINDER_METHOD_ENTRY(closeInput) \
BINDER_METHOD_ENTRY(setVoiceVolume) \
BINDER_METHOD_ENTRY(getRenderPosition) \
BINDER_METHOD_ENTRY(getInputFramesLost) \
BINDER_METHOD_ENTRY(newAudioUniqueId) \
BINDER_METHOD_ENTRY(acquireAudioSessionId) \
BINDER_METHOD_ENTRY(releaseAudioSessionId) \
BINDER_METHOD_ENTRY(queryNumberEffects) \
BINDER_METHOD_ENTRY(queryEffect) \
BINDER_METHOD_ENTRY(getEffectDescriptor) \
BINDER_METHOD_ENTRY(createEffect) \
BINDER_METHOD_ENTRY(moveEffects) \
BINDER_METHOD_ENTRY(loadHwModule) \
BINDER_METHOD_ENTRY(getPrimaryOutputSamplingRate) \
BINDER_METHOD_ENTRY(getPrimaryOutputFrameCount) \
BINDER_METHOD_ENTRY(setLowRamDevice) \
BINDER_METHOD_ENTRY(getAudioPort) \
BINDER_METHOD_ENTRY(createAudioPatch) \
BINDER_METHOD_ENTRY(releaseAudioPatch) \
BINDER_METHOD_ENTRY(listAudioPatches) \
BINDER_METHOD_ENTRY(setAudioPortConfig) \
BINDER_METHOD_ENTRY(getAudioHwSyncForSession) \
BINDER_METHOD_ENTRY(systemReady) \
BINDER_METHOD_ENTRY(audioPolicyReady) \
BINDER_METHOD_ENTRY(frameCountHAL) \
BINDER_METHOD_ENTRY(getMicrophones) \
BINDER_METHOD_ENTRY(setMasterBalance) \
BINDER_METHOD_ENTRY(getMasterBalance) \
BINDER_METHOD_ENTRY(setEffectSuspended) \
BINDER_METHOD_ENTRY(setAudioHalPids) \
BINDER_METHOD_ENTRY(setVibratorInfos) \
BINDER_METHOD_ENTRY(updateSecondaryOutputs) \
BINDER_METHOD_ENTRY(getMmapPolicyInfos) \
BINDER_METHOD_ENTRY(getAAudioMixerBurstCount) \
BINDER_METHOD_ENTRY(getAAudioHardwareBurstMinUsec) \
BINDER_METHOD_ENTRY(setDeviceConnectedState) \
BINDER_METHOD_ENTRY(setSimulateDeviceConnections) \
BINDER_METHOD_ENTRY(setRequestedLatencyMode) \
BINDER_METHOD_ENTRY(getSupportedLatencyModes) \
BINDER_METHOD_ENTRY(setBluetoothVariableLatencyEnabled) \
BINDER_METHOD_ENTRY(isBluetoothVariableLatencyEnabled) \
BINDER_METHOD_ENTRY(supportsBluetoothVariableLatency) \
BINDER_METHOD_ENTRY(getSoundDoseInterface) \
BINDER_METHOD_ENTRY(getAudioPolicyConfig) \
static auto& getIAudioFlingerStatistics() {
    using Code = android::AudioFlingerServerAdapter::Delegate::TransactionCode;
       
#undef BINDER_METHOD_ENTRY
#define BINDER_METHOD_ENTRY(ENTRY) \
    {(Code)media::BnAudioFlingerService::TRANSACTION_##ENTRY, #ENTRY},
    static mediautils::MethodStatistics<Code> methodStatistics{
        IAUDIOFLINGER_BINDER_METHOD_MACRO_LIST
        METHOD_STATISTICS_BINDER_CODE_NAMES(Code)
    };
       
#undef BINDER_METHOD_ENTRY
    return methodStatistics;
}
class DevicesFactoryHalCallbackImpl : public DevicesFactoryHalCallback {
  public:
    void onNewDevicesAvailable() override {
        std::thread notifier([]() {
            AudioSystem::onNewAudioModulesAvailable();
        });
        notifier.detach();
    }
};
void AudioFlinger::instantiate() {
    sp<IServiceManager> sm(defaultServiceManager());
    sm->addService(String16(IAudioFlinger::DEFAULT_SERVICE_NAME),
                   new AudioFlingerServerAdapter(new AudioFlinger()), false,
                   IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT);
}
AudioFlinger::AudioFlinger()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t movingBase = (uint32_t)std::max((long)1, ts.tv_sec);
    for (unsigned use = AUDIO_UNIQUE_ID_USE_UNSPECIFIED; use < AUDIO_UNIQUE_ID_USE_MAX; use++) {
        mNextUniqueIds[use] =
                ((use == AUDIO_UNIQUE_ID_USE_SESSION || use == AUDIO_UNIQUE_ID_USE_CLIENT) ?
                        movingBase : 1) * AUDIO_UNIQUE_ID_USE_MAX;
    }
#if 1
    const bool doLog = false;
#else
    const bool doLog = property_get_bool("ro.test_harness", false);
#endif
    if (doLog) {
        mLogMemoryDealer = new MemoryDealer(kLogMemorySize, "LogWriters",
                MemoryHeapBase::READ_ONLY);
        (void) pthread_once(&sMediaLogOnce, sMediaLogInit);
    }
    BatteryNotifier::getInstance().noteResetAudio();
    mMediaLogNotifier->run("MediaLogNotifier");
    std::vector<pid_t> halPids;
    mDevicesFactoryHal->getHalPids(&halPids);
    mediautils::TimeCheck::setAudioHalPids(halPids);
    mediametrics::LogItem(mMetricsId)
        .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_CTOR)
        .record();
}
void AudioFlinger::onFirstRef()
{
    audio_utils::lock_guard _l(mutex());
    mMode = AUDIO_MODE_NORMAL;
    gAudioFlinger = this;
    mDeviceEffectManager = sp<DeviceEffectManager>::make(
            sp<IAfDeviceEffectManagerCallback>::fromExisting(this)),
    mDevicesFactoryHalCallback = new DevicesFactoryHalCallbackImpl;
    mDevicesFactoryHal->setCallbackOnce(mDevicesFactoryHalCallback);
    if (mDevicesFactoryHal->getHalVersion() <= kMaxAAudioPropertyDeviceHalVersion) {
        mAAudioBurstsPerBuffer = getAAudioMixerBurstCountFromSystemProperty();
        mAAudioHwBurstMinMicros = getAAudioHardwareBurstMinUsecFromSystemProperty();
    }
    mPatchPanel = IAfPatchPanel::create(sp<IAfPatchPanelCallback>::fromExisting(this));
    mMelReporter = sp<MelReporter>::make(sp<IAfMelReporterCallback>::fromExisting(this));
}
status_t AudioFlinger::setAudioHalPids(const std::vector<pid_t>& pids) {
  mediautils::TimeCheck::setAudioHalPids(pids);
  return NO_ERROR;
}
status_t AudioFlinger::setVibratorInfos(
        const std::vector<media::AudioVibratorInfo>& vibratorInfos) {
    audio_utils::lock_guard _l(mutex());
    mAudioVibratorInfos = vibratorInfos;
    return NO_ERROR;
}
status_t AudioFlinger::updateSecondaryOutputs(
        const TrackSecondaryOutputsMap& trackSecondaryOutputs) {
    audio_utils::lock_guard _l(mutex());
    for (const auto& [trackId, secondaryOutputs] : trackSecondaryOutputs) {
        size_t i = 0;
        for (; i < mPlaybackThreads.size(); ++i) {
            IAfPlaybackThread* thread = mPlaybackThreads.valueAt(i).get();
            audio_utils::lock_guard _tl(thread->mutex());
            sp<IAfTrack> track = thread->getTrackById_l(trackId);
            if (track != nullptr) {
                ALOGD("%s trackId: %u", __func__, trackId);
                updateSecondaryOutputsForTrack_l(track.get(), thread, secondaryOutputs);
                break;
            }
        }
        ALOGW_IF(i >= mPlaybackThreads.size(),
                 "%s cannot find track with id %u", __func__, trackId);
    }
    return NO_ERROR;
}
status_t AudioFlinger::getMmapPolicyInfos(
            AudioMMapPolicyType policyType, std::vector<AudioMMapPolicyInfo> *policyInfos) {
    audio_utils::lock_guard _l(mutex());
    if (const auto it = mPolicyInfos.find(policyType); it != mPolicyInfos.end()) {
        *policyInfos = it->second;
        return NO_ERROR;
    }
    if (mDevicesFactoryHal->getHalVersion() > kMaxAAudioPropertyDeviceHalVersion) {
        audio_utils::lock_guard lock(hardwareMutex());
        for (size_t i = 0; i < mAudioHwDevs.size(); ++i) {
            AudioHwDevice *dev = mAudioHwDevs.valueAt(i);
            std::vector<AudioMMapPolicyInfo> infos;
            status_t status = dev->getMmapPolicyInfos(policyType, &infos);
            if (status != NO_ERROR) {
                ALOGE("Failed to query mmap policy info of %d, error %d",
                      mAudioHwDevs.keyAt(i), status);
                continue;
            }
            policyInfos->insert(policyInfos->end(), infos.begin(), infos.end());
        }
        mPolicyInfos[policyType] = *policyInfos;
    } else {
        getMmapPolicyInfosFromSystemProperty(policyType, policyInfos);
        mPolicyInfos[policyType] = *policyInfos;
    }
    return NO_ERROR;
}
int32_t AudioFlinger::getAAudioMixerBurstCount() const {
    audio_utils::lock_guard _l(mutex());
    return mAAudioBurstsPerBuffer;
}
int32_t AudioFlinger::getAAudioHardwareBurstMinUsec() const {
    audio_utils::lock_guard _l(mutex());
    return mAAudioHwBurstMinMicros;
}
status_t AudioFlinger::setDeviceConnectedState(const struct audio_port_v7 *port,
                                               media::DeviceConnectedState state) {
    status_t final_result = NO_INIT;
    audio_utils::lock_guard _l(mutex());
    audio_utils::lock_guard lock(hardwareMutex());
    mHardwareStatus = AUDIO_HW_SET_CONNECTED_STATE;
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        sp<DeviceHalInterface> dev = mAudioHwDevs.valueAt(i)->hwDevice();
        status_t result = state == media::DeviceConnectedState::PREPARE_TO_DISCONNECT
                ? dev->prepareToDisconnectExternalDevice(port)
                : dev->setConnectedState(port, state == media::DeviceConnectedState::CONNECTED);
        if (final_result != NO_ERROR) {
            final_result = result;
        }
    }
    mHardwareStatus = AUDIO_HW_IDLE;
    return final_result;
}
status_t AudioFlinger::setSimulateDeviceConnections(bool enabled) {
    bool at_least_one_succeeded = false;
    status_t last_error = INVALID_OPERATION;
    audio_utils::lock_guard _l(mutex());
    audio_utils::lock_guard lock(hardwareMutex());
    mHardwareStatus = AUDIO_HW_SET_SIMULATE_CONNECTIONS;
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        sp<DeviceHalInterface> dev = mAudioHwDevs.valueAt(i)->hwDevice();
        status_t result = dev->setSimulateDeviceConnections(enabled);
        if (result == OK) {
            at_least_one_succeeded = true;
        } else {
            last_error = result;
        }
    }
    mHardwareStatus = AUDIO_HW_IDLE;
    return at_least_one_succeeded ? OK : last_error;
}
std::optional<media::AudioVibratorInfo> AudioFlinger::getDefaultVibratorInfo_l() const {
    if (mAudioVibratorInfos.empty()) {
        return {};
    }
    return mAudioVibratorInfos.front();
}
AudioFlinger::~AudioFlinger()
{
    while (!mRecordThreads.isEmpty()) {
        closeInput_nonvirtual(mRecordThreads.keyAt(0));
    }
    while (!mPlaybackThreads.isEmpty()) {
        closeOutput_nonvirtual(mPlaybackThreads.keyAt(0));
    }
    while (!mMmapThreads.isEmpty()) {
        const audio_io_handle_t io = mMmapThreads.keyAt(0);
        if (mMmapThreads.valueAt(0)->isOutput()) {
            closeOutput_nonvirtual(io);
        } else {
            closeInput_nonvirtual(io);
        }
    }
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        delete mAudioHwDevs.valueAt(i);
    }
    if (sMediaLogService != 0) {
        for (size_t count = mUnregisteredWriters.size(); count > 0; count--) {
            sp<IMemory> iMemory(mUnregisteredWriters.top()->getIMemory());
            mUnregisteredWriters.pop();
            sMediaLogService->unregisterWriter(iMemory);
        }
    }
}
__attribute__ ((visibility ("default")))
status_t MmapStreamInterface::openMmapStream(MmapStreamInterface::stream_direction_t direction,
                                             const audio_attributes_t *attr,
                                             audio_config_base_t *config,
                                             const AudioClient& client,
                                             audio_port_handle_t *deviceId,
                                             audio_session_t *sessionId,
                                             const sp<MmapStreamCallback>& callback,
                                             sp<MmapStreamInterface>& interface,
                                             audio_port_handle_t *handle)
{
    sp<AudioFlinger> af = AudioFlinger::gAudioFlinger.load();
    status_t ret = NO_INIT;
    if (af != 0) {
        ret = af->openMmapStream(
                direction, attr, config, client, deviceId,
                sessionId, callback, interface, handle);
    }
    return ret;
}
status_t AudioFlinger::openMmapStream(MmapStreamInterface::stream_direction_t direction,
                                      const audio_attributes_t *attr,
                                      audio_config_base_t *config,
                                      const AudioClient& client,
                                      audio_port_handle_t *deviceId,
                                      audio_session_t *sessionId,
                                      const sp<MmapStreamCallback>& callback,
                                      sp<MmapStreamInterface>& interface,
                                      audio_port_handle_t *handle)
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return ret;
    }
    audio_session_t actualSessionId = *sessionId;
    if (actualSessionId == AUDIO_SESSION_ALLOCATE) {
        actualSessionId = (audio_session_t) newAudioUniqueId(AUDIO_UNIQUE_ID_USE_SESSION);
    }
    audio_stream_type_t streamType = AUDIO_STREAM_DEFAULT;
    audio_io_handle_t io = AUDIO_IO_HANDLE_NONE;
    audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE;
    audio_attributes_t localAttr = *attr;
    pid_t clientPid =
        VALUE_OR_RETURN_STATUS(aidl2legacy_int32_t_pid_t(client.attributionSource.pid));
    bool updatePid = (clientPid == (pid_t)-1);
    const uid_t callingUid = IPCThreadState::self()->getCallingUid();
    AttributionSourceState adjAttributionSource = client.attributionSource;
    if (!isAudioServerOrMediaServerOrSystemServerOrRootUid(callingUid)) {
        uid_t clientUid =
            VALUE_OR_RETURN_STATUS(aidl2legacy_int32_t_uid_t(client.attributionSource.uid));
        ALOGW_IF(clientUid != callingUid,
                "%s uid %d tried to pass itself off as %d",
                __FUNCTION__, callingUid, clientUid);
        adjAttributionSource.uid = VALUE_OR_RETURN_STATUS(legacy2aidl_uid_t_int32_t(callingUid));
        updatePid = true;
    }
    if (updatePid) {
        const pid_t callingPid = IPCThreadState::self()->getCallingPid();
        ALOGW_IF(clientPid != (pid_t)-1 && clientPid != callingPid,
                 "%s uid %d pid %d tried to pass itself off as pid %d",
                 __func__, callingUid, callingPid, clientPid);
        adjAttributionSource.pid = VALUE_OR_RETURN_STATUS(legacy2aidl_pid_t_int32_t(callingPid));
    }
    adjAttributionSource = afutils::checkAttributionSourcePackage(
            adjAttributionSource);
    if (direction == MmapStreamInterface::DIRECTION_OUTPUT) {
        audio_config_t fullConfig = AUDIO_CONFIG_INITIALIZER;
        fullConfig.sample_rate = config->sample_rate;
        fullConfig.channel_mask = config->channel_mask;
        fullConfig.format = config->format;
        std::vector<audio_io_handle_t> secondaryOutputs;
        bool isSpatialized;
        bool isBitPerfect;
        ret = AudioSystem::getOutputForAttr(&localAttr, &io,
                                            actualSessionId,
                                            &streamType, adjAttributionSource,
                                            &fullConfig,
                                            (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_MMAP_NOIRQ |
                                                    AUDIO_OUTPUT_FLAG_DIRECT),
                                            deviceId, &portId, &secondaryOutputs, &isSpatialized,
                                            &isBitPerfect);
        if (ret != NO_ERROR) {
            config->sample_rate = fullConfig.sample_rate;
            config->channel_mask = fullConfig.channel_mask;
            config->format = fullConfig.format;
        }
        ALOGW_IF(!secondaryOutputs.empty(),
                 "%s does not support secondary outputs, ignoring them", __func__);
    } else {
        ret = AudioSystem::getInputForAttr(&localAttr, &io,
                                              RECORD_RIID_INVALID,
                                              actualSessionId,
                                              adjAttributionSource,
                                              config,
                                              AUDIO_INPUT_FLAG_MMAP_NOIRQ, deviceId, &portId);
    }
    if (ret != NO_ERROR) {
        return ret;
    }
    audio_utils::unique_lock l(mutex());
    const sp<IAfMmapThread> thread = mMmapThreads.valueFor(io);
    if (thread != 0) {
        interface = IAfMmapThread::createMmapStreamInterfaceAdapter(thread);
        thread->configure(&localAttr, streamType, actualSessionId, callback, *deviceId, portId);
        *handle = portId;
        *sessionId = actualSessionId;
        config->sample_rate = thread->sampleRate();
        config->channel_mask = thread->channelMask();
        config->format = thread->format();
    } else {
        l.unlock();
        if (direction == MmapStreamInterface::DIRECTION_OUTPUT) {
            AudioSystem::releaseOutput(portId);
        } else {
            AudioSystem::releaseInput(portId);
        }
        ret = NO_INIT;
    }
    ALOGV("%s done status %d portId %d", __FUNCTION__, ret, portId);
    return ret;
}
status_t AudioFlinger::addEffectToHal(
        const struct audio_port_config *device, const sp<EffectHalInterface>& effect) {
    audio_utils::lock_guard lock(hardwareMutex());
    AudioHwDevice *audioHwDevice = mAudioHwDevs.valueFor(device->ext.device.hw_module);
    if (audioHwDevice == nullptr) {
        return NO_INIT;
    }
    return audioHwDevice->hwDevice()->addDeviceEffect(device, effect);
}
status_t AudioFlinger::removeEffectFromHal(
        const struct audio_port_config *device, const sp<EffectHalInterface>& effect) {
    audio_utils::lock_guard lock(hardwareMutex());
    AudioHwDevice *audioHwDevice = mAudioHwDevs.valueFor(device->ext.device.hw_module);
    if (audioHwDevice == nullptr) {
        return NO_INIT;
    }
    return audioHwDevice->hwDevice()->removeDeviceEffect(device, effect);
}
static const char * const audio_interfaces[] = {
    AUDIO_HARDWARE_MODULE_ID_PRIMARY,
    AUDIO_HARDWARE_MODULE_ID_A2DP,
    AUDIO_HARDWARE_MODULE_ID_USB,
};
AudioHwDevice* AudioFlinger::findSuitableHwDev_l(
        audio_module_handle_t module,
        audio_devices_t deviceType)
{
    audio_utils::lock_guard lock(hardwareMutex());
    if (module == 0) {
        ALOGW("findSuitableHwDev_l() loading well know audio hw modules");
        for (size_t i = 0; i < arraysize(audio_interfaces); i++) {
            loadHwModule_ll(audio_interfaces[i]);
        }
        for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
            AudioHwDevice *audioHwDevice = mAudioHwDevs.valueAt(i);
            sp<DeviceHalInterface> dev = audioHwDevice->hwDevice();
            uint32_t supportedDevices;
            if (dev->getSupportedDevices(&supportedDevices) == OK &&
                    (supportedDevices & deviceType) == deviceType) {
                return audioHwDevice;
            }
        }
    } else {
        AudioHwDevice *audioHwDevice = mAudioHwDevs.valueFor(module);
        if (audioHwDevice != NULL) {
            return audioHwDevice;
        }
    }
    return NULL;
}
void AudioFlinger::dumpClients_ll(int fd, const Vector<String16>& args __unused)
{
    String8 result;
    result.append("Client Allocators:\n");
    for (size_t i = 0; i < mClients.size(); ++i) {
        sp<Client> client = mClients.valueAt(i).promote();
        if (client != 0) {
          result.appendFormat("Client: %d\n", client->pid());
          result.append(client->allocator().dump().c_str());
        }
   }
    result.append("Notification Clients:\n");
    result.append("   pid    uid  name\n");
    for (size_t i = 0; i < mNotificationClients.size(); ++i) {
        const pid_t pid = mNotificationClients[i]->getPid();
        const uid_t uid = mNotificationClients[i]->getUid();
        const mediautils::UidInfo::Info info = mUidInfo.getInfo(uid);
        result.appendFormat("%6d %6u  %s\n", pid, uid, info.package.c_str());
    }
    result.append("Global session refs:\n");
    result.append("  session  cnt     pid    uid  name\n");
    for (size_t i = 0; i < mAudioSessionRefs.size(); i++) {
        AudioSessionRef *r = mAudioSessionRefs[i];
        const mediautils::UidInfo::Info info = mUidInfo.getInfo(r->mUid);
        result.appendFormat("  %7d %4d %7d %6u  %s\n", r->mSessionid, r->mCnt, r->mPid,
                r->mUid, info.package.c_str());
    }
    write(fd, result.c_str(), result.size());
}
void AudioFlinger::dumpInternals_l(int fd, const Vector<String16>& args __unused)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    hardware_call_state hardwareStatus = mHardwareStatus;
    snprintf(buffer, SIZE, "Hardware status: %d\n", hardwareStatus);
    result.append(buffer);
    write(fd, result.c_str(), result.size());
    dprintf(fd, "Vibrator infos(size=%zu):\n", mAudioVibratorInfos.size());
    for (const auto& vibratorInfo : mAudioVibratorInfos) {
        dprintf(fd, "  - %s\n", vibratorInfo.toString().c_str());
    }
    dprintf(fd, "Bluetooth latency modes are %senabled\n",
            mBluetoothLatencyModesEnabled ? "" : "not ");
}
void AudioFlinger::dumpPermissionDenial(int fd, const Vector<String16>& args __unused)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    snprintf(buffer, SIZE, "Permission Denial: "
            "can't dump AudioFlinger from pid=%d, uid=%d\n",
            IPCThreadState::self()->getCallingPid(),
            IPCThreadState::self()->getCallingUid());
    result.append(buffer);
    write(fd, result.c_str(), result.size());
}
status_t AudioFlinger::dump(int fd, const Vector<String16>& args)
NO_THREAD_SAFETY_ANALYSIS
{
    if (!dumpAllowed()) {
        dumpPermissionDenial(fd, args);
    } else {
        const bool hardwareLocked = afutils::dumpTryLock(hardwareMutex());
        if (!hardwareLocked) {
            String8 result(kHardwareLockedString);
            write(fd, result.c_str(), result.size());
        } else {
            hardwareMutex().unlock();
        }
        const bool locked = afutils::dumpTryLock(mutex());
        if (!locked) {
            String8 result(kDeadlockedString);
            write(fd, result.c_str(), result.size());
        }
        const bool clientLocked = afutils::dumpTryLock(clientMutex());
        if (!clientLocked) {
            String8 result(kClientLockedString);
            write(fd, result.c_str(), result.size());
        }
        if (mEffectsFactoryHal != 0) {
            mEffectsFactoryHal->dumpEffects(fd);
        } else {
            String8 result(kNoEffectsFactory);
            write(fd, result.c_str(), result.size());
        }
        dumpClients_ll(fd, args);
        if (clientLocked) {
            clientMutex().unlock();
        }
        dumpInternals_l(fd, args);
        for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
            mPlaybackThreads.valueAt(i)->dump(fd, args);
        }
        for (size_t i = 0; i < mRecordThreads.size(); i++) {
            mRecordThreads.valueAt(i)->dump(fd, args);
        }
        for (size_t i = 0; i < mMmapThreads.size(); i++) {
            mMmapThreads.valueAt(i)->dump(fd, args);
        }
        if (mOrphanEffectChains.size() != 0) {
            write(fd, "  Orphan Effect Chains\n", strlen("  Orphan Effect Chains\n"));
            for (size_t i = 0; i < mOrphanEffectChains.size(); i++) {
                mOrphanEffectChains.valueAt(i)->dump(fd, args);
            }
        }
        for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
            sp<DeviceHalInterface> dev = mAudioHwDevs.valueAt(i)->hwDevice();
            dev->dump(fd, args);
        }
        mPatchPanel->dump(fd);
        mDeviceEffectManager->dump(fd);
        std::string melOutput = mMelReporter->dump();
        write(fd, melOutput.c_str(), melOutput.size());
        auto dumpLogger = [fd](SimpleLog& logger, const char* name) {
            dprintf(fd, "\n%s setParameters:\n", name);
            logger.dump(fd, "    " );
        };
        dumpLogger(mRejectedSetParameterLog, "Rejected");
        dumpLogger(mAppSetParameterLog, "App");
        dumpLogger(mSystemSetParameterLog, "System");
        const std::string threadLog = mThreadLog.dumpToString(
                "Historical Thread Log ", 0 ,
                audio_utils_get_real_time_ns() - 10 * 60 * NANOS_PER_SECOND);
        write(fd, threadLog.c_str(), threadLog.size());
        BUFLOG_RESET;
        if (locked) {
            mutex().unlock();
        }
#ifdef TEE_SINK
        NBAIO_Tee::dumpAll(fd, "_DUMP");
#endif
        if (sMediaLogServiceAsBinder != 0) {
            dprintf(fd, "\nmedia.log:\n");
            sMediaLogServiceAsBinder->dump(fd, args);
        }
        bool dumpMem = false;
        bool unreachableMemory = false;
        for (const auto &arg : args) {
            if (arg == String16("-m")) {
                dumpMem = true;
            } else if (arg == String16("--unreachable")) {
                unreachableMemory = true;
            }
        }
        if (dumpMem) {
            dprintf(fd, "\nDumping memory:\n");
            std::string s = dumpMemoryAddresses(100 );
            write(fd, s.c_str(), s.size());
        }
        if (unreachableMemory) {
            dprintf(fd, "\nDumping unreachable memory:\n");
            std::string s = GetUnreachableMemoryString(true , 100 );
            write(fd, s.c_str(), s.size());
        }
        {
            std::string timeCheckStats = getIAudioFlingerStatistics().dump();
            dprintf(fd, "\nIAudioFlinger binder call profile:\n");
            write(fd, timeCheckStats.c_str(), timeCheckStats.size());
            extern mediautils::MethodStatistics<int>& getIEffectStatistics();
            timeCheckStats = getIEffectStatistics().dump();
            dprintf(fd, "\nIEffect binder call profile:\n");
            write(fd, timeCheckStats.c_str(), timeCheckStats.size());
            std::shared_ptr<std::vector<std::string>> hidlClassNames =
                    mediautils::getStatisticsClassesForModule(
                            METHOD_STATISTICS_MODULE_NAME_AUDIO_HIDL);
            if (hidlClassNames) {
                for (const auto& className : *hidlClassNames) {
                    auto stats = mediautils::getStatisticsForClass(className);
                    if (stats) {
                        timeCheckStats = stats->dump();
                        dprintf(fd, "\n%s binder call profile:\n", className.c_str());
                        write(fd, timeCheckStats.c_str(), timeCheckStats.size());
                    }
                }
            }
            timeCheckStats = mediautils::TimeCheck::toString();
            dprintf(fd, "\nTimeCheck:\n");
            write(fd, timeCheckStats.c_str(), timeCheckStats.size());
            dprintf(fd, "\n");
        }
    }
    return NO_ERROR;
}
sp<Client> AudioFlinger::registerPid(pid_t pid)
{
    audio_utils::lock_guard _cl(clientMutex());
    sp<Client> client = mClients.valueFor(pid).promote();
    if (client == 0) {
        client = sp<Client>::make(sp<IAfClientCallback>::fromExisting(this), pid);
        mClients.add(pid, client);
    }
    return client;
}
sp<NBLog::Writer> AudioFlinger::newWriter_l(size_t size, const char *name)
{
    if (mLogMemoryDealer == 0 || sMediaLogService == 0) {
        return new NBLog::Writer();
    }
    sp<IMemory> shared = mLogMemoryDealer->allocate(NBLog::Timeline::sharedSize(size));
    if (shared == 0) {
        audio_utils::lock_guard _l(unregisteredWritersMutex());
        for (size_t count = mUnregisteredWriters.size(); count > 0; count--) {
            {
                sp<IMemory> iMemory(mUnregisteredWriters[0]->getIMemory());
                mUnregisteredWriters.removeAt(0);
                sMediaLogService->unregisterWriter(iMemory);
            }
            shared = mLogMemoryDealer->allocate(NBLog::Timeline::sharedSize(size));
            if (shared != 0) {
                goto success;
            }
        }
        return new NBLog::Writer();
    }
success:
    NBLog::Shared *sharedRawPtr = (NBLog::Shared *) shared->unsecurePointer();
    new((void *) sharedRawPtr) NBLog::Shared();
    sMediaLogService->registerWriter(shared, size, name);
    return new NBLog::Writer(shared, size);
}
void AudioFlinger::unregisterWriter(const sp<NBLog::Writer>& writer)
{
    if (writer == 0) {
        return;
    }
    sp<IMemory> iMemory(writer->getIMemory());
    if (iMemory == 0) {
        return;
    }
    audio_utils::lock_guard _l(unregisteredWritersMutex());
    mUnregisteredWriters.push(writer);
}
status_t AudioFlinger::createTrack(const media::CreateTrackRequest& _input,
                                   media::CreateTrackResponse& _output)
{
    CreateTrackInput input = VALUE_OR_RETURN_STATUS(CreateTrackInput::fromAidl(_input));
    CreateTrackOutput output;
    sp<IAfTrack> track;
    sp<Client> client;
    status_t lStatus;
    audio_stream_type_t streamType;
    audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE;
    std::vector<audio_io_handle_t> secondaryOutputs;
    bool isSpatialized = false;
    bool isBitPerfect = false;
    pid_t clientPid =
        VALUE_OR_RETURN_STATUS(aidl2legacy_int32_t_pid_t(input.clientInfo.attributionSource.pid));
    bool updatePid = (clientPid == (pid_t)-1);
    const uid_t callingUid = IPCThreadState::self()->getCallingUid();
    uid_t clientUid =
        VALUE_OR_RETURN_STATUS(aidl2legacy_int32_t_uid_t(input.clientInfo.attributionSource.uid));
    audio_io_handle_t effectThreadId = AUDIO_IO_HANDLE_NONE;
    std::vector<int> effectIds;
    audio_attributes_t localAttr = input.attr;
    AttributionSourceState adjAttributionSource = input.clientInfo.attributionSource;
    if (!isAudioServerOrMediaServerOrSystemServerOrRootUid(callingUid)) {
        ALOGW_IF(clientUid != callingUid,
                "%s uid %d tried to pass itself off as %d",
                __FUNCTION__, callingUid, clientUid);
        adjAttributionSource.uid = VALUE_OR_RETURN_STATUS(legacy2aidl_uid_t_int32_t(callingUid));
        clientUid = callingUid;
        updatePid = true;
    }
    const pid_t callingPid = IPCThreadState::self()->getCallingPid();
    if (updatePid) {
        ALOGW_IF(clientPid != (pid_t)-1 && clientPid != callingPid,
                 "%s uid %d pid %d tried to pass itself off as pid %d",
                 __func__, callingUid, callingPid, clientPid);
        clientPid = callingPid;
        adjAttributionSource.pid = VALUE_OR_RETURN_STATUS(legacy2aidl_pid_t_int32_t(callingPid));
    }
    adjAttributionSource = afutils::checkAttributionSourcePackage(
            adjAttributionSource);
    audio_session_t sessionId = input.sessionId;
    if (sessionId == AUDIO_SESSION_ALLOCATE) {
        sessionId = (audio_session_t) newAudioUniqueId(AUDIO_UNIQUE_ID_USE_SESSION);
    } else if (audio_unique_id_get_use(sessionId) != AUDIO_UNIQUE_ID_USE_SESSION) {
        lStatus = BAD_VALUE;
        goto Exit;
    }
    output.sessionId = sessionId;
    output.outputId = AUDIO_IO_HANDLE_NONE;
    output.selectedDeviceId = input.selectedDeviceId;
    lStatus = AudioSystem::getOutputForAttr(&localAttr, &output.outputId, sessionId, &streamType,
                                            adjAttributionSource, &input.config, input.flags,
                                            &output.selectedDeviceId, &portId, &secondaryOutputs,
                                            &isSpatialized, &isBitPerfect);
    if (lStatus != NO_ERROR || output.outputId == AUDIO_IO_HANDLE_NONE) {
        ALOGE("createTrack() getOutputForAttr() return error %d or invalid output handle", lStatus);
        goto Exit;
    }
    if (uint32_t(streamType) >= AUDIO_STREAM_CNT) {
        ALOGE("createTrack() invalid stream type %d", streamType);
        lStatus = BAD_VALUE;
        goto Exit;
    }
    if (!audio_is_output_channel(input.config.channel_mask)) {
        ALOGE("createTrack() invalid channel mask %#x", input.config.channel_mask);
        lStatus = BAD_VALUE;
        goto Exit;
    }
    if (!audio_is_valid_format(input.config.format)) {
        ALOGE("createTrack() invalid format %#x", input.config.format);
        lStatus = BAD_VALUE;
        goto Exit;
    }
    {
        audio_utils::lock_guard _l(mutex());
        IAfPlaybackThread* thread = checkPlaybackThread_l(output.outputId);
        if (thread == NULL) {
            ALOGE("no playback thread found for output handle %d", output.outputId);
            lStatus = BAD_VALUE;
            goto Exit;
        }
        client = registerPid(clientPid);
        IAfPlaybackThread* effectThread = nullptr;
        for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
            sp<IAfPlaybackThread> t = mPlaybackThreads.valueAt(i);
            if (mPlaybackThreads.keyAt(i) != output.outputId) {
                uint32_t sessions = t->hasAudioSession(sessionId);
                if (sessions & IAfThreadBase::EFFECT_SESSION) {
                    effectThread = t.get();
                    break;
                }
            }
        }
        ALOGV("createTrack() sessionId: %d", sessionId);
        output.sampleRate = input.config.sample_rate;
        output.frameCount = input.frameCount;
        output.notificationFrameCount = input.notificationFrameCount;
        output.flags = input.flags;
        output.streamType = streamType;
        track = thread->createTrack_l(client, streamType, localAttr, &output.sampleRate,
                                      input.config.format, input.config.channel_mask,
                                      &output.frameCount, &output.notificationFrameCount,
                                      input.notificationsPerBuffer, input.speed,
                                      input.sharedBuffer, sessionId, &output.flags,
                                      callingPid, adjAttributionSource, input.clientInfo.clientTid,
                                      &lStatus, portId, input.audioTrackCallback, isSpatialized,
                                      isBitPerfect);
        LOG_ALWAYS_FATAL_IF((lStatus == NO_ERROR) && (track == 0));
        output.afFrameCount = thread->frameCount();
        output.afSampleRate = thread->sampleRate();
        output.afChannelMask = static_cast<audio_channel_mask_t>(thread->channelMask() |
                                                                 thread->hapticChannelMask());
        output.afFormat = thread->format();
        output.afLatencyMs = thread->latency();
        output.portId = portId;
        if (lStatus == NO_ERROR) {
            audio_utils::lock_guard _dl(thread->mutex());
            updateSecondaryOutputsForTrack_l(track.get(), thread, secondaryOutputs);
            if (effectThread != nullptr) {
                audio_utils::lock_guard_no_thread_safety_analysis _sl(effectThread->mutex());
                if (moveEffectChain_ll(sessionId, effectThread, thread) == NO_ERROR) {
                    effectThreadId = thread->id();
                    effectIds = thread->getEffectIds_l(sessionId);
                }
            }
        }
        for (auto it = mPendingSyncEvents.begin(); it != mPendingSyncEvents.end();) {
            if ((*it)->triggerSession() == sessionId) {
                if (thread->isValidSyncEvent(*it)) {
                    if (lStatus == NO_ERROR) {
                        (void) track->setSyncEvent(*it);
                    } else {
                        (*it)->cancel();
                    }
                    it = mPendingSyncEvents.erase(it);
                    continue;
                }
            }
            ++it;
        }
        if ((output.flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC) == AUDIO_OUTPUT_FLAG_HW_AV_SYNC) {
            setAudioHwSyncForSession_l(thread, sessionId);
        }
    }
    if (lStatus != NO_ERROR) {
        {
            audio_utils::lock_guard _cl(clientMutex());
            client.clear();
        }
        track.clear();
        goto Exit;
    }
    if (effectThreadId != AUDIO_IO_HANDLE_NONE) {
        AudioSystem::moveEffectsToIo(effectIds, effectThreadId);
    }
    output.audioTrack = IAfTrack::createIAudioTrackAdapter(track);
    _output = VALUE_OR_FATAL(output.toAidl());
Exit:
    if (lStatus != NO_ERROR && output.outputId != AUDIO_IO_HANDLE_NONE) {
        AudioSystem::releaseOutput(portId);
    }
    return lStatus;
}
uint32_t AudioFlinger::sampleRate(audio_io_handle_t ioHandle) const
{
    audio_utils::lock_guard _l(mutex());
    IAfThreadBase* const thread = checkThread_l(ioHandle);
    if (thread == NULL) {
        ALOGW("sampleRate() unknown thread %d", ioHandle);
        return 0;
    }
    return thread->sampleRate();
}
audio_format_t AudioFlinger::format(audio_io_handle_t output) const
{
    audio_utils::lock_guard _l(mutex());
    IAfPlaybackThread* const thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        ALOGW("format() unknown thread %d", output);
        return AUDIO_FORMAT_INVALID;
    }
    return thread->format();
}
size_t AudioFlinger::frameCount(audio_io_handle_t ioHandle) const
{
    audio_utils::lock_guard _l(mutex());
    IAfThreadBase* const thread = checkThread_l(ioHandle);
    if (thread == NULL) {
        ALOGW("frameCount() unknown thread %d", ioHandle);
        return 0;
    }
    return thread->frameCount();
}
size_t AudioFlinger::frameCountHAL(audio_io_handle_t ioHandle) const
{
    audio_utils::lock_guard _l(mutex());
    IAfThreadBase* const thread = checkThread_l(ioHandle);
    if (thread == NULL) {
        ALOGW("frameCountHAL() unknown thread %d", ioHandle);
        return 0;
    }
    return thread->frameCountHAL();
}
uint32_t AudioFlinger::latency(audio_io_handle_t output) const
{
    audio_utils::lock_guard _l(mutex());
    IAfPlaybackThread* const thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        ALOGW("latency(): no playback thread found for output handle %d", output);
        return 0;
    }
    return thread->latency();
}
status_t AudioFlinger::setMasterVolume(float value)
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return ret;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    audio_utils::lock_guard _l(mutex());
    mMasterVolume = value;
    {
        audio_utils::lock_guard lock(hardwareMutex());
        for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
            AudioHwDevice *dev = mAudioHwDevs.valueAt(i);
            mHardwareStatus = AUDIO_HW_SET_MASTER_VOLUME;
            if (dev->canSetMasterVolume()) {
                dev->hwDevice()->setMasterVolume(value);
            }
            mHardwareStatus = AUDIO_HW_IDLE;
        }
    }
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        if (mPlaybackThreads.valueAt(i)->isDuplicating()) {
            continue;
        }
        mPlaybackThreads.valueAt(i)->setMasterVolume(value);
    }
    return NO_ERROR;
}
status_t AudioFlinger::setMasterBalance(float balance)
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return ret;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    if (isnan(balance) || fabs(balance) > 1.f) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard _l(mutex());
    if (mMasterBalance == balance) return NO_ERROR;
    mMasterBalance = balance;
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        if (mPlaybackThreads.valueAt(i)->isDuplicating()) {
            continue;
        }
        mPlaybackThreads.valueAt(i)->setMasterBalance(balance);
    }
    return NO_ERROR;
}
status_t AudioFlinger::setMode(audio_mode_t mode)
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return ret;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    if (uint32_t(mode) >= AUDIO_MODE_CNT) {
        ALOGW("Illegal value: setMode(%d)", mode);
        return BAD_VALUE;
    }
    {
        audio_utils::lock_guard lock(hardwareMutex());
        if (mPrimaryHardwareDev == nullptr) {
            return INVALID_OPERATION;
        }
        sp<DeviceHalInterface> dev = mPrimaryHardwareDev.load()->hwDevice();
        mHardwareStatus = AUDIO_HW_SET_MODE;
        ret = dev->setMode(mode);
        mHardwareStatus = AUDIO_HW_IDLE;
    }
    if (NO_ERROR == ret) {
        audio_utils::lock_guard _l(mutex());
        mMode = mode;
        for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
            mPlaybackThreads.valueAt(i)->setMode(mode);
        }
    }
    mediametrics::LogItem(mMetricsId)
        .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_SETMODE)
        .set(AMEDIAMETRICS_PROP_AUDIOMODE, toString(mode))
        .record();
    return ret;
}
status_t AudioFlinger::setMicMute(bool state)
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return ret;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    audio_utils::lock_guard lock(hardwareMutex());
    if (mPrimaryHardwareDev == nullptr) {
        return INVALID_OPERATION;
    }
    sp<DeviceHalInterface> primaryDev = mPrimaryHardwareDev.load()->hwDevice();
    if (primaryDev == nullptr) {
        ALOGW("%s: no primary HAL device", __func__);
        return INVALID_OPERATION;
    }
    mHardwareStatus = AUDIO_HW_SET_MIC_MUTE;
    ret = primaryDev->setMicMute(state);
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        sp<DeviceHalInterface> dev = mAudioHwDevs.valueAt(i)->hwDevice();
        if (dev != primaryDev) {
            (void)dev->setMicMute(state);
        }
    }
    mHardwareStatus = AUDIO_HW_IDLE;
    ALOGW_IF(ret != NO_ERROR, "%s: error %d setting state to HAL", __func__, ret);
    return ret;
}
bool AudioFlinger::getMicMute() const
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return false;
    }
    audio_utils::lock_guard lock(hardwareMutex());
    if (mPrimaryHardwareDev == nullptr) {
        return false;
    }
    sp<DeviceHalInterface> primaryDev = mPrimaryHardwareDev.load()->hwDevice();
    if (primaryDev == nullptr) {
        ALOGW("%s: no primary HAL device", __func__);
        return false;
    }
    bool state;
    mHardwareStatus = AUDIO_HW_GET_MIC_MUTE;
    ret = primaryDev->getMicMute(&state);
    mHardwareStatus = AUDIO_HW_IDLE;
    ALOGE_IF(ret != NO_ERROR, "%s: error %d getting state from HAL", __func__, ret);
    return (ret == NO_ERROR) && state;
}
void AudioFlinger::setRecordSilenced(audio_port_handle_t portId, bool silenced)
{
    ALOGV("AudioFlinger::setRecordSilenced(portId:%d, silenced:%d)", portId, silenced);
    audio_utils::lock_guard lock(mutex());
    for (size_t i = 0; i < mRecordThreads.size(); i++) {
        mRecordThreads[i]->setRecordSilenced(portId, silenced);
    }
    for (size_t i = 0; i < mMmapThreads.size(); i++) {
        mMmapThreads[i]->setRecordSilenced(portId, silenced);
    }
}
status_t AudioFlinger::setMasterMute(bool muted)
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return ret;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    audio_utils::lock_guard _l(mutex());
    mMasterMute = muted;
    {
        audio_utils::lock_guard lock(hardwareMutex());
        for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
            AudioHwDevice *dev = mAudioHwDevs.valueAt(i);
            mHardwareStatus = AUDIO_HW_SET_MASTER_MUTE;
            if (dev->canSetMasterMute()) {
                dev->hwDevice()->setMasterMute(muted);
            }
            mHardwareStatus = AUDIO_HW_IDLE;
        }
    }
    std::vector<sp<VolumeInterface>> volumeInterfaces = getAllVolumeInterfaces_l();
    for (size_t i = 0; i < volumeInterfaces.size(); i++) {
        volumeInterfaces[i]->setMasterMute(muted);
    }
    return NO_ERROR;
}
float AudioFlinger::masterVolume() const
{
    audio_utils::lock_guard _l(mutex());
    return masterVolume_l();
}
status_t AudioFlinger::getMasterBalance(float *balance) const
{
    audio_utils::lock_guard _l(mutex());
    *balance = getMasterBalance_l();
    return NO_ERROR;
}
bool AudioFlinger::masterMute() const
{
    audio_utils::lock_guard _l(mutex());
    return masterMute_l();
}
float AudioFlinger::masterVolume_l() const
{
    return mMasterVolume;
}
float AudioFlinger::getMasterBalance_l() const
{
    return mMasterBalance;
}
bool AudioFlinger::masterMute_l() const
{
    return mMasterMute;
}
status_t AudioFlinger::checkStreamType(audio_stream_type_t stream)
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        ALOGW("checkStreamType() invalid stream %d", stream);
        return BAD_VALUE;
    }
    const uid_t callerUid = IPCThreadState::self()->getCallingUid();
    if (uint32_t(stream) >= AUDIO_STREAM_PUBLIC_CNT && !isAudioServerUid(callerUid)) {
        ALOGW("checkStreamType() uid %d cannot use internal stream type %d", callerUid, stream);
        return PERMISSION_DENIED;
    }
    return NO_ERROR;
}
status_t AudioFlinger::setStreamVolume(audio_stream_type_t stream, float value,
        audio_io_handle_t output)
{
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    status_t status = checkStreamType(stream);
    if (status != NO_ERROR) {
        return status;
    }
    if (output == AUDIO_IO_HANDLE_NONE) {
        return BAD_VALUE;
    }
    LOG_ALWAYS_FATAL_IF(stream == AUDIO_STREAM_PATCH && value != 1.0f,
                        "AUDIO_STREAM_PATCH must have full scale volume");
    audio_utils::lock_guard lock(mutex());
    sp<VolumeInterface> volumeInterface = getVolumeInterface_l(output);
    if (volumeInterface == NULL) {
        return BAD_VALUE;
    }
    volumeInterface->setStreamVolume(stream, value);
    return NO_ERROR;
}
status_t AudioFlinger::setRequestedLatencyMode(
        audio_io_handle_t output, audio_latency_mode_t mode) {
    if (output == AUDIO_IO_HANDLE_NONE) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard lock(mutex());
    IAfPlaybackThread* const thread = checkPlaybackThread_l(output);
    if (thread == nullptr) {
        return BAD_VALUE;
    }
    return thread->setRequestedLatencyMode(mode);
}
status_t AudioFlinger::getSupportedLatencyModes(audio_io_handle_t output,
            std::vector<audio_latency_mode_t>* modes) const {
    if (output == AUDIO_IO_HANDLE_NONE) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard lock(mutex());
    IAfPlaybackThread* const thread = checkPlaybackThread_l(output);
    if (thread == nullptr) {
        return BAD_VALUE;
    }
    return thread->getSupportedLatencyModes(modes);
}
status_t AudioFlinger::setBluetoothVariableLatencyEnabled(bool enabled) {
    audio_utils::lock_guard _l(mutex());
    status_t status = INVALID_OPERATION;
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        if (mPlaybackThreads.valueAt(i)->setBluetoothVariableLatencyEnabled(enabled) == NO_ERROR) {
            status = NO_ERROR;
        }
    }
    if (status == NO_ERROR) {
        mBluetoothLatencyModesEnabled.store(enabled);
    }
    return status;
}
status_t AudioFlinger::isBluetoothVariableLatencyEnabled(bool* enabled) const {
    if (enabled == nullptr) {
        return BAD_VALUE;
    }
    *enabled = mBluetoothLatencyModesEnabled.load();
    return NO_ERROR;
}
status_t AudioFlinger::supportsBluetoothVariableLatency(bool* support) const {
    if (support == nullptr) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard _l(hardwareMutex());
    *support = false;
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        if (mAudioHwDevs.valueAt(i)->supportsBluetoothVariableLatency()) {
             *support = true;
             break;
        }
    }
    return NO_ERROR;
}
status_t AudioFlinger::getSoundDoseInterface(const sp<media::ISoundDoseCallback>& callback,
                                             sp<media::ISoundDose>* soundDose) const {
    if (soundDose == nullptr) {
        return BAD_VALUE;
    }
    *soundDose = mMelReporter->getSoundDoseInterface(callback);
    return NO_ERROR;
}
status_t AudioFlinger::setStreamMute(audio_stream_type_t stream, bool muted)
{
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    status_t status = checkStreamType(stream);
    if (status != NO_ERROR) {
        return status;
    }
    ALOG_ASSERT(stream != AUDIO_STREAM_PATCH, "attempt to mute AUDIO_STREAM_PATCH");
    if (uint32_t(stream) == AUDIO_STREAM_ENFORCED_AUDIBLE) {
        ALOGE("setStreamMute() invalid stream %d", stream);
        return BAD_VALUE;
    }
    audio_utils::lock_guard lock(mutex());
    mStreamTypes[stream].mute = muted;
    std::vector<sp<VolumeInterface>> volumeInterfaces = getAllVolumeInterfaces_l();
    for (size_t i = 0; i < volumeInterfaces.size(); i++) {
        volumeInterfaces[i]->setStreamMute(stream, muted);
    }
    return NO_ERROR;
}
float AudioFlinger::streamVolume(audio_stream_type_t stream, audio_io_handle_t output) const
{
    status_t status = checkStreamType(stream);
    if (status != NO_ERROR) {
        return 0.0f;
    }
    if (output == AUDIO_IO_HANDLE_NONE) {
        return 0.0f;
    }
    audio_utils::lock_guard lock(mutex());
    sp<VolumeInterface> volumeInterface = getVolumeInterface_l(output);
    if (volumeInterface == NULL) {
        return 0.0f;
    }
    return volumeInterface->streamVolume(stream);
}
bool AudioFlinger::streamMute(audio_stream_type_t stream) const
{
    status_t status = checkStreamType(stream);
    if (status != NO_ERROR) {
        return true;
    }
    audio_utils::lock_guard lock(mutex());
    return streamMute_l(stream);
}
void AudioFlinger::broadcastParametersToRecordThreads_l(const String8& keyValuePairs)
{
    for (size_t i = 0; i < mRecordThreads.size(); i++) {
        mRecordThreads.valueAt(i)->setParameters(keyValuePairs);
    }
}
void AudioFlinger::updateOutDevicesForRecordThreads_l(const DeviceDescriptorBaseVector& devices)
{
    for (size_t i = 0; i < mRecordThreads.size(); i++) {
        mRecordThreads.valueAt(i)->updateOutDevices(devices);
    }
}
void AudioFlinger::forwardParametersToDownstreamPatches_l(
        audio_io_handle_t upStream, const String8& keyValuePairs,
        const std::function<bool(const sp<IAfPlaybackThread>&)>& useThread)
{
    std::vector<SoftwarePatch> swPatches;
    if (mPatchPanel->getDownstreamSoftwarePatches(upStream, &swPatches) != OK) return;
    ALOGV_IF(!swPatches.empty(), "%s found %zu downstream patches for stream ID %d",
            __func__, swPatches.size(), upStream);
    for (const auto& swPatch : swPatches) {
        const sp<IAfPlaybackThread> downStream =
                checkPlaybackThread_l(swPatch.getPlaybackThreadHandle());
        if (downStream != NULL && (useThread == nullptr || useThread(downStream))) {
            downStream->setParameters(keyValuePairs);
        }
    }
}
void AudioFlinger::updateDownStreamPatches_l(const struct audio_patch *patch,
                                             const std::set<audio_io_handle_t>& streams)
{
    for (const audio_io_handle_t stream : streams) {
        IAfPlaybackThread* const playbackThread = checkPlaybackThread_l(stream);
        if (playbackThread == nullptr || !playbackThread->isMsdDevice()) {
            continue;
        }
        playbackThread->setDownStreamPatch(patch);
        playbackThread->sendIoConfigEvent(AUDIO_OUTPUT_CONFIG_CHANGED);
    }
}
void AudioFlinger::filterReservedParameters(String8& keyValuePairs, uid_t callingUid)
{
    static const String8 kReservedParameters[] = {
        String8(AudioParameter::keyRouting),
        String8(AudioParameter::keySamplingRate),
        String8(AudioParameter::keyFormat),
        String8(AudioParameter::keyChannels),
        String8(AudioParameter::keyFrameCount),
        String8(AudioParameter::keyInputSource),
        String8(AudioParameter::keyMonoOutput),
        String8(AudioParameter::keyDeviceConnect),
        String8(AudioParameter::keyDeviceDisconnect),
        String8(AudioParameter::keyStreamSupportedFormats),
        String8(AudioParameter::keyStreamSupportedChannels),
        String8(AudioParameter::keyStreamSupportedSamplingRates),
        String8(AudioParameter::keyClosing),
        String8(AudioParameter::keyExiting),
    };
    if (isAudioServerUid(callingUid)) {
        return;
    }
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 value;
    AudioParameter rejectedParam;
    for (auto& key : kReservedParameters) {
        if (param.get(key, value) == NO_ERROR) {
            rejectedParam.add(key, value);
            param.remove(key);
        }
    }
    logFilteredParameters(param.size() + rejectedParam.size(), keyValuePairs,
                          rejectedParam.size(), rejectedParam.toString(), callingUid);
    keyValuePairs = param.toString();
}
void AudioFlinger::logFilteredParameters(size_t originalKVPSize, const String8& originalKVPs,
                                         size_t rejectedKVPSize, const String8& rejectedKVPs,
                                         uid_t callingUid) {
    auto prefix = String8::format("UID %5d", callingUid);
    auto suffix = String8::format("%zu KVP received: %s", originalKVPSize, originalKVPs.c_str());
    if (rejectedKVPSize != 0) {
        auto error = String8::format("%zu KVP rejected: %s", rejectedKVPSize, rejectedKVPs.c_str());
        ALOGW("%s: %s, %s, %s", __func__, prefix.c_str(), error.c_str(), suffix.c_str());
        mRejectedSetParameterLog.log("%s, %s, %s", prefix.c_str(), error.c_str(), suffix.c_str());
    } else {
        auto& logger = (isServiceUid(callingUid) ? mSystemSetParameterLog : mAppSetParameterLog);
        logger.log("%s, %s", prefix.c_str(), suffix.c_str());
    }
}
status_t AudioFlinger::setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs)
{
    ALOGV("setParameters(): io %d, keyvalue %s, calling pid %d calling uid %d",
            ioHandle, keyValuePairs.c_str(),
            IPCThreadState::self()->getCallingPid(), IPCThreadState::self()->getCallingUid());
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    String8 filteredKeyValuePairs = keyValuePairs;
    filterReservedParameters(filteredKeyValuePairs, IPCThreadState::self()->getCallingUid());
    ALOGV("%s: filtered keyvalue %s", __func__, filteredKeyValuePairs.c_str());
    if (ioHandle == AUDIO_IO_HANDLE_NONE) {
        audio_utils::lock_guard _l(mutex());
        status_t final_result = NO_INIT;
        {
            audio_utils::lock_guard lock(hardwareMutex());
            mHardwareStatus = AUDIO_HW_SET_PARAMETER;
            for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
                sp<DeviceHalInterface> dev = mAudioHwDevs.valueAt(i)->hwDevice();
                status_t result = dev->setParameters(filteredKeyValuePairs);
                if (final_result != NO_ERROR) {
                    final_result = result;
                }
            }
            mHardwareStatus = AUDIO_HW_IDLE;
        }
        AudioParameter param = AudioParameter(filteredKeyValuePairs);
        String8 value;
        if (param.get(String8(AudioParameter::keyBtNrec), value) == NO_ERROR) {
            bool btNrecIsOff = (value == AudioParameter::valueOff);
            if (mBtNrecIsOff.exchange(btNrecIsOff) != btNrecIsOff) {
                for (size_t i = 0; i < mRecordThreads.size(); i++) {
                    mRecordThreads.valueAt(i)->checkBtNrec();
                }
            }
        }
        String8 screenState;
        if (param.get(String8(AudioParameter::keyScreenState), screenState) == NO_ERROR) {
            bool isOff = (screenState == AudioParameter::valueOff);
            if (isOff != (mScreenState & 1)) {
                mScreenState = ((mScreenState & ~1) + 2) | isOff;
            }
        }
        return final_result;
    }
    sp<IAfThreadBase> thread;
    {
        audio_utils::lock_guard _l(mutex());
        thread = checkPlaybackThread_l(ioHandle);
        if (thread == 0) {
            thread = checkRecordThread_l(ioHandle);
            if (thread == 0) {
                thread = checkMmapThread_l(ioHandle);
            }
        } else if (thread == primaryPlaybackThread_l()) {
            AudioParameter param = AudioParameter(filteredKeyValuePairs);
            int value;
            if ((param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) &&
                    (value != 0)) {
                broadcastParametersToRecordThreads_l(filteredKeyValuePairs);
            }
        }
    }
    if (thread != 0) {
        status_t result = thread->setParameters(filteredKeyValuePairs);
        audio_utils::lock_guard _l(mutex());
        forwardParametersToDownstreamPatches_l(thread->id(), filteredKeyValuePairs);
        return result;
    }
    return BAD_VALUE;
}
String8 AudioFlinger::getParameters(audio_io_handle_t ioHandle, const String8& keys) const
{
    ALOGVV("getParameters() io %d, keys %s, calling pid %d",
            ioHandle, keys.c_str(), IPCThreadState::self()->getCallingPid());
    audio_utils::lock_guard _l(mutex());
    if (ioHandle == AUDIO_IO_HANDLE_NONE) {
        String8 out_s8;
        audio_utils::lock_guard lock(hardwareMutex());
        for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
            String8 s;
            mHardwareStatus = AUDIO_HW_GET_PARAMETER;
            sp<DeviceHalInterface> dev = mAudioHwDevs.valueAt(i)->hwDevice();
            status_t result = dev->getParameters(keys, &s);
            mHardwareStatus = AUDIO_HW_IDLE;
            if (result == OK) out_s8 += s;
        }
        return out_s8;
    }
    IAfThreadBase* thread = checkPlaybackThread_l(ioHandle);
    if (thread == NULL) {
        thread = checkRecordThread_l(ioHandle);
        if (thread == NULL) {
            thread = checkMmapThread_l(ioHandle);
            if (thread == NULL) {
                return String8("");
            }
        }
    }
    return thread->getParameters(keys);
}
size_t AudioFlinger::getInputBufferSize(uint32_t sampleRate, audio_format_t format,
        audio_channel_mask_t channelMask) const
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return 0;
    }
    if ((sampleRate == 0) ||
            !audio_is_valid_format(format) || !audio_has_proportional_frames(format) ||
            !audio_is_input_channel(channelMask)) {
        return 0;
    }
    audio_utils::lock_guard lock(hardwareMutex());
    if (mPrimaryHardwareDev == nullptr) {
        return 0;
    }
    mHardwareStatus = AUDIO_HW_GET_INPUT_BUFFER_SIZE;
    sp<DeviceHalInterface> dev = mPrimaryHardwareDev.load()->hwDevice();
    std::vector<audio_channel_mask_t> channelMasks = {channelMask};
    if (channelMask != AUDIO_CHANNEL_IN_MONO) {
        channelMasks.push_back(AUDIO_CHANNEL_IN_MONO);
    }
    if (channelMask != AUDIO_CHANNEL_IN_STEREO) {
        channelMasks.push_back(AUDIO_CHANNEL_IN_STEREO);
    }
    std::vector<audio_format_t> formats = {format};
    if (format != AUDIO_FORMAT_PCM_16_BIT) {
        formats.push_back(AUDIO_FORMAT_PCM_16_BIT);
    }
    std::vector<uint32_t> sampleRates = {sampleRate};
    static const uint32_t SR_44100 = 44100;
    static const uint32_t SR_48000 = 48000;
    if (sampleRate != SR_48000) {
        sampleRates.push_back(SR_48000);
    }
    if (sampleRate != SR_44100) {
        sampleRates.push_back(SR_44100);
    }
    mHardwareStatus = AUDIO_HW_IDLE;
    audio_config_t config = AUDIO_CONFIG_INITIALIZER;
    for (auto testChannelMask : channelMasks) {
        config.channel_mask = testChannelMask;
        for (auto testFormat : formats) {
            config.format = testFormat;
            for (auto testSampleRate : sampleRates) {
                config.sample_rate = testSampleRate;
                size_t bytes = 0;
                status_t result = dev->getInputBufferSize(&config, &bytes);
                if (result != OK || bytes == 0) {
                    continue;
                }
                if (config.sample_rate != sampleRate || config.channel_mask != channelMask ||
                    config.format != format) {
                    uint32_t dstChannelCount = audio_channel_count_from_in_mask(channelMask);
                    uint32_t srcChannelCount =
                        audio_channel_count_from_in_mask(config.channel_mask);
                    size_t srcFrames =
                        bytes / audio_bytes_per_frame(srcChannelCount, config.format);
                    size_t dstFrames = destinationFramesPossible(
                        srcFrames, config.sample_rate, sampleRate);
                    bytes = dstFrames * audio_bytes_per_frame(dstChannelCount, format);
                }
                return bytes;
            }
        }
    }
    ALOGW("getInputBufferSize failed with minimum buffer size sampleRate %u, "
              "format %#x, channelMask %#x",sampleRate, format, channelMask);
    return 0;
}
uint32_t AudioFlinger::getInputFramesLost(audio_io_handle_t ioHandle) const
{
    audio_utils::lock_guard _l(mutex());
    IAfRecordThread* const recordThread = checkRecordThread_l(ioHandle);
    if (recordThread != NULL) {
        return recordThread->getInputFramesLost();
    }
    return 0;
}
status_t AudioFlinger::setVoiceVolume(float value)
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return ret;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    audio_utils::lock_guard lock(hardwareMutex());
    if (mPrimaryHardwareDev == nullptr) {
        return INVALID_OPERATION;
    }
    sp<DeviceHalInterface> dev = mPrimaryHardwareDev.load()->hwDevice();
    mHardwareStatus = AUDIO_HW_SET_VOICE_VOLUME;
    ret = dev->setVoiceVolume(value);
    mHardwareStatus = AUDIO_HW_IDLE;
    mediametrics::LogItem(mMetricsId)
        .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_SETVOICEVOLUME)
        .set(AMEDIAMETRICS_PROP_VOICEVOLUME, (double)value)
        .record();
    return ret;
}
status_t AudioFlinger::getRenderPosition(uint32_t *halFrames, uint32_t *dspFrames,
        audio_io_handle_t output) const
{
    audio_utils::lock_guard _l(mutex());
    IAfPlaybackThread* const playbackThread = checkPlaybackThread_l(output);
    if (playbackThread != NULL) {
        return playbackThread->getRenderPosition(halFrames, dspFrames);
    }
    return BAD_VALUE;
}
void AudioFlinger::registerClient(const sp<media::IAudioFlingerClient>& client)
{
    audio_utils::lock_guard _l(mutex());
    if (client == 0) {
        return;
    }
    pid_t pid = IPCThreadState::self()->getCallingPid();
    const uid_t uid = IPCThreadState::self()->getCallingUid();
    {
        audio_utils::lock_guard _cl(clientMutex());
        if (mNotificationClients.indexOfKey(pid) < 0) {
            sp<NotificationClient> notificationClient = new NotificationClient(this,
                                                                                client,
                                                                                pid,
                                                                                uid);
            ALOGV("registerClient() client %p, pid %d, uid %u",
                    notificationClient.get(), pid, uid);
            mNotificationClients.add(pid, notificationClient);
            sp<IBinder> binder = IInterface::asBinder(client);
            binder->linkToDeath(notificationClient);
        }
    }
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        mPlaybackThreads.valueAt(i)->sendIoConfigEvent(AUDIO_OUTPUT_REGISTERED, pid);
    }
    for (size_t i = 0; i < mRecordThreads.size(); i++) {
        mRecordThreads.valueAt(i)->sendIoConfigEvent(AUDIO_INPUT_REGISTERED, pid);
    }
}
void AudioFlinger::removeNotificationClient(pid_t pid)
{
    std::vector<sp<IAfEffectModule>> removedEffects;
    {
        audio_utils::lock_guard _l(mutex());
        {
            audio_utils::lock_guard _cl(clientMutex());
            mNotificationClients.removeItem(pid);
        }
        ALOGV("%d died, releasing its sessions", pid);
        size_t num = mAudioSessionRefs.size();
        bool removed = false;
        for (size_t i = 0; i < num; ) {
            AudioSessionRef *ref = mAudioSessionRefs.itemAt(i);
            ALOGV(" pid %d @ %zu", ref->mPid, i);
            if (ref->mPid == pid) {
                ALOGV(" removing entry for pid %d session %d", pid, ref->mSessionid);
                mAudioSessionRefs.removeAt(i);
                delete ref;
                removed = true;
                num--;
            } else {
                i++;
            }
        }
        if (removed) {
            removedEffects = purgeStaleEffects_l();
        }
    }
    for (auto& effect : removedEffects) {
        effect->updatePolicyState();
    }
}
void AudioFlinger::ioConfigChanged(audio_io_config_event_t event,
                                   const sp<AudioIoDescriptor>& ioDesc,
                                   pid_t pid) {
    media::AudioIoConfigEvent eventAidl = VALUE_OR_FATAL(
            legacy2aidl_audio_io_config_event_t_AudioIoConfigEvent(event));
    media::AudioIoDescriptor descAidl = VALUE_OR_FATAL(
            legacy2aidl_AudioIoDescriptor_AudioIoDescriptor(ioDesc));
    audio_utils::lock_guard _l(clientMutex());
    size_t size = mNotificationClients.size();
    for (size_t i = 0; i < size; i++) {
        if ((pid == 0) || (mNotificationClients.keyAt(i) == pid)) {
            mNotificationClients.valueAt(i)->audioFlingerClient()->ioConfigChanged(eventAidl,
                                                                                   descAidl);
        }
    }
}
void AudioFlinger::onSupportedLatencyModesChanged(
        audio_io_handle_t output, const std::vector<audio_latency_mode_t>& modes) {
    int32_t outputAidl = VALUE_OR_FATAL(legacy2aidl_audio_io_handle_t_int32_t(output));
    std::vector<media::audio::common::AudioLatencyMode> modesAidl = VALUE_OR_FATAL(
                convertContainer<std::vector<media::audio::common::AudioLatencyMode>>(
                        modes, legacy2aidl_audio_latency_mode_t_AudioLatencyMode));
    audio_utils::lock_guard _l(clientMutex());
    size_t size = mNotificationClients.size();
    for (size_t i = 0; i < size; i++) {
        mNotificationClients.valueAt(i)->audioFlingerClient()
                ->onSupportedLatencyModesChanged(outputAidl, modesAidl);
    }
}
void AudioFlinger::removeClient_l(pid_t pid)
{
    ALOGV("removeClient_l() pid %d, calling pid %d", pid,
            IPCThreadState::self()->getCallingPid());
    mClients.removeItem(pid);
}
sp<IAfThreadBase> AudioFlinger::getEffectThread_l(audio_session_t sessionId,
        int effectId)
{
    sp<IAfThreadBase> thread;
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        if (mPlaybackThreads.valueAt(i)->getEffect(sessionId, effectId) != 0) {
            ALOG_ASSERT(thread == 0);
            thread = mPlaybackThreads.valueAt(i);
        }
    }
    if (thread != nullptr) {
        return thread;
    }
    for (size_t i = 0; i < mRecordThreads.size(); i++) {
        if (mRecordThreads.valueAt(i)->getEffect(sessionId, effectId) != 0) {
            ALOG_ASSERT(thread == 0);
            thread = mRecordThreads.valueAt(i);
        }
    }
    if (thread != nullptr) {
        return thread;
    }
    for (size_t i = 0; i < mMmapThreads.size(); i++) {
        if (mMmapThreads.valueAt(i)->getEffect(sessionId, effectId) != 0) {
            ALOG_ASSERT(thread == 0);
            thread = mMmapThreads.valueAt(i);
        }
    }
    return thread;
}
AudioFlinger::NotificationClient::NotificationClient(const sp<AudioFlinger>& audioFlinger,
                                                     const sp<media::IAudioFlingerClient>& client,
                                                     pid_t pid,
                                                     uid_t uid)
    : mAudioFlinger(audioFlinger), mPid(pid), mUid(uid), mAudioFlingerClient(client)
{
}
AudioFlinger::NotificationClient::~NotificationClient()
{
}
void AudioFlinger::NotificationClient::binderDied(const wp<IBinder>& who __unused)
{
    sp<NotificationClient> keep(this);
    mAudioFlinger->removeNotificationClient(mPid);
}
AudioFlinger::MediaLogNotifier::MediaLogNotifier()
    : mPendingRequests(false) {}
void AudioFlinger::MediaLogNotifier::requestMerge() {
    audio_utils::lock_guard _l(mMutex);
    mPendingRequests = true;
    mCondition.notify_one();
}
bool AudioFlinger::MediaLogNotifier::threadLoop() {
    if (sMediaLogService == 0) {
        return false;
    }
    {
        audio_utils::unique_lock _l(mMutex);
        mPendingRequests = false;
        while (!mPendingRequests) {
            mCondition.wait(_l);
        }
        mPendingRequests = false;
    }
    sMediaLogService->requestMergeWakeup();
    usleep(kPostTriggerSleepPeriod);
    return true;
}
void AudioFlinger::requestLogMerge() {
    mMediaLogNotifier->requestMerge();
}
status_t AudioFlinger::createRecord(const media::CreateRecordRequest& _input,
                                    media::CreateRecordResponse& _output)
{
    CreateRecordInput input = VALUE_OR_RETURN_STATUS(CreateRecordInput::fromAidl(_input));
    CreateRecordOutput output;
    sp<IAfRecordTrack> recordTrack;
    sp<Client> client;
    status_t lStatus;
    audio_session_t sessionId = input.sessionId;
    audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE;
    output.cblk.clear();
    output.buffers.clear();
    output.inputId = AUDIO_IO_HANDLE_NONE;
    AttributionSourceState adjAttributionSource = input.clientInfo.attributionSource;
    bool updatePid = (adjAttributionSource.pid == -1);
    const uid_t callingUid = IPCThreadState::self()->getCallingUid();
    const uid_t currentUid = VALUE_OR_RETURN_STATUS(legacy2aidl_uid_t_int32_t(
           adjAttributionSource.uid));
    if (!isAudioServerOrMediaServerOrSystemServerOrRootUid(callingUid)) {
        ALOGW_IF(currentUid != callingUid,
                "%s uid %d tried to pass itself off as %d",
                __FUNCTION__, callingUid, currentUid);
        adjAttributionSource.uid = VALUE_OR_RETURN_STATUS(legacy2aidl_uid_t_int32_t(callingUid));
        updatePid = true;
    }
    const pid_t callingPid = IPCThreadState::self()->getCallingPid();
    const pid_t currentPid = VALUE_OR_RETURN_STATUS(aidl2legacy_int32_t_pid_t(
            adjAttributionSource.pid));
    if (updatePid) {
        ALOGW_IF(currentPid != (pid_t)-1 && currentPid != callingPid,
                 "%s uid %d pid %d tried to pass itself off as pid %d",
                 __func__, callingUid, callingPid, currentPid);
        adjAttributionSource.pid = VALUE_OR_RETURN_STATUS(legacy2aidl_pid_t_int32_t(callingPid));
    }
    adjAttributionSource = afutils::checkAttributionSourcePackage(
            adjAttributionSource);
    if (!audio_is_valid_format(input.config.format) || !audio_is_linear_pcm(input.config.format)) {
        ALOGE("createRecord() invalid format %#x", input.config.format);
        lStatus = BAD_VALUE;
        goto Exit;
    }
    if (!audio_is_input_channel(input.config.channel_mask)) {
        ALOGE("createRecord() invalid channel mask %#x", input.config.channel_mask);
        lStatus = BAD_VALUE;
        goto Exit;
    }
    if (sessionId == AUDIO_SESSION_ALLOCATE) {
        sessionId = (audio_session_t) newAudioUniqueId(AUDIO_UNIQUE_ID_USE_SESSION);
    } else if (audio_unique_id_get_use(sessionId) != AUDIO_UNIQUE_ID_USE_SESSION) {
        lStatus = BAD_VALUE;
        goto Exit;
    }
    output.sessionId = sessionId;
    output.selectedDeviceId = input.selectedDeviceId;
    output.flags = input.flags;
    client = registerPid(VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(adjAttributionSource.pid)));
    for (;;) {
    if (output.inputId != AUDIO_IO_HANDLE_NONE) {
        recordTrack.clear();
        AudioSystem::releaseInput(portId);
        output.inputId = AUDIO_IO_HANDLE_NONE;
        output.selectedDeviceId = input.selectedDeviceId;
        portId = AUDIO_PORT_HANDLE_NONE;
    }
    lStatus = AudioSystem::getInputForAttr(&input.attr, &output.inputId,
                                      input.riid,
                                      sessionId,
                                      adjAttributionSource,
                                      &input.config,
                                      output.flags, &output.selectedDeviceId, &portId);
    if (lStatus != NO_ERROR) {
        ALOGE("createRecord() getInputForAttr return error %d", lStatus);
        goto Exit;
    }
    {
        audio_utils::lock_guard _l(mutex());
        IAfRecordThread* const thread = checkRecordThread_l(output.inputId);
        if (thread == NULL) {
            ALOGW("createRecord() checkRecordThread_l failed, input handle %d", output.inputId);
            lStatus = FAILED_TRANSACTION;
            goto Exit;
        }
        ALOGV("createRecord() lSessionId: %d input %d", sessionId, output.inputId);
        output.sampleRate = input.config.sample_rate;
        output.frameCount = input.frameCount;
        output.notificationFrameCount = input.notificationFrameCount;
        recordTrack = thread->createRecordTrack_l(client, input.attr, &output.sampleRate,
                                                  input.config.format, input.config.channel_mask,
                                                  &output.frameCount, sessionId,
                                                  &output.notificationFrameCount,
                                                  callingPid, adjAttributionSource, &output.flags,
                                                  input.clientInfo.clientTid,
                                                  &lStatus, portId, input.maxSharedAudioHistoryMs);
        LOG_ALWAYS_FATAL_IF((lStatus == NO_ERROR) && (recordTrack == 0));
        if (lStatus == BAD_TYPE) {
            continue;
        }
        if (lStatus != NO_ERROR) {
            goto Exit;
        }
        if (recordTrack->isFastTrack()) {
            output.serverConfig = {
                    thread->sampleRate(),
                    thread->channelMask(),
                    thread->format()
            };
        } else {
            output.serverConfig = {
                    recordTrack->sampleRate(),
                    recordTrack->channelMask(),
                    recordTrack->format()
            };
        }
        output.halConfig = {
                thread->sampleRate(),
                thread->channelMask(),
                thread->format()
        };
        sp<IAfEffectChain> chain = getOrphanEffectChain_l(sessionId);
        if (chain != 0) {
            audio_utils::lock_guard _l2(thread->mutex());
            thread->addEffectChain_l(chain);
        }
        break;
    }
    }
    output.cblk = recordTrack->getCblk();
    output.buffers = recordTrack->getBuffers();
    output.portId = portId;
    output.audioRecord = IAfRecordTrack::createIAudioRecordAdapter(recordTrack);
    _output = VALUE_OR_FATAL(output.toAidl());
Exit:
    if (lStatus != NO_ERROR) {
        {
            audio_utils::lock_guard _cl(clientMutex());
            client.clear();
        }
        recordTrack.clear();
        if (output.inputId != AUDIO_IO_HANDLE_NONE) {
            AudioSystem::releaseInput(portId);
        }
    }
    return lStatus;
}
status_t AudioFlinger::getAudioPolicyConfig(media::AudioPolicyConfig *config)
{
    if (config == nullptr) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard _l(mutex());
    audio_utils::lock_guard lock(hardwareMutex());
    RETURN_STATUS_IF_ERROR(
            mDevicesFactoryHal->getSurroundSoundConfig(&config->surroundSoundConfig));
    RETURN_STATUS_IF_ERROR(mDevicesFactoryHal->getEngineConfig(&config->engineConfig));
    std::vector<std::string> hwModuleNames;
    RETURN_STATUS_IF_ERROR(mDevicesFactoryHal->getDeviceNames(&hwModuleNames));
    std::set<AudioMode> allSupportedModes;
    for (const auto& name : hwModuleNames) {
        AudioHwDevice* module = loadHwModule_ll(name.c_str());
        if (module == nullptr) continue;
        media::AudioHwModule aidlModule;
        if (module->hwDevice()->getAudioPorts(&aidlModule.ports) == OK &&
                module->hwDevice()->getAudioRoutes(&aidlModule.routes) == OK) {
            aidlModule.handle = module->handle();
            aidlModule.name = module->moduleName();
            config->modules.push_back(std::move(aidlModule));
        }
        std::vector<AudioMode> supportedModes;
        if (module->hwDevice()->getSupportedModes(&supportedModes) == OK) {
            allSupportedModes.insert(supportedModes.begin(), supportedModes.end());
        }
    }
    if (!allSupportedModes.empty()) {
        config->supportedModes.insert(config->supportedModes.end(),
                allSupportedModes.begin(), allSupportedModes.end());
    } else {
        ALOGW("%s: The HAL does not provide telephony functionality", __func__);
        config->supportedModes = { media::audio::common::AudioMode::NORMAL,
            media::audio::common::AudioMode::RINGTONE,
            media::audio::common::AudioMode::IN_CALL,
            media::audio::common::AudioMode::IN_COMMUNICATION };
    }
    return OK;
}
audio_module_handle_t AudioFlinger::loadHwModule(const char *name)
{
    if (name == NULL) {
        return AUDIO_MODULE_HANDLE_NONE;
    }
    if (!settingsAllowed()) {
        return AUDIO_MODULE_HANDLE_NONE;
    }
    audio_utils::lock_guard _l(mutex());
    audio_utils::lock_guard lock(hardwareMutex());
    AudioHwDevice* module = loadHwModule_ll(name);
    return module != nullptr ? module->handle() : AUDIO_MODULE_HANDLE_NONE;
}
AudioHwDevice* AudioFlinger::loadHwModule_ll(const char *name)
{
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        if (strncmp(mAudioHwDevs.valueAt(i)->moduleName(), name, strlen(name)) == 0) {
            ALOGW("loadHwModule() module %s already loaded", name);
            return mAudioHwDevs.valueAt(i);
        }
    }
    sp<DeviceHalInterface> dev;
    int rc = mDevicesFactoryHal->openDevice(name, &dev);
    if (rc) {
        ALOGE("loadHwModule() error %d loading module %s", rc, name);
        return nullptr;
    }
    if (!mMelReporter->activateHalSoundDoseComputation(name, dev)) {
        ALOGW("loadHwModule() sound dose reporting is not available");
    }
    mHardwareStatus = AUDIO_HW_INIT;
    rc = dev->initCheck();
    mHardwareStatus = AUDIO_HW_IDLE;
    if (rc) {
        ALOGE("loadHwModule() init check error %d for module %s", rc, name);
        return nullptr;
    }
    AudioHwDevice::Flags flags = static_cast<AudioHwDevice::Flags>(0);
    if (0 == mAudioHwDevs.size()) {
        mHardwareStatus = AUDIO_HW_GET_MASTER_VOLUME;
        float mv;
        if (OK == dev->getMasterVolume(&mv)) {
            mMasterVolume = mv;
        }
        mHardwareStatus = AUDIO_HW_GET_MASTER_MUTE;
        bool mm;
        if (OK == dev->getMasterMute(&mm)) {
            mMasterMute = mm;
        }
    }
    mHardwareStatus = AUDIO_HW_SET_MASTER_VOLUME;
    if (OK == dev->setMasterVolume(mMasterVolume)) {
        flags = static_cast<AudioHwDevice::Flags>(flags |
                AudioHwDevice::AHWD_CAN_SET_MASTER_VOLUME);
    }
    mHardwareStatus = AUDIO_HW_SET_MASTER_MUTE;
    if (OK == dev->setMasterMute(mMasterMute)) {
        flags = static_cast<AudioHwDevice::Flags>(flags |
                AudioHwDevice::AHWD_CAN_SET_MASTER_MUTE);
    }
    mHardwareStatus = AUDIO_HW_IDLE;
    if (strcmp(name, AUDIO_HARDWARE_MODULE_ID_MSD) == 0) {
        flags = static_cast<AudioHwDevice::Flags>(flags | AudioHwDevice::AHWD_IS_INSERT);
    }
    if (bool supports = false;
            dev->supportsBluetoothVariableLatency(&supports) == NO_ERROR && supports) {
        flags = static_cast<AudioHwDevice::Flags>(flags |
                AudioHwDevice::AHWD_SUPPORTS_BT_LATENCY_MODES);
    }
    audio_module_handle_t handle = (audio_module_handle_t) nextUniqueId(AUDIO_UNIQUE_ID_USE_MODULE);
    AudioHwDevice *audioDevice = new AudioHwDevice(handle, name, dev, flags);
    if (strcmp(name, AUDIO_HARDWARE_MODULE_ID_PRIMARY) == 0) {
        mPrimaryHardwareDev = audioDevice;
        mHardwareStatus = AUDIO_HW_SET_MODE;
        mPrimaryHardwareDev.load()->hwDevice()->setMode(mMode);
        mHardwareStatus = AUDIO_HW_IDLE;
    }
    if (mDevicesFactoryHal->getHalVersion() > kMaxAAudioPropertyDeviceHalVersion) {
        if (int32_t mixerBursts = dev->getAAudioMixerBurstCount();
            mixerBursts > 0 && mixerBursts > mAAudioBurstsPerBuffer) {
            mAAudioBurstsPerBuffer = mixerBursts;
        }
        if (int32_t hwBurstMinMicros = dev->getAAudioHardwareBurstMinUsec();
            hwBurstMinMicros > 0
            && (hwBurstMinMicros < mAAudioHwBurstMinMicros || mAAudioHwBurstMinMicros == 0)) {
            mAAudioHwBurstMinMicros = hwBurstMinMicros;
        }
    }
    mAudioHwDevs.add(handle, audioDevice);
    ALOGI("loadHwModule() Loaded %s audio interface, handle %d", name, handle);
    return audioDevice;
}
uint32_t AudioFlinger::getPrimaryOutputSamplingRate() const
{
    audio_utils::lock_guard _l(mutex());
    IAfPlaybackThread* const thread = fastPlaybackThread_l();
    return thread != NULL ? thread->sampleRate() : 0;
}
size_t AudioFlinger::getPrimaryOutputFrameCount() const
{
    audio_utils::lock_guard _l(mutex());
    IAfPlaybackThread* const thread = fastPlaybackThread_l();
    return thread != NULL ? thread->frameCountHAL() : 0;
}
status_t AudioFlinger::setLowRamDevice(bool isLowRamDevice, int64_t totalMemory)
{
    uid_t uid = IPCThreadState::self()->getCallingUid();
    if (!isAudioServerOrSystemServerUid(uid)) {
        return PERMISSION_DENIED;
    }
    audio_utils::lock_guard _l(mutex());
    if (mIsDeviceTypeKnown) {
        return INVALID_OPERATION;
    }
    mIsLowRamDevice = isLowRamDevice;
    mTotalMemory = totalMemory;
    constexpr int64_t GB = 1024 * 1024 * 1024;
    mClientSharedHeapSize =
            isLowRamDevice ? kMinimumClientSharedHeapSizeBytes
                    : mTotalMemory < 2 * GB ? 4 * kMinimumClientSharedHeapSizeBytes
                    : mTotalMemory < 3 * GB ? 8 * kMinimumClientSharedHeapSizeBytes
                    : mTotalMemory < 4 * GB ? 16 * kMinimumClientSharedHeapSizeBytes
                    : 32 * kMinimumClientSharedHeapSizeBytes;
    mIsDeviceTypeKnown = true;
    ALOGD("isLowRamDevice:%s totalMemory:%lld mClientSharedHeapSize:%zu",
            (isLowRamDevice ? "true" : "false"),
            (long long)mTotalMemory,
            mClientSharedHeapSize.load());
    return NO_ERROR;
}
size_t AudioFlinger::getClientSharedHeapSize() const
{
    size_t heapSizeInBytes = property_get_int32("ro.af.client_heap_size_kbyte", 0) * 1024;
    if (heapSizeInBytes != 0) {
        return heapSizeInBytes;
    }
    return mClientSharedHeapSize;
}
status_t AudioFlinger::setAudioPortConfig(const struct audio_port_config *config)
{
    ALOGV(__func__);
    status_t status = AudioValidator::validateAudioPortConfig(*config);
    if (status != NO_ERROR) {
        return status;
    }
    audio_module_handle_t module;
    if (config->type == AUDIO_PORT_TYPE_DEVICE) {
        module = config->ext.device.hw_module;
    } else {
        module = config->ext.mix.hw_module;
    }
    audio_utils::lock_guard _l(mutex());
    audio_utils::lock_guard lock(hardwareMutex());
    ssize_t index = mAudioHwDevs.indexOfKey(module);
    if (index < 0) {
        ALOGW("%s() bad hw module %d", __func__, module);
        return BAD_VALUE;
    }
    AudioHwDevice *audioHwDevice = mAudioHwDevs.valueAt(index);
    return audioHwDevice->hwDevice()->setAudioPortConfig(config);
}
audio_hw_sync_t AudioFlinger::getAudioHwSyncForSession(audio_session_t sessionId)
{
    audio_utils::lock_guard _l(mutex());
    ssize_t index = mHwAvSyncIds.indexOfKey(sessionId);
    if (index >= 0) {
        ALOGV("getAudioHwSyncForSession found ID %d for session %d",
              mHwAvSyncIds.valueAt(index), sessionId);
        return mHwAvSyncIds.valueAt(index);
    }
    sp<DeviceHalInterface> dev;
    {
        audio_utils::lock_guard lock(hardwareMutex());
        if (mPrimaryHardwareDev == nullptr) {
            return AUDIO_HW_SYNC_INVALID;
        }
        dev = mPrimaryHardwareDev.load()->hwDevice();
    }
    if (dev == nullptr) {
        return AUDIO_HW_SYNC_INVALID;
    }
    error::Result<audio_hw_sync_t> result = dev->getHwAvSync();
    if (!result.ok()) {
        ALOGW("getAudioHwSyncForSession error getting sync for session %d", sessionId);
        return AUDIO_HW_SYNC_INVALID;
    }
    audio_hw_sync_t value = VALUE_OR_FATAL(result);
    for (size_t i = 0; i < mHwAvSyncIds.size(); i++) {
        if (mHwAvSyncIds.valueAt(i) == value) {
            ALOGV("getAudioHwSyncForSession removing ID %d for session %d",
                  value, mHwAvSyncIds.keyAt(i));
            mHwAvSyncIds.removeItemsAt(i);
            break;
        }
    }
    mHwAvSyncIds.add(sessionId, value);
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        const sp<IAfPlaybackThread> thread = mPlaybackThreads.valueAt(i);
        uint32_t sessions = thread->hasAudioSession(sessionId);
        if (sessions & IAfThreadBase::TRACK_SESSION) {
            AudioParameter param = AudioParameter();
            param.addInt(String8(AudioParameter::keyStreamHwAvSync), value);
            String8 keyValuePairs = param.toString();
            thread->setParameters(keyValuePairs);
            forwardParametersToDownstreamPatches_l(thread->id(), keyValuePairs,
                    [](const sp<IAfPlaybackThread>& thread) { return thread->usesHwAvSync(); });
            break;
        }
    }
    ALOGV("getAudioHwSyncForSession adding ID %d for session %d", value, sessionId);
    return (audio_hw_sync_t)value;
}
status_t AudioFlinger::systemReady()
{
    audio_utils::lock_guard _l(mutex());
    ALOGI("%s", __FUNCTION__);
    if (mSystemReady) {
        ALOGW("%s called twice", __FUNCTION__);
        return NO_ERROR;
    }
    mSystemReady = true;
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        IAfThreadBase* const thread = mPlaybackThreads.valueAt(i).get();
        thread->systemReady();
    }
    for (size_t i = 0; i < mRecordThreads.size(); i++) {
        IAfThreadBase* const thread = mRecordThreads.valueAt(i).get();
        thread->systemReady();
    }
    for (size_t i = 0; i < mMmapThreads.size(); i++) {
        IAfThreadBase* const thread = mMmapThreads.valueAt(i).get();
        thread->systemReady();
    }
    getOrCreateAudioManager();
    return NO_ERROR;
}
sp<IAudioManager> AudioFlinger::getOrCreateAudioManager()
{
    if (mAudioManager.load() == nullptr) {
        sp<IBinder> binder =
            defaultServiceManager()->checkService(String16(kAudioServiceName));
        if (binder != nullptr) {
            mAudioManager = interface_cast<IAudioManager>(binder);
        } else {
            ALOGE("%s(): binding to audio service failed.", __func__);
        }
    }
    return mAudioManager.load();
}
status_t AudioFlinger::getMicrophones(std::vector<media::MicrophoneInfoFw>* microphones) const
{
    audio_utils::lock_guard lock(hardwareMutex());
    status_t status = INVALID_OPERATION;
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        std::vector<audio_microphone_characteristic_t> mics;
        AudioHwDevice *dev = mAudioHwDevs.valueAt(i);
        mHardwareStatus = AUDIO_HW_GET_MICROPHONES;
        status_t devStatus = dev->hwDevice()->getMicrophones(&mics);
        mHardwareStatus = AUDIO_HW_IDLE;
        if (devStatus == NO_ERROR) {
            std::transform(mics.begin(), mics.end(), std::back_inserter(*microphones), [](auto& mic)
            {
                auto microphone =
                        legacy2aidl_audio_microphone_characteristic_t_MicrophoneInfoFw(mic);
                return microphone.ok() ? microphone.value() : media::MicrophoneInfoFw{};
            });
            status = NO_ERROR;
        }
    }
    return status;
}
void AudioFlinger::setAudioHwSyncForSession_l(
        IAfPlaybackThread* const thread, audio_session_t sessionId)
{
    ssize_t index = mHwAvSyncIds.indexOfKey(sessionId);
    if (index >= 0) {
        audio_hw_sync_t syncId = mHwAvSyncIds.valueAt(index);
        ALOGV("setAudioHwSyncForSession_l found ID %d for session %d", syncId, sessionId);
        AudioParameter param = AudioParameter();
        param.addInt(String8(AudioParameter::keyStreamHwAvSync), syncId);
        String8 keyValuePairs = param.toString();
        thread->setParameters(keyValuePairs);
        forwardParametersToDownstreamPatches_l(thread->id(), keyValuePairs,
                [](const sp<IAfPlaybackThread>& thread) { return thread->usesHwAvSync(); });
    }
}
sp<IAfThreadBase> AudioFlinger::openOutput_l(audio_module_handle_t module,
                                                        audio_io_handle_t *output,
                                                        audio_config_t *halConfig,
                                                        audio_config_base_t *mixerConfig,
                                                        audio_devices_t deviceType,
                                                        const String8& address,
                                                        audio_output_flags_t flags)
{
    AudioHwDevice *outHwDev = findSuitableHwDev_l(module, deviceType);
    if (outHwDev == NULL) {
        return nullptr;
    }
    if (*output == AUDIO_IO_HANDLE_NONE) {
        *output = nextUniqueId(AUDIO_UNIQUE_ID_USE_OUTPUT);
    } else {
        ALOGE("openOutput_l requested output handle %d is not AUDIO_IO_HANDLE_NONE", *output);
        return nullptr;
    }
    mHardwareStatus = AUDIO_HW_OUTPUT_OPEN;
    AudioStreamOut *outputStream = NULL;
    status_t status = outHwDev->openOutputStream(
            &outputStream,
            *output,
            deviceType,
            flags,
            halConfig,
            address.c_str());
    mHardwareStatus = AUDIO_HW_IDLE;
    if (status == NO_ERROR) {
        if (flags & AUDIO_OUTPUT_FLAG_MMAP_NOIRQ) {
            const sp<IAfMmapPlaybackThread> thread = IAfMmapPlaybackThread::create(
                    this, *output, outHwDev, outputStream, mSystemReady);
            mMmapThreads.add(*output, thread);
            ALOGV("openOutput_l() created mmap playback thread: ID %d thread %p",
                  *output, thread.get());
            return thread;
        } else {
            sp<IAfPlaybackThread> thread;
            if (flags & AUDIO_OUTPUT_FLAG_BIT_PERFECT) {
                thread = IAfPlaybackThread::createBitPerfectThread(
                        this, outputStream, *output, mSystemReady);
                ALOGV("%s() created bit-perfect output: ID %d thread %p",
                      __func__, *output, thread.get());
            } else if (flags & AUDIO_OUTPUT_FLAG_SPATIALIZER) {
                thread = IAfPlaybackThread::createSpatializerThread(this, outputStream, *output,
                                                    mSystemReady, mixerConfig);
                ALOGV("openOutput_l() created spatializer output: ID %d thread %p",
                      *output, thread.get());
            } else if (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
                thread = IAfPlaybackThread::createOffloadThread(this, outputStream, *output,
                        mSystemReady, halConfig->offload_info);
                ALOGV("openOutput_l() created offload output: ID %d thread %p",
                      *output, thread.get());
            } else if ((flags & AUDIO_OUTPUT_FLAG_DIRECT)
                    || !IAfThreadBase::isValidPcmSinkFormat(halConfig->format)
                    || !IAfThreadBase::isValidPcmSinkChannelMask(halConfig->channel_mask)) {
                thread = IAfPlaybackThread::createDirectOutputThread(this, outputStream, *output,
                        mSystemReady, halConfig->offload_info);
                ALOGV("openOutput_l() created direct output: ID %d thread %p",
                      *output, thread.get());
            } else {
                thread = IAfPlaybackThread::createMixerThread(
                        this, outputStream, *output, mSystemReady);
                ALOGV("openOutput_l() created mixer output: ID %d thread %p",
                      *output, thread.get());
            }
            mPlaybackThreads.add(*output, thread);
            struct audio_patch patch;
            mPatchPanel->notifyStreamOpened(outHwDev, *output, &patch);
            if (thread->isMsdDevice()) {
                thread->setDownStreamPatch(&patch);
            }
            thread->setBluetoothVariableLatencyEnabled(mBluetoothLatencyModesEnabled.load());
            return thread;
        }
    }
    return nullptr;
}
status_t AudioFlinger::openOutput(const media::OpenOutputRequest& request,
                                media::OpenOutputResponse* response)
{
    audio_module_handle_t module = VALUE_OR_RETURN_STATUS(
            aidl2legacy_int32_t_audio_module_handle_t(request.module));
    audio_config_t halConfig = VALUE_OR_RETURN_STATUS(
            aidl2legacy_AudioConfig_audio_config_t(request.halConfig, false ));
    audio_config_base_t mixerConfig = VALUE_OR_RETURN_STATUS(
            aidl2legacy_AudioConfigBase_audio_config_base_t(request.mixerConfig, false ));
    sp<DeviceDescriptorBase> device = VALUE_OR_RETURN_STATUS(
            aidl2legacy_DeviceDescriptorBase(request.device));
    audio_output_flags_t flags = VALUE_OR_RETURN_STATUS(
            aidl2legacy_int32_t_audio_output_flags_t_mask(request.flags));
    audio_io_handle_t output;
    ALOGI("openOutput() this %p, module %d Device %s, SamplingRate %d, Format %#08x, "
              "Channels %#x, flags %#x",
              this, module,
              device->toString().c_str(),
              halConfig.sample_rate,
              halConfig.format,
              halConfig.channel_mask,
              flags);
    audio_devices_t deviceType = device->type();
    const String8 address = String8(device->address().c_str());
    if (deviceType == AUDIO_DEVICE_NONE) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard _l(mutex());
    const sp<IAfThreadBase> thread = openOutput_l(module, &output, &halConfig,
            &mixerConfig, deviceType, address, flags);
    if (thread != 0) {
        uint32_t latencyMs = 0;
        if ((flags & AUDIO_OUTPUT_FLAG_MMAP_NOIRQ) == 0) {
            const auto playbackThread = thread->asIAfPlaybackThread();
            latencyMs = playbackThread->latency();
            playbackThread->ioConfigChanged(AUDIO_OUTPUT_OPENED);
            audio_utils::lock_guard lock(hardwareMutex());
            if ((mPrimaryHardwareDev == nullptr) && (flags & AUDIO_OUTPUT_FLAG_PRIMARY)) {
                ALOGI("Using module %d as the primary audio interface", module);
                mPrimaryHardwareDev = playbackThread->getOutput()->audioHwDev;
                mHardwareStatus = AUDIO_HW_SET_MODE;
                mPrimaryHardwareDev.load()->hwDevice()->setMode(mMode);
                mHardwareStatus = AUDIO_HW_IDLE;
            }
        } else {
            thread->ioConfigChanged(AUDIO_OUTPUT_OPENED);
        }
        response->output = VALUE_OR_RETURN_STATUS(legacy2aidl_audio_io_handle_t_int32_t(output));
        response->config = VALUE_OR_RETURN_STATUS(
                legacy2aidl_audio_config_t_AudioConfig(halConfig, false ));
        response->latencyMs = VALUE_OR_RETURN_STATUS(convertIntegral<int32_t>(latencyMs));
        response->flags = VALUE_OR_RETURN_STATUS(
                legacy2aidl_audio_output_flags_t_int32_t_mask(flags));
        return NO_ERROR;
    }
    return NO_INIT;
}
audio_io_handle_t AudioFlinger::openDuplicateOutput(audio_io_handle_t output1,
        audio_io_handle_t output2)
{
    audio_utils::lock_guard _l(mutex());
    IAfPlaybackThread* const thread1 = checkMixerThread_l(output1);
    IAfPlaybackThread* const thread2 = checkMixerThread_l(output2);
    if (thread1 == NULL || thread2 == NULL) {
        ALOGW("openDuplicateOutput() wrong output mixer type for output %d or %d", output1,
                output2);
        return AUDIO_IO_HANDLE_NONE;
    }
    audio_io_handle_t id = nextUniqueId(AUDIO_UNIQUE_ID_USE_OUTPUT);
    const sp<IAfDuplicatingThread> thread = IAfDuplicatingThread::create(
            this, thread1, id, mSystemReady);
    thread->addOutputTrack(thread2);
    mPlaybackThreads.add(id, thread);
    thread->ioConfigChanged(AUDIO_OUTPUT_OPENED);
    return id;
}
status_t AudioFlinger::closeOutput(audio_io_handle_t output)
{
    return closeOutput_nonvirtual(output);
}
status_t AudioFlinger::closeOutput_nonvirtual(audio_io_handle_t output)
{
    sp<IAfPlaybackThread> playbackThread;
    sp<IAfMmapPlaybackThread> mmapThread;
    {
        audio_utils::lock_guard _l(mutex());
        playbackThread = checkPlaybackThread_l(output);
        if (playbackThread != NULL) {
            ALOGV("closeOutput() %d", output);
            dumpToThreadLog_l(playbackThread);
            if (playbackThread->type() == IAfThreadBase::MIXER) {
                for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
                    if (mPlaybackThreads.valueAt(i)->isDuplicating()) {
                        IAfDuplicatingThread* const dupThread =
                                mPlaybackThreads.valueAt(i)->asIAfDuplicatingThread().get();
                        dupThread->removeOutputTrack(playbackThread.get());
                    }
                }
            }
            mPlaybackThreads.removeItem(output);
            if (mPlaybackThreads.size()) {
                IAfPlaybackThread* const dstThread =
                        checkPlaybackThread_l(mPlaybackThreads.keyAt(0));
                if (dstThread != NULL) {
                    audio_utils::scoped_lock sl(dstThread->mutex(), playbackThread->mutex());
                    Vector<sp<IAfEffectChain>> effectChains = playbackThread->getEffectChains_l();
                    for (size_t i = 0; i < effectChains.size(); i ++) {
                        moveEffectChain_ll(effectChains[i]->sessionId(), playbackThread.get(),
                                dstThread);
                    }
                }
            }
        } else {
            const sp<IAfMmapThread> mt = checkMmapThread_l(output);
            mmapThread = mt ? mt->asIAfMmapPlaybackThread().get() : nullptr;
            if (mmapThread == 0) {
                return BAD_VALUE;
            }
            dumpToThreadLog_l(mmapThread);
            mMmapThreads.removeItem(output);
            ALOGD("closing mmapThread %p", mmapThread.get());
        }
        ioConfigChanged(AUDIO_OUTPUT_CLOSED, sp<AudioIoDescriptor>::make(output));
        mPatchPanel->notifyStreamClosed(output);
    }
    if (playbackThread != 0) {
        playbackThread->exit();
        if (!playbackThread->isDuplicating()) {
            closeOutputFinish(playbackThread);
        }
    } else if (mmapThread != 0) {
        ALOGD("mmapThread exit()");
        mmapThread->exit();
        AudioStreamOut *out = mmapThread->clearOutput();
        ALOG_ASSERT(out != NULL, "out shouldn't be NULL");
        delete out;
    }
    return NO_ERROR;
}
void AudioFlinger::closeOutputFinish(const sp<IAfPlaybackThread>& thread)
{
    AudioStreamOut *out = thread->clearOutput();
    ALOG_ASSERT(out != NULL, "out shouldn't be NULL");
    delete out;
}
void AudioFlinger::closeThreadInternal_l(const sp<IAfPlaybackThread>& thread)
{
    mPlaybackThreads.removeItem(thread->id());
    thread->exit();
    closeOutputFinish(thread);
}
status_t AudioFlinger::suspendOutput(audio_io_handle_t output)
{
    audio_utils::lock_guard _l(mutex());
    IAfPlaybackThread* const thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        return BAD_VALUE;
    }
    ALOGV("suspendOutput() %d", output);
    thread->suspend();
    return NO_ERROR;
}
status_t AudioFlinger::restoreOutput(audio_io_handle_t output)
{
    audio_utils::lock_guard _l(mutex());
    IAfPlaybackThread* const thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        return BAD_VALUE;
    }
    ALOGV("restoreOutput() %d", output);
    thread->restore();
    return NO_ERROR;
}
status_t AudioFlinger::openInput(const media::OpenInputRequest& request,
                                 media::OpenInputResponse* response)
{
    audio_utils::lock_guard _l(mutex());
    AudioDeviceTypeAddr device = VALUE_OR_RETURN_STATUS(
            aidl2legacy_AudioDeviceTypeAddress(request.device));
    if (device.mType == AUDIO_DEVICE_NONE) {
        return BAD_VALUE;
    }
    audio_io_handle_t input = VALUE_OR_RETURN_STATUS(
            aidl2legacy_int32_t_audio_io_handle_t(request.input));
    audio_config_t config = VALUE_OR_RETURN_STATUS(
            aidl2legacy_AudioConfig_audio_config_t(request.config, true ));
    const sp<IAfThreadBase> thread = openInput_l(
            VALUE_OR_RETURN_STATUS(aidl2legacy_int32_t_audio_module_handle_t(request.module)),
            &input,
            &config,
            device.mType,
            device.address().c_str(),
            VALUE_OR_RETURN_STATUS(aidl2legacy_AudioSource_audio_source_t(request.source)),
            VALUE_OR_RETURN_STATUS(aidl2legacy_int32_t_audio_input_flags_t_mask(request.flags)),
            AUDIO_DEVICE_NONE,
            String8{});
    response->input = VALUE_OR_RETURN_STATUS(legacy2aidl_audio_io_handle_t_int32_t(input));
    response->config = VALUE_OR_RETURN_STATUS(
            legacy2aidl_audio_config_t_AudioConfig(config, true ));
    response->device = request.device;
    if (thread != 0) {
        thread->ioConfigChanged(AUDIO_INPUT_OPENED);
        return NO_ERROR;
    }
    return NO_INIT;
}
sp<IAfThreadBase> AudioFlinger::openInput_l(audio_module_handle_t module,
                                                         audio_io_handle_t *input,
                                                         audio_config_t *config,
                                                         audio_devices_t devices,
                                                         const char* address,
                                                         audio_source_t source,
                                                         audio_input_flags_t flags,
                                                         audio_devices_t outputDevice,
                                                         const String8& outputDeviceAddress)
{
    AudioHwDevice *inHwDev = findSuitableHwDev_l(module, devices);
    if (inHwDev == NULL) {
        *input = AUDIO_IO_HANDLE_NONE;
        return 0;
    }
    if (*input == AUDIO_IO_HANDLE_NONE) {
        *input = nextUniqueId(AUDIO_UNIQUE_ID_USE_INPUT);
    } else if (audio_unique_id_get_use(*input) != AUDIO_UNIQUE_ID_USE_INPUT) {
        ALOGE("openInput_l() requested input handle %d is invalid", *input);
        return 0;
    } else if (mRecordThreads.indexOfKey(*input) >= 0) {
        ALOGE("openInput_l() requested input handle %d is already assigned", *input);
        return 0;
    }
    audio_config_t halconfig = *config;
    sp<DeviceHalInterface> inHwHal = inHwDev->hwDevice();
    sp<StreamInHalInterface> inStream;
    status_t status = inHwHal->openInputStream(
            *input, devices, &halconfig, flags, address, source,
            outputDevice, outputDeviceAddress, &inStream);
    ALOGV("openInput_l() openInputStream returned input %p, devices %#x, SamplingRate %d"
           ", Format %#x, Channels %#x, flags %#x, status %d addr %s",
            inStream.get(),
            devices,
            halconfig.sample_rate,
            halconfig.format,
            halconfig.channel_mask,
            flags,
            status, address);
    if (status == BAD_VALUE &&
        audio_is_linear_pcm(config->format) &&
        audio_is_linear_pcm(halconfig.format) &&
        (halconfig.sample_rate <= AUDIO_RESAMPLER_DOWN_RATIO_MAX * config->sample_rate) &&
        (audio_channel_count_from_in_mask(halconfig.channel_mask) <= FCC_LIMIT) &&
        (audio_channel_count_from_in_mask(config->channel_mask) <= FCC_LIMIT)) {
        ALOGV("openInput_l() reopening with proposed sampling rate and channel mask");
        inStream.clear();
        status = inHwHal->openInputStream(
                *input, devices, &halconfig, flags, address, source,
                outputDevice, outputDeviceAddress, &inStream);
    }
    if (status == NO_ERROR && inStream != 0) {
        AudioStreamIn *inputStream = new AudioStreamIn(inHwDev, inStream, flags);
        if ((flags & AUDIO_INPUT_FLAG_MMAP_NOIRQ) != 0) {
            const sp<IAfMmapCaptureThread> thread =
                    IAfMmapCaptureThread::create(this, *input, inHwDev, inputStream, mSystemReady);
            mMmapThreads.add(*input, thread);
            ALOGV("openInput_l() created mmap capture thread: ID %d thread %p", *input,
                    thread.get());
            return thread;
        } else {
            const sp<IAfRecordThread> thread =
                    IAfRecordThread::create(this, inputStream, *input, mSystemReady);
            mRecordThreads.add(*input, thread);
            ALOGV("openInput_l() created record thread: ID %d thread %p", *input, thread.get());
            return thread;
        }
    }
    *input = AUDIO_IO_HANDLE_NONE;
    return 0;
}
status_t AudioFlinger::closeInput(audio_io_handle_t input)
{
    return closeInput_nonvirtual(input);
}
status_t AudioFlinger::closeInput_nonvirtual(audio_io_handle_t input)
{
    sp<IAfRecordThread> recordThread;
    sp<IAfMmapCaptureThread> mmapThread;
    {
        audio_utils::lock_guard _l(mutex());
        recordThread = checkRecordThread_l(input);
        if (recordThread != 0) {
            ALOGV("closeInput() %d", input);
            dumpToThreadLog_l(recordThread);
            sp<IAfEffectChain> chain;
            {
                audio_utils::lock_guard _sl(recordThread->mutex());
                const Vector<sp<IAfEffectChain>> effectChains = recordThread->getEffectChains_l();
                if (effectChains.size() != 0) {
                    chain = effectChains[0];
                }
            }
            if (chain != 0) {
                size_t i;
                for (i = 0; i < mRecordThreads.size(); i++) {
                    const sp<IAfRecordThread> t = mRecordThreads.valueAt(i);
                    if (t == recordThread) {
                        continue;
                    }
                    if (t->hasAudioSession(chain->sessionId()) != 0) {
                        audio_utils::lock_guard _l2(t->mutex());
                        ALOGV("closeInput() found thread %d for effect session %d",
                              t->id(), chain->sessionId());
                        t->addEffectChain_l(chain);
                        break;
                    }
                }
                if (i == mRecordThreads.size()) {
                    putOrphanEffectChain_l(chain);
                }
            }
            mRecordThreads.removeItem(input);
        } else {
            const sp<IAfMmapThread> mt = checkMmapThread_l(input);
            mmapThread = mt ? mt->asIAfMmapCaptureThread().get() : nullptr;
            if (mmapThread == 0) {
                return BAD_VALUE;
            }
            dumpToThreadLog_l(mmapThread);
            mMmapThreads.removeItem(input);
        }
        ioConfigChanged(AUDIO_INPUT_CLOSED, sp<AudioIoDescriptor>::make(input));
    }
    if (recordThread != 0) {
        closeInputFinish(recordThread);
    } else if (mmapThread != 0) {
        mmapThread->exit();
        AudioStreamIn *in = mmapThread->clearInput();
        ALOG_ASSERT(in != NULL, "in shouldn't be NULL");
        delete in;
    }
    return NO_ERROR;
}
void AudioFlinger::closeInputFinish(const sp<IAfRecordThread>& thread)
{
    thread->exit();
    AudioStreamIn *in = thread->clearInput();
    ALOG_ASSERT(in != NULL, "in shouldn't be NULL");
    delete in;
}
void AudioFlinger::closeThreadInternal_l(const sp<IAfRecordThread>& thread)
{
    mRecordThreads.removeItem(thread->id());
    closeInputFinish(thread);
}
status_t AudioFlinger::invalidateTracks(const std::vector<audio_port_handle_t> &portIds) {
    audio_utils::lock_guard _l(mutex());
    ALOGV("%s", __func__);
    std::set<audio_port_handle_t> portIdSet(portIds.begin(), portIds.end());
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        IAfPlaybackThread* const thread = mPlaybackThreads.valueAt(i).get();
        thread->invalidateTracks(portIdSet);
        if (portIdSet.empty()) {
            return NO_ERROR;
        }
    }
    for (size_t i = 0; i < mMmapThreads.size(); i++) {
        mMmapThreads[i]->invalidateTracks(portIdSet);
        if (portIdSet.empty()) {
            return NO_ERROR;
        }
    }
    return NO_ERROR;
}
audio_unique_id_t AudioFlinger::newAudioUniqueId(audio_unique_id_use_t use)
{
    if ((unsigned) use >= (unsigned) AUDIO_UNIQUE_ID_USE_MAX) {
        ALOGE("newAudioUniqueId invalid use %d", use);
        return AUDIO_UNIQUE_ID_ALLOCATE;
    }
    return nextUniqueId(use);
}
void AudioFlinger::acquireAudioSessionId(
        audio_session_t audioSession, pid_t pid, uid_t uid)
{
    audio_utils::lock_guard _l(mutex());
    pid_t caller = IPCThreadState::self()->getCallingPid();
    ALOGV("acquiring %d from %d, for %d", audioSession, caller, pid);
    const uid_t callerUid = IPCThreadState::self()->getCallingUid();
    if (pid != (pid_t)-1 && isAudioServerOrMediaServerUid(callerUid)) {
        caller = pid;
    }
    if (uid == (uid_t)-1 || !isAudioServerOrMediaServerUid(callerUid)) {
        uid = callerUid;
    }
    {
        audio_utils::lock_guard _cl(clientMutex());
        if (mNotificationClients.indexOfKey(caller) < 0) {
            ALOGW("acquireAudioSessionId() unknown client %d for session %d", caller, audioSession);
            return;
        }
    }
    size_t num = mAudioSessionRefs.size();
    for (size_t i = 0; i < num; i++) {
        AudioSessionRef *ref = mAudioSessionRefs.editItemAt(i);
        if (ref->mSessionid == audioSession && ref->mPid == caller) {
            ref->mCnt++;
            ALOGV(" incremented refcount to %d", ref->mCnt);
            return;
        }
    }
    mAudioSessionRefs.push(new AudioSessionRef(audioSession, caller, uid));
    ALOGV(" added new entry for %d", audioSession);
}
void AudioFlinger::releaseAudioSessionId(audio_session_t audioSession, pid_t pid)
{
    std::vector<sp<IAfEffectModule>> removedEffects;
    {
        audio_utils::lock_guard _l(mutex());
        pid_t caller = IPCThreadState::self()->getCallingPid();
        ALOGV("releasing %d from %d for %d", audioSession, caller, pid);
        const uid_t callerUid = IPCThreadState::self()->getCallingUid();
        if (pid != (pid_t)-1 && isAudioServerOrMediaServerUid(callerUid)) {
            caller = pid;
        }
        size_t num = mAudioSessionRefs.size();
        for (size_t i = 0; i < num; i++) {
            AudioSessionRef *ref = mAudioSessionRefs.itemAt(i);
            if (ref->mSessionid == audioSession && ref->mPid == caller) {
                ref->mCnt--;
                ALOGV(" decremented refcount to %d", ref->mCnt);
                if (ref->mCnt == 0) {
                    mAudioSessionRefs.removeAt(i);
                    delete ref;
                    std::vector<sp<IAfEffectModule>> effects = purgeStaleEffects_l();
                    removedEffects.insert(removedEffects.end(), effects.begin(), effects.end());
                }
                goto Exit;
            }
        }
        ALOGW_IF(!isAudioServerUid(callerUid),
                 "session id %d not found for pid %d", audioSession, caller);
    }
Exit:
    for (auto& effect : removedEffects) {
        effect->updatePolicyState();
    }
}
bool AudioFlinger::isSessionAcquired_l(audio_session_t audioSession)
{
    size_t num = mAudioSessionRefs.size();
    for (size_t i = 0; i < num; i++) {
        AudioSessionRef *ref = mAudioSessionRefs.itemAt(i);
        if (ref->mSessionid == audioSession) {
            return true;
        }
    }
    return false;
}
std::vector<sp<IAfEffectModule>> AudioFlinger::purgeStaleEffects_l() {
    ALOGV("purging stale effects");
    Vector<sp<IAfEffectChain>> chains;
    std::vector< sp<IAfEffectModule> > removedEffects;
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        sp<IAfPlaybackThread> t = mPlaybackThreads.valueAt(i);
        audio_utils::lock_guard _l(t->mutex());
        const Vector<sp<IAfEffectChain>> threadChains = t->getEffectChains_l();
        for (size_t j = 0; j < threadChains.size(); j++) {
            sp<IAfEffectChain> ec = threadChains[j];
            if (!audio_is_global_session(ec->sessionId())) {
                chains.push(ec);
            }
        }
    }
    for (size_t i = 0; i < mRecordThreads.size(); i++) {
        sp<IAfRecordThread> t = mRecordThreads.valueAt(i);
        audio_utils::lock_guard _l(t->mutex());
        const Vector<sp<IAfEffectChain>> threadChains = t->getEffectChains_l();
        for (size_t j = 0; j < threadChains.size(); j++) {
            sp<IAfEffectChain> ec = threadChains[j];
            chains.push(ec);
        }
    }
    for (size_t i = 0; i < mMmapThreads.size(); i++) {
        const sp<IAfMmapThread> t = mMmapThreads.valueAt(i);
        audio_utils::lock_guard _l(t->mutex());
        const Vector<sp<IAfEffectChain>> threadChains = t->getEffectChains_l();
        for (size_t j = 0; j < threadChains.size(); j++) {
            sp<IAfEffectChain> ec = threadChains[j];
            chains.push(ec);
        }
    }
    for (size_t i = 0; i < chains.size(); i++) {
        sp<IAfEffectChain> ec = chains[i];
        int sessionid = ec->sessionId();
        const auto t = ec->thread().promote();
        if (t == 0) {
            continue;
        }
        size_t numsessionrefs = mAudioSessionRefs.size();
        bool found = false;
        for (size_t k = 0; k < numsessionrefs; k++) {
            AudioSessionRef *ref = mAudioSessionRefs.itemAt(k);
            if (ref->mSessionid == sessionid) {
                ALOGV(" session %d still exists for %d with %d refs",
                    sessionid, ref->mPid, ref->mCnt);
                found = true;
                break;
            }
        }
        if (!found) {
            audio_utils::lock_guard _l(t->mutex());
            while (ec->numberOfEffects()) {
                sp<IAfEffectModule> effect = ec->getEffectModule(0);
                effect->unPin();
                t->removeEffect_l(effect, true);
                if (effect->purgeHandles()) {
                    effect->checkSuspendOnEffectEnabled(false, true );
                }
                removedEffects.push_back(effect);
            }
        }
    }
    return removedEffects;
}
void AudioFlinger::dumpToThreadLog_l(const sp<IAfThreadBase> &thread)
{
    constexpr int THREAD_DUMP_TIMEOUT_MS = 2;
    audio_utils::FdToString fdToString("- ", THREAD_DUMP_TIMEOUT_MS);
    const int fd = fdToString.fd();
    if (fd >= 0) {
        thread->dump(fd, {} );
        mThreadLog.logs(-1 , fdToString.getStringAndClose());
    }
}
IAfThreadBase* AudioFlinger::checkThread_l(audio_io_handle_t ioHandle) const
{
    IAfThreadBase* thread = checkMmapThread_l(ioHandle);
    if (thread == 0) {
        switch (audio_unique_id_get_use(ioHandle)) {
        case AUDIO_UNIQUE_ID_USE_OUTPUT:
            thread = checkPlaybackThread_l(ioHandle);
            break;
        case AUDIO_UNIQUE_ID_USE_INPUT:
            thread = checkRecordThread_l(ioHandle);
            break;
        default:
            break;
        }
    }
    return thread;
}
sp<IAfThreadBase> AudioFlinger::checkOutputThread_l(audio_io_handle_t ioHandle) const
{
    if (audio_unique_id_get_use(ioHandle) != AUDIO_UNIQUE_ID_USE_OUTPUT) {
        return nullptr;
    }
    sp<IAfThreadBase> thread = mPlaybackThreads.valueFor(ioHandle);
    if (thread == nullptr) {
        thread = mMmapThreads.valueFor(ioHandle);
    }
    return thread;
}
IAfPlaybackThread* AudioFlinger::checkPlaybackThread_l(audio_io_handle_t output) const
{
    return mPlaybackThreads.valueFor(output).get();
}
IAfPlaybackThread* AudioFlinger::checkMixerThread_l(audio_io_handle_t output) const
{
    IAfPlaybackThread * const thread = checkPlaybackThread_l(output);
    return thread != nullptr && thread->type() != IAfThreadBase::DIRECT ? thread : nullptr;
}
IAfRecordThread* AudioFlinger::checkRecordThread_l(audio_io_handle_t input) const
{
    return mRecordThreads.valueFor(input).get();
}
IAfMmapThread* AudioFlinger::checkMmapThread_l(audio_io_handle_t io) const
{
    return mMmapThreads.valueFor(io).get();
}
sp<VolumeInterface> AudioFlinger::getVolumeInterface_l(audio_io_handle_t output) const
{
    sp<VolumeInterface> volumeInterface = mPlaybackThreads.valueFor(output).get();
    if (volumeInterface == nullptr) {
        IAfMmapThread* const mmapThread = mMmapThreads.valueFor(output).get();
        if (mmapThread != nullptr) {
            if (mmapThread->isOutput()) {
                IAfMmapPlaybackThread* const mmapPlaybackThread =
                        mmapThread->asIAfMmapPlaybackThread().get();
                volumeInterface = mmapPlaybackThread;
            }
        }
    }
    return volumeInterface;
}
std::vector<sp<VolumeInterface>> AudioFlinger::getAllVolumeInterfaces_l() const
{
    std::vector<sp<VolumeInterface>> volumeInterfaces;
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        volumeInterfaces.push_back(mPlaybackThreads.valueAt(i).get());
    }
    for (size_t i = 0; i < mMmapThreads.size(); i++) {
        if (mMmapThreads.valueAt(i)->isOutput()) {
            IAfMmapPlaybackThread* const mmapPlaybackThread =
                    mMmapThreads.valueAt(i)->asIAfMmapPlaybackThread().get();
            volumeInterfaces.push_back(mmapPlaybackThread);
        }
    }
    return volumeInterfaces;
}
audio_unique_id_t AudioFlinger::nextUniqueId(audio_unique_id_use_t use)
{
    LOG_ALWAYS_FATAL_IF((unsigned) use >= (unsigned) AUDIO_UNIQUE_ID_USE_MAX);
    const int maxRetries = use == AUDIO_UNIQUE_ID_USE_SESSION ? 3 : 1;
    for (int retry = 0; retry < maxRetries; retry++) {
        uint32_t base = (uint32_t) atomic_fetch_add_explicit(&mNextUniqueIds[use],
                (uint_fast32_t) AUDIO_UNIQUE_ID_USE_MAX, memory_order_acq_rel);
        ALOG_ASSERT(audio_unique_id_get_use(base) == AUDIO_UNIQUE_ID_USE_UNSPECIFIED);
        if (!(base == 0 || base == (~0u & ~AUDIO_UNIQUE_ID_USE_MASK))) {
            ALOGW_IF(retry != 0, "unique ID overflow for use %d", use);
            return (audio_unique_id_t) (base | use);
        }
    }
    LOG_ALWAYS_FATAL("unique ID overflow for use %d", use);
}
IAfPlaybackThread* AudioFlinger::primaryPlaybackThread_l() const
{
    audio_utils::lock_guard lock(hardwareMutex());
    if (mPrimaryHardwareDev == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        IAfPlaybackThread* const thread = mPlaybackThreads.valueAt(i).get();
        if(thread->isDuplicating()) {
            continue;
        }
        AudioStreamOut *output = thread->getOutput();
        if (output != NULL && output->audioHwDev == mPrimaryHardwareDev) {
            return thread;
        }
    }
    return nullptr;
}
DeviceTypeSet AudioFlinger::primaryOutputDevice_l() const
{
    IAfPlaybackThread* const thread = primaryPlaybackThread_l();
    if (thread == NULL) {
        return {};
    }
    return thread->outDeviceTypes();
}
IAfPlaybackThread* AudioFlinger::fastPlaybackThread_l() const
{
    size_t minFrameCount = 0;
    IAfPlaybackThread* minThread = nullptr;
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        IAfPlaybackThread* const thread = mPlaybackThreads.valueAt(i).get();
        if (!thread->isDuplicating()) {
            size_t frameCount = thread->frameCountHAL();
            if (frameCount != 0 && (minFrameCount == 0 || frameCount < minFrameCount ||
                    (frameCount == minFrameCount && thread->hasFastMixer() &&
                                             !minThread->hasFastMixer()))) {
                minFrameCount = frameCount;
                minThread = thread;
            }
        }
    }
    return minThread;
}
IAfThreadBase* AudioFlinger::hapticPlaybackThread_l() const {
    for (size_t i = 0; i < mPlaybackThreads.size(); ++i) {
        IAfPlaybackThread* const thread = mPlaybackThreads.valueAt(i).get();
        if (thread->hapticChannelMask() != AUDIO_CHANNEL_NONE) {
            return thread;
        }
    }
    return nullptr;
}
void AudioFlinger::updateSecondaryOutputsForTrack_l(
        IAfTrack* track,
        IAfPlaybackThread* thread,
        const std::vector<audio_io_handle_t> &secondaryOutputs) const {
    TeePatches teePatches;
    for (audio_io_handle_t secondaryOutput : secondaryOutputs) {
        IAfPlaybackThread* const secondaryThread = checkPlaybackThread_l(secondaryOutput);
        if (secondaryThread == nullptr) {
            ALOGE("no playback thread found for secondary output %d", thread->id());
            continue;
        }
        size_t sourceFrameCount = thread->frameCount() * track->sampleRate()
                                  / thread->sampleRate();
        size_t sinkFrameCount = secondaryThread->frameCount() * track->sampleRate()
                                  / secondaryThread->sampleRate();
        size_t frameCountToBeReady = 2 * sinkFrameCount + sourceFrameCount;
        size_t frameCount = frameCountToBeReady + 2 * (sourceFrameCount + sinkFrameCount);
        size_t minFrameCount = AudioSystem::calculateMinFrameCount(
                    [&] { audio_utils::lock_guard _l(secondaryThread->mutex());
                          return secondaryThread->latency_l(); }(),
                    secondaryThread->frameCount(),
                    secondaryThread->sampleRate(),
                    track->sampleRate(),
                    track->getSpeed());
        frameCount = std::max(frameCount, minFrameCount);
        using namespace std::chrono_literals;
        auto inChannelMask = audio_channel_mask_out_to_in(track->channelMask());
        if (inChannelMask == AUDIO_CHANNEL_INVALID) {
            inChannelMask = audio_channel_mask_out_to_in_index_mask(track->channelMask());
        }
        sp<IAfPatchRecord> patchRecord = IAfPatchRecord::create(nullptr ,
                                                       track->sampleRate(),
                                                       inChannelMask,
                                                       track->format(),
                                                       frameCount,
                                                       nullptr ,
                                                       (size_t)0 ,
                                                       AUDIO_INPUT_FLAG_DIRECT,
                                                       0ns );
        status_t status = patchRecord->initCheck();
        if (status != NO_ERROR) {
            ALOGE("Secondary output patchRecord init failed: %d", status);
            continue;
        }
        const audio_output_flags_t outputFlags =
                (audio_output_flags_t)(track->getOutputFlags() & ~AUDIO_OUTPUT_FLAG_FAST);
        sp<IAfPatchTrack> patchTrack = IAfPatchTrack::create(secondaryThread,
                                                       track->streamType(),
                                                       track->sampleRate(),
                                                       track->channelMask(),
                                                       track->format(),
                                                       frameCount,
                                                       patchRecord->buffer(),
                                                       patchRecord->bufferSize(),
                                                       outputFlags,
                                                       0ns ,
                                                       frameCountToBeReady);
        status = patchTrack->initCheck();
        if (status != NO_ERROR) {
            ALOGE("Secondary output patchTrack init failed: %d", status);
            continue;
        }
        teePatches.push_back({patchRecord, patchTrack});
        secondaryThread->addPatchTrack(patchTrack);
        patchTrack->setPeerProxy(patchRecord, true );
        patchRecord->setPeerProxy(patchTrack, false );
    }
    track->setTeePatchesToUpdate(std::move(teePatches));
}
sp<audioflinger::SyncEvent> AudioFlinger::createSyncEvent(AudioSystem::sync_event_t type,
                                    audio_session_t triggerSession,
                                    audio_session_t listenerSession,
                                    const audioflinger::SyncEventCallback& callBack,
                                    const wp<IAfTrackBase>& cookie)
{
    audio_utils::lock_guard _l(mutex());
    auto event = sp<audioflinger::SyncEvent>::make(
            type, triggerSession, listenerSession, callBack, cookie);
    status_t playStatus = NAME_NOT_FOUND;
    status_t recStatus = NAME_NOT_FOUND;
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        playStatus = mPlaybackThreads.valueAt(i)->setSyncEvent(event);
        if (playStatus == NO_ERROR) {
            return event;
        }
    }
    for (size_t i = 0; i < mRecordThreads.size(); i++) {
        recStatus = mRecordThreads.valueAt(i)->setSyncEvent(event);
        if (recStatus == NO_ERROR) {
            return event;
        }
    }
    if (playStatus == NAME_NOT_FOUND || recStatus == NAME_NOT_FOUND) {
        mPendingSyncEvents.emplace_back(event);
    } else {
        ALOGV("createSyncEvent() invalid event %d", event->type());
        event.clear();
    }
    return event;
}
sp<EffectsFactoryHalInterface> AudioFlinger::getEffectsFactory() {
    return mEffectsFactoryHal;
}
status_t AudioFlinger::queryNumberEffects(uint32_t *numEffects) const
{
    audio_utils::lock_guard _l(mutex());
    if (mEffectsFactoryHal.get()) {
        return mEffectsFactoryHal->queryNumberEffects(numEffects);
    } else {
        return -ENODEV;
    }
}
status_t AudioFlinger::queryEffect(uint32_t index, effect_descriptor_t *descriptor) const
{
    audio_utils::lock_guard _l(mutex());
    if (mEffectsFactoryHal.get()) {
        return mEffectsFactoryHal->getDescriptor(index, descriptor);
    } else {
        return -ENODEV;
    }
}
status_t AudioFlinger::getEffectDescriptor(const effect_uuid_t *pUuid,
                                           const effect_uuid_t *pTypeUuid,
                                           uint32_t preferredTypeFlag,
                                           effect_descriptor_t *descriptor) const
{
    if (pUuid == NULL || pTypeUuid == NULL || descriptor == NULL) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard _l(mutex());
    if (!mEffectsFactoryHal.get()) {
        return -ENODEV;
    }
    status_t status = NO_ERROR;
    if (!EffectsFactoryHalInterface::isNullUuid(pUuid)) {
        status = mEffectsFactoryHal->getDescriptor(pUuid, descriptor);
    } else if (!EffectsFactoryHalInterface::isNullUuid(pTypeUuid)) {
        effect_descriptor_t desc;
        desc.flags = 0;
        uint32_t numEffects = 0;
        status = mEffectsFactoryHal->queryNumberEffects(&numEffects);
        if (status < 0) {
            ALOGW("getEffectDescriptor() error %d from FactoryHal queryNumberEffects", status);
            return status;
        }
        bool found = false;
        for (uint32_t i = 0; i < numEffects; i++) {
            status = mEffectsFactoryHal->getDescriptor(i, &desc);
            if (status < 0) {
                ALOGW("getEffectDescriptor() error %d from FactoryHal getDescriptor", status);
                continue;
            }
            if (memcmp(&desc.type, pTypeUuid, sizeof(effect_uuid_t)) == 0) {
                found = true;
                *descriptor = desc;
                if (preferredTypeFlag == EFFECT_FLAG_TYPE_MASK ||
                    (desc.flags & EFFECT_FLAG_TYPE_MASK) == preferredTypeFlag) {
                    break;
                }
            }
        }
        if (!found) {
            status = NAME_NOT_FOUND;
            ALOGW("getEffectDescriptor(): Effect not found by type.");
        }
    } else {
        status = BAD_VALUE;
        ALOGE("getEffectDescriptor(): Either uuid or type uuid must be non-null UUIDs.");
    }
    return status;
}
status_t AudioFlinger::createEffect(const media::CreateEffectRequest& request,
                                    media::CreateEffectResponse* response) {
    const sp<IEffectClient>& effectClient = request.client;
    const int32_t priority = request.priority;
    const AudioDeviceTypeAddr device = VALUE_OR_RETURN_STATUS(
            aidl2legacy_AudioDeviceTypeAddress(request.device));
    AttributionSourceState adjAttributionSource = request.attributionSource;
    const audio_session_t sessionId = VALUE_OR_RETURN_STATUS(
            aidl2legacy_int32_t_audio_session_t(request.sessionId));
    audio_io_handle_t io = VALUE_OR_RETURN_STATUS(
            aidl2legacy_int32_t_audio_io_handle_t(request.output));
    const effect_descriptor_t descIn = VALUE_OR_RETURN_STATUS(
            aidl2legacy_EffectDescriptor_effect_descriptor_t(request.desc));
    const bool probe = request.probe;
    sp<IAfEffectHandle> handle;
    effect_descriptor_t descOut;
    int enabledOut = 0;
    int idOut = -1;
    status_t lStatus = NO_ERROR;
    const uid_t callingUid = IPCThreadState::self()->getCallingUid();
    adjAttributionSource.uid = VALUE_OR_RETURN_STATUS(legacy2aidl_uid_t_int32_t(callingUid));
    pid_t currentPid = VALUE_OR_RETURN_STATUS(aidl2legacy_int32_t_pid_t(adjAttributionSource.pid));
    if (currentPid == -1 || !isAudioServerOrMediaServerOrSystemServerOrRootUid(callingUid)) {
        const pid_t callingPid = IPCThreadState::self()->getCallingPid();
        ALOGW_IF(currentPid != -1 && currentPid != callingPid,
                 "%s uid %d pid %d tried to pass itself off as pid %d",
                 __func__, callingUid, callingPid, currentPid);
        adjAttributionSource.pid = VALUE_OR_RETURN_STATUS(legacy2aidl_pid_t_int32_t(callingPid));
        currentPid = callingPid;
    }
    adjAttributionSource = afutils::checkAttributionSourcePackage(adjAttributionSource);
    ALOGV("createEffect pid %d, effectClient %p, priority %d, sessionId %d, io %d, factory %p",
          adjAttributionSource.pid, effectClient.get(), priority, sessionId, io,
          mEffectsFactoryHal.get());
    if (mEffectsFactoryHal == 0) {
        ALOGE("%s: no effects factory hal", __func__);
        lStatus = NO_INIT;
        goto Exit;
    }
    if (sessionId == AUDIO_SESSION_OUTPUT_MIX) {
        if (!settingsAllowed()) {
            ALOGE("%s: no permission for AUDIO_SESSION_OUTPUT_MIX", __func__);
            lStatus = PERMISSION_DENIED;
            goto Exit;
        }
    } else if (sessionId == AUDIO_SESSION_OUTPUT_STAGE) {
        if (io == AUDIO_IO_HANDLE_NONE) {
            ALOGE("%s: APM must specify output when using AUDIO_SESSION_OUTPUT_STAGE", __func__);
            lStatus = BAD_VALUE;
            goto Exit;
        }
        IAfPlaybackThread* thread;
        {
            audio_utils::lock_guard l(mutex());
            thread = checkPlaybackThread_l(io);
        }
        if (thread == nullptr) {
            ALOGE("%s: invalid output %d specified for AUDIO_SESSION_OUTPUT_STAGE", __func__, io);
            lStatus = BAD_VALUE;
            goto Exit;
        }
        if (!modifyDefaultAudioEffectsAllowed(adjAttributionSource)
                && !isAudioServerUid(callingUid)) {
            ALOGE("%s: effect on AUDIO_SESSION_OUTPUT_STAGE not granted for uid %d",
                    __func__, callingUid);
            lStatus = PERMISSION_DENIED;
            goto Exit;
        }
    } else if (sessionId == AUDIO_SESSION_DEVICE) {
        if (!modifyDefaultAudioEffectsAllowed(adjAttributionSource)) {
            ALOGE("%s: device effect permission denied for uid %d", __func__, callingUid);
            lStatus = PERMISSION_DENIED;
            goto Exit;
        }
        if (io != AUDIO_IO_HANDLE_NONE) {
            ALOGE("%s: io handle should not be specified for device effect", __func__);
            lStatus = BAD_VALUE;
            goto Exit;
        }
    } else {
        if (audio_unique_id_get_use(sessionId) != AUDIO_UNIQUE_ID_USE_SESSION) {
            ALOGE("%s: invalid sessionId %d", __func__, sessionId);
            lStatus = BAD_VALUE;
            goto Exit;
        }
    }
    {
        uint32_t preferredType = (sessionId == AUDIO_SESSION_OUTPUT_MIX ?
                                  EFFECT_FLAG_TYPE_AUXILIARY : EFFECT_FLAG_TYPE_MASK);
        lStatus = getEffectDescriptor(&descIn.uuid, &descIn.type, preferredType, &descOut);
        if (lStatus < 0) {
            ALOGW("createEffect() error %d from getEffectDescriptor", lStatus);
            goto Exit;
        }
        if (sessionId != AUDIO_SESSION_OUTPUT_MIX &&
             (descOut.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
            lStatus = INVALID_OPERATION;
            goto Exit;
        }
        if ((memcmp(&descOut.type, SL_IID_VISUALIZATION, sizeof(effect_uuid_t)) == 0) &&
            !recordingAllowed(adjAttributionSource)) {
            lStatus = PERMISSION_DENIED;
            goto Exit;
        }
        const bool hapticPlaybackRequired = IAfEffectModule::isHapticGenerator(&descOut.type);
        if (hapticPlaybackRequired
                && (sessionId == AUDIO_SESSION_DEVICE
                        || sessionId == AUDIO_SESSION_OUTPUT_MIX
                        || sessionId == AUDIO_SESSION_OUTPUT_STAGE)) {
            lStatus = INVALID_OPERATION;
            goto Exit;
        }
        if ((memcmp(&descOut.type, FX_IID_SPATIALIZER, sizeof(effect_uuid_t)) == 0) &&
            (callingUid != AID_AUDIOSERVER || currentPid != getpid())) {
            ALOGW("%s: attempt to create a spatializer effect from uid/pid %d/%d",
                    __func__, callingUid, currentPid);
            lStatus = PERMISSION_DENIED;
            goto Exit;
        }
        if (io == AUDIO_IO_HANDLE_NONE && sessionId == AUDIO_SESSION_OUTPUT_MIX) {
            io = AudioSystem::getOutputForEffect(&descOut);
            ALOGV("createEffect got output %d", io);
        }
        audio_utils::lock_guard _l(mutex());
        if (sessionId == AUDIO_SESSION_DEVICE) {
            sp<Client> client = registerPid(currentPid);
            ALOGV("%s device type %#x address %s", __func__, device.mType, device.getAddress());
            handle = mDeviceEffectManager->createEffect_l(
                    &descOut, device, client, effectClient, mPatchPanel->patches_l(),
                    &enabledOut, &lStatus, probe, request.notifyFramesProcessed);
            if (lStatus != NO_ERROR && lStatus != ALREADY_EXISTS) {
                audio_utils::lock_guard _cl(clientMutex());
                client.clear();
            } else {
                if (handle.get() != nullptr) idOut = handle->id();
            }
            goto Register;
        }
        if (io == AUDIO_IO_HANDLE_NONE) {
            io = findIoHandleBySessionId_l(sessionId, mPlaybackThreads);
            if (io == AUDIO_IO_HANDLE_NONE) {
                io = findIoHandleBySessionId_l(sessionId, mRecordThreads);
            }
            if (io == AUDIO_IO_HANDLE_NONE) {
                io = findIoHandleBySessionId_l(sessionId, mMmapThreads);
            }
            if (getOrphanEffectChain_l(sessionId).get() != nullptr) {
                ALOGE("%s: effect %s with no specified io handle is denied because the AudioRecord"
                      " for session %d no longer exists",
                      __func__, descOut.name, sessionId);
                lStatus = PERMISSION_DENIED;
                goto Exit;
            }
            if (io == AUDIO_IO_HANDLE_NONE && mPlaybackThreads.size() > 0) {
                io = mPlaybackThreads.keyAt(0);
            }
            ALOGV("createEffect() got io %d for effect %s", io, descOut.name);
        } else if (checkPlaybackThread_l(io) != nullptr
                        && sessionId != AUDIO_SESSION_OUTPUT_STAGE) {
            for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
                const audio_io_handle_t checkIo = mPlaybackThreads.keyAt(i);
                if (io == checkIo) {
                    if (hapticPlaybackRequired
                            && mPlaybackThreads.valueAt(i)
                                    ->hapticChannelMask() == AUDIO_CHANNEL_NONE) {
                        ALOGE("%s: haptic playback thread is required while the required playback "
                              "thread(io=%d) doesn't support", __func__, (int)io);
                        lStatus = BAD_VALUE;
                        goto Exit;
                    }
                    continue;
                }
                const uint32_t sessionType =
                        mPlaybackThreads.valueAt(i)->hasAudioSession(sessionId);
                if ((sessionType & IAfThreadBase::EFFECT_SESSION) != 0) {
                    ALOGE("%s: effect %s io %d denied because session %d effect exists on io %d",
                          __func__, descOut.name, (int) io, (int) sessionId, (int) checkIo);
                    android_errorWriteLog(0x534e4554, "123237974");
                    lStatus = BAD_VALUE;
                    goto Exit;
                }
            }
        }
        IAfThreadBase* thread = checkRecordThread_l(io);
        if (thread == NULL) {
            thread = checkPlaybackThread_l(io);
            if (thread == NULL) {
                thread = checkMmapThread_l(io);
                if (thread == NULL) {
                    ALOGE("createEffect() unknown output thread");
                    lStatus = BAD_VALUE;
                    goto Exit;
                }
            }
        } else {
            sp<IAfEffectChain> chain = getOrphanEffectChain_l(sessionId);
            if (chain != 0) {
                audio_utils::lock_guard _l2(thread->mutex());
                thread->addEffectChain_l(chain);
            }
        }
        sp<Client> client = registerPid(currentPid);
        bool pinned = !audio_is_global_session(sessionId) && isSessionAcquired_l(sessionId);
        IAfThreadBase* oriThread = nullptr;
        if (hapticPlaybackRequired && thread->hapticChannelMask() == AUDIO_CHANNEL_NONE) {
            IAfThreadBase* const hapticThread = hapticPlaybackThread_l();
            if (hapticThread == nullptr) {
                ALOGE("%s haptic thread not found while it is required", __func__);
                lStatus = INVALID_OPERATION;
                goto Exit;
            }
            if (hapticThread != thread) {
                oriThread = thread;
                thread = hapticThread;
            }
        }
        handle = thread->createEffect_l(client, effectClient, priority, sessionId,
                                        &descOut, &enabledOut, &lStatus, pinned, probe,
                                        request.notifyFramesProcessed);
        if (lStatus != NO_ERROR && lStatus != ALREADY_EXISTS) {
            audio_utils::lock_guard _cl(clientMutex());
            client.clear();
        } else {
            if (handle.get() != nullptr) idOut = handle->id();
            if (hapticPlaybackRequired && oriThread != nullptr) {
                oriThread->invalidateTracksForAudioSession(sessionId);
            }
        }
    }
Register:
    if (!probe && (lStatus == NO_ERROR || lStatus == ALREADY_EXISTS)) {
        if (lStatus == ALREADY_EXISTS) {
            response->alreadyExists = true;
            lStatus = NO_ERROR;
        } else {
            response->alreadyExists = false;
        }
        sp<IAfEffectBase> effect = handle->effect().promote();
        if (effect != nullptr) {
            status_t rStatus = effect->updatePolicyState();
            if (rStatus != NO_ERROR) {
                lStatus = rStatus;
            }
        }
    } else {
        handle.clear();
    }
    response->id = idOut;
    response->enabled = enabledOut != 0;
    response->effect = handle->asIEffect();
    response->desc = VALUE_OR_RETURN_STATUS(
            legacy2aidl_effect_descriptor_t_EffectDescriptor(descOut));
Exit:
    return lStatus;
}
status_t AudioFlinger::moveEffects(audio_session_t sessionId, audio_io_handle_t srcOutput,
        audio_io_handle_t dstOutput)
{
    ALOGV("%s() session %d, srcOutput %d, dstOutput %d",
            __func__, sessionId, srcOutput, dstOutput);
    audio_utils::lock_guard _l(mutex());
    if (srcOutput == dstOutput) {
        ALOGW("%s() same dst and src outputs %d", __func__, dstOutput);
        return NO_ERROR;
    }
    IAfPlaybackThread* const srcThread = checkPlaybackThread_l(srcOutput);
    if (srcThread == nullptr) {
        ALOGW("%s() bad srcOutput %d", __func__, srcOutput);
        return BAD_VALUE;
    }
    IAfPlaybackThread* const dstThread = checkPlaybackThread_l(dstOutput);
    if (dstThread == nullptr) {
        ALOGW("%s() bad dstOutput %d", __func__, dstOutput);
        return BAD_VALUE;
    }
    audio_utils::scoped_lock _ll(dstThread->mutex(), srcThread->mutex());
    return moveEffectChain_ll(sessionId, srcThread, dstThread);
}
void AudioFlinger::setEffectSuspended(int effectId,
                                audio_session_t sessionId,
                                bool suspended)
{
    audio_utils::lock_guard _l(mutex());
    sp<IAfThreadBase> thread = getEffectThread_l(sessionId, effectId);
    if (thread == nullptr) {
      return;
    }
    audio_utils::lock_guard _sl(thread->mutex());
    sp<IAfEffectModule> effect = thread->getEffect_l(sessionId, effectId);
    thread->setEffectSuspended_l(&effect->desc().type, suspended, sessionId);
}
status_t AudioFlinger::moveEffectChain_ll(audio_session_t sessionId,
        IAfPlaybackThread* srcThread, IAfPlaybackThread* dstThread)
{
    ALOGV("%s: session %d from thread %p to thread %p",
            __func__, sessionId, srcThread, dstThread);
    sp<IAfEffectChain> chain = srcThread->getEffectChain_l(sessionId);
    if (chain == 0) {
        ALOGW("%s: effect chain for session %d not on source thread %p",
                __func__, sessionId, srcThread);
        return INVALID_OPERATION;
    }
    if (!chain->isCompatibleWithThread_l(dstThread)) {
        ALOGW("%s: effect chain failed because"
                " destination thread %p is not compatible with effects in the chain",
                __func__, dstThread);
        return INVALID_OPERATION;
    }
    srcThread->removeEffectChain_l(chain);
    sp<IAfEffectChain> dstChain;
    Vector<sp<IAfEffectModule>> removed;
    status_t status = NO_ERROR;
    std::string errorString;
    for (sp<IAfEffectModule> effect = chain->getEffectFromId_l(0); effect != nullptr;
            effect = chain->getEffectFromId_l(0)) {
        srcThread->removeEffect_l(effect);
        removed.add(effect);
        status = dstThread->addEffect_ll(effect);
        if (status != NO_ERROR) {
            errorString = StringPrintf(
                    "cannot add effect %p to destination thread", effect.get());
            break;
        }
        if (dstChain == nullptr) {
            dstChain = effect->getCallback()->chain().promote();
            if (dstChain == nullptr) {
                errorString = StringPrintf("cannot get chain from effect %p", effect.get());
                status = NO_INIT;
                break;
            }
        }
    }
    size_t restored = 0;
    if (status != NO_ERROR) {
        dstChain.clear();
        for (const auto& effect : removed) {
            dstThread->removeEffect_l(effect);
            if (srcThread->addEffect_ll(effect) == NO_ERROR) {
                ++restored;
                if (dstChain == nullptr) {
                    dstChain = effect->getCallback()->chain().promote();
                }
            }
        }
    }
    size_t started = 0;
    if (dstChain != nullptr && !removed.empty()) {
        dstChain->mutex().lock();
        for (const auto& effect : removed) {
            if (effect->state() == IAfEffectModule::ACTIVE ||
                    effect->state() == IAfEffectModule::STOPPING) {
                ++started;
                effect->start();
            }
        }
        dstChain->mutex().unlock();
    }
    if (status != NO_ERROR) {
        if (errorString.empty()) {
            errorString = StringPrintf("%s: failed status %d", __func__, status);
        }
        ALOGW("%s: %s unsuccessful move of session %d from srcThread %p to dstThread %p "
                "(%zu effects removed from srcThread, %zu effects restored to srcThread, "
                "%zu effects started)",
                __func__, errorString.c_str(), sessionId, srcThread, dstThread,
                removed.size(), restored, started);
    } else {
        ALOGD("%s: successful move of session %d from srcThread %p to dstThread %p "
                "(%zu effects moved, %zu effects started)",
                __func__, sessionId, srcThread, dstThread, removed.size(), started);
    }
    return status;
}
status_t AudioFlinger::moveAuxEffectToIo(int EffectId,
        const sp<IAfPlaybackThread>& dstThread, sp<IAfPlaybackThread>* srcThread)
{
    status_t status = NO_ERROR;
    audio_utils::lock_guard _l(mutex());
    const sp<IAfThreadBase> threadBase = getEffectThread_l(AUDIO_SESSION_OUTPUT_MIX, EffectId);
    const sp<IAfPlaybackThread> thread = threadBase ? threadBase->asIAfPlaybackThread() : nullptr;
    if (EffectId != 0 && thread != 0 && dstThread != thread.get()) {
        audio_utils::scoped_lock _ll(dstThread->mutex(), thread->mutex());
        sp<IAfEffectChain> srcChain = thread->getEffectChain_l(AUDIO_SESSION_OUTPUT_MIX);
        sp<IAfEffectChain> dstChain;
        if (srcChain == 0) {
            return INVALID_OPERATION;
        }
        sp<IAfEffectModule> effect = srcChain->getEffectFromId_l(EffectId);
        if (effect == 0) {
            return INVALID_OPERATION;
        }
        thread->removeEffect_l(effect);
        status = dstThread->addEffect_ll(effect);
        if (status != NO_ERROR) {
            thread->addEffect_ll(effect);
            status = INVALID_OPERATION;
            goto Exit;
        }
        dstChain = effect->getCallback()->chain().promote();
        if (dstChain == 0) {
            thread->addEffect_ll(effect);
            status = INVALID_OPERATION;
        }
Exit:
        if (effect->state() == IAfEffectModule::ACTIVE ||
            effect->state() == IAfEffectModule::STOPPING) {
            effect->start();
        }
    }
    if (status == NO_ERROR && srcThread != nullptr) {
        *srcThread = thread;
    }
    return status;
}
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
{
    if (mGlobalEffectEnableTime != 0 &&
            ((systemTime() - mGlobalEffectEnableTime) < kMinGlobalEffectEnabletimeNs)) {
        return true;
    }
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        sp<IAfEffectChain> ec =
                mPlaybackThreads.valueAt(i)->getEffectChain_l(AUDIO_SESSION_OUTPUT_MIX);
        if (ec != 0 && ec->isNonOffloadableEnabled()) {
            return true;
        }
    }
    return false;
}
void AudioFlinger::onNonOffloadableGlobalEffectEnable()
{
    audio_utils::lock_guard _l(mutex());
    mGlobalEffectEnableTime = systemTime();
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        const sp<IAfPlaybackThread> t = mPlaybackThreads.valueAt(i);
        if (t->type() == IAfThreadBase::OFFLOAD) {
            t->invalidateTracks(AUDIO_STREAM_MUSIC);
        }
    }
}
status_t AudioFlinger::putOrphanEffectChain_l(const sp<IAfEffectChain>& chain)
{
    chain->setEffectSuspended_l(FX_IID_AEC, false);
    chain->setEffectSuspended_l(FX_IID_NS, false);
    audio_session_t session = chain->sessionId();
    ssize_t index = mOrphanEffectChains.indexOfKey(session);
    ALOGV("putOrphanEffectChain_l session %d index %zd", session, index);
    if (index >= 0) {
        ALOGW("putOrphanEffectChain_l chain for session %d already present", session);
        return ALREADY_EXISTS;
    }
    mOrphanEffectChains.add(session, chain);
    return NO_ERROR;
}
sp<IAfEffectChain> AudioFlinger::getOrphanEffectChain_l(audio_session_t session)
{
    sp<IAfEffectChain> chain;
    ssize_t index = mOrphanEffectChains.indexOfKey(session);
    ALOGV("getOrphanEffectChain_l session %d index %zd", session, index);
    if (index >= 0) {
        chain = mOrphanEffectChains.valueAt(index);
        mOrphanEffectChains.removeItemsAt(index);
    }
    return chain;
}
bool AudioFlinger::updateOrphanEffectChains(const sp<IAfEffectModule>& effect)
{
    audio_utils::lock_guard _l(mutex());
    audio_session_t session = effect->sessionId();
    ssize_t index = mOrphanEffectChains.indexOfKey(session);
    ALOGV("updateOrphanEffectChains session %d index %zd", session, index);
    if (index >= 0) {
        sp<IAfEffectChain> chain = mOrphanEffectChains.valueAt(index);
        if (chain->removeEffect_l(effect, true) == 0) {
            ALOGV("updateOrphanEffectChains removing effect chain at index %zd", index);
            mOrphanEffectChains.removeItemsAt(index);
        }
        return true;
    }
    return false;
}
status_t AudioFlinger::listAudioPorts(unsigned int* num_ports,
        struct audio_port* ports) const
{
    audio_utils::lock_guard _l(mutex());
    return mPatchPanel->listAudioPorts_l(num_ports, ports);
}
status_t AudioFlinger::getAudioPort(struct audio_port_v7* port) const {
    const status_t status = AudioValidator::validateAudioPort(*port);
    if (status != NO_ERROR) {
        return status;
    }
    audio_utils::lock_guard _l(mutex());
    return mPatchPanel->getAudioPort_l(port);
}
status_t AudioFlinger::createAudioPatch(
        const struct audio_patch* patch, audio_patch_handle_t* handle)
{
    const status_t status = AudioValidator::validateAudioPatch(*patch);
    if (status != NO_ERROR) {
        return status;
    }
    audio_utils::lock_guard _l(mutex());
    return mPatchPanel->createAudioPatch_l(patch, handle);
}
status_t AudioFlinger::releaseAudioPatch(audio_patch_handle_t handle)
{
    audio_utils::lock_guard _l(mutex());
    return mPatchPanel->releaseAudioPatch_l(handle);
}
status_t AudioFlinger::listAudioPatches(
        unsigned int* num_patches, struct audio_patch* patches) const
{
    audio_utils::lock_guard _l(mutex());
    return mPatchPanel->listAudioPatches_l(num_patches, patches);
}
status_t AudioFlinger::onTransactWrapper(TransactionCode code,
                                         [[maybe_unused]] const Parcel& data,
                                         [[maybe_unused]] uint32_t flags,
                                         const std::function<status_t()>& delegate) {
    switch (code) {
        case TransactionCode::SET_STREAM_VOLUME:
        case TransactionCode::SET_STREAM_MUTE:
        case TransactionCode::OPEN_OUTPUT:
        case TransactionCode::OPEN_DUPLICATE_OUTPUT:
        case TransactionCode::CLOSE_OUTPUT:
        case TransactionCode::SUSPEND_OUTPUT:
        case TransactionCode::RESTORE_OUTPUT:
        case TransactionCode::OPEN_INPUT:
        case TransactionCode::CLOSE_INPUT:
        case TransactionCode::SET_VOICE_VOLUME:
        case TransactionCode::MOVE_EFFECTS:
        case TransactionCode::SET_EFFECT_SUSPENDED:
        case TransactionCode::LOAD_HW_MODULE:
        case TransactionCode::GET_AUDIO_PORT:
        case TransactionCode::CREATE_AUDIO_PATCH:
        case TransactionCode::RELEASE_AUDIO_PATCH:
        case TransactionCode::LIST_AUDIO_PATCHES:
        case TransactionCode::SET_AUDIO_PORT_CONFIG:
        case TransactionCode::SET_RECORD_SILENCED:
        case TransactionCode::AUDIO_POLICY_READY:
        case TransactionCode::SET_DEVICE_CONNECTED_STATE:
        case TransactionCode::SET_REQUESTED_LATENCY_MODE:
        case TransactionCode::GET_SUPPORTED_LATENCY_MODES:
        case TransactionCode::INVALIDATE_TRACKS:
        case TransactionCode::GET_AUDIO_POLICY_CONFIG:
            ALOGW("%s: transaction %d received from PID %d",
                  __func__, code, IPCThreadState::self()->getCallingPid());
            switch (code) {
                case TransactionCode::SET_RECORD_SILENCED:
                case TransactionCode::SET_EFFECT_SUSPENDED:
                    break;
                default:
                    return INVALID_OPERATION;
            }
            return OK;
        default:
            break;
    }
    switch (code) {
        case TransactionCode::SET_MASTER_VOLUME:
        case TransactionCode::SET_MASTER_MUTE:
        case TransactionCode::MASTER_MUTE:
        case TransactionCode::GET_SOUND_DOSE_INTERFACE:
        case TransactionCode::SET_MODE:
        case TransactionCode::SET_MIC_MUTE:
        case TransactionCode::SET_LOW_RAM_DEVICE:
        case TransactionCode::SYSTEM_READY:
        case TransactionCode::SET_AUDIO_HAL_PIDS:
        case TransactionCode::SET_VIBRATOR_INFOS:
        case TransactionCode::UPDATE_SECONDARY_OUTPUTS:
        case TransactionCode::SET_BLUETOOTH_VARIABLE_LATENCY_ENABLED:
        case TransactionCode::IS_BLUETOOTH_VARIABLE_LATENCY_ENABLED:
        case TransactionCode::SUPPORTS_BLUETOOTH_VARIABLE_LATENCY: {
            if (!isServiceUid(IPCThreadState::self()->getCallingUid())) {
                ALOGW("%s: transaction %d received from PID %d unauthorized UID %d",
                      __func__, code, IPCThreadState::self()->getCallingPid(),
                      IPCThreadState::self()->getCallingUid());
                switch (code) {
                    case TransactionCode::SYSTEM_READY:
                        break;
                    default:
                        return INVALID_OPERATION;
                }
                return OK;
            }
        } break;
        default:
            break;
    }
    switch (code) {
        case TransactionCode::CREATE_TRACK:
        case TransactionCode::CREATE_RECORD:
        case TransactionCode::SET_MASTER_VOLUME:
        case TransactionCode::SET_MASTER_MUTE:
        case TransactionCode::SET_MIC_MUTE:
        case TransactionCode::SET_PARAMETERS:
        case TransactionCode::CREATE_EFFECT:
        case TransactionCode::SYSTEM_READY: {
            requestLogMerge();
            break;
        }
        default:
            break;
    }
    const std::string methodName = getIAudioFlingerStatistics().getMethodForCode(code);
    mediautils::TimeCheck check(
            std::string("IAudioFlinger::").append(methodName),
            [code, methodName](bool timeout, float elapsedMs) {
        if (timeout) {
            mediametrics::LogItem(mMetricsId)
                .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_TIMEOUT)
                .set(AMEDIAMETRICS_PROP_METHODCODE, int64_t(code))
                .set(AMEDIAMETRICS_PROP_METHODNAME, methodName.c_str())
                .record();
        } else {
            getIAudioFlingerStatistics().event(code, elapsedMs);
        }
    }, mediautils::TimeCheck::kDefaultTimeoutDuration,
    mediautils::TimeCheck::kDefaultSecondChanceDuration,
    true );
    if (IPCThreadState::self()->getCallingPid() != getpid()) {
        AudioSystem::get_audio_policy_service();
    }
    return delegate();
}
}
}
