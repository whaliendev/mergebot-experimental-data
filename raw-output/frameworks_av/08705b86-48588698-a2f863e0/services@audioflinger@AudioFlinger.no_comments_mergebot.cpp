#define LOG_TAG "AudioFlinger"
#define AUDIO_ARRAYS_STATIC_CHECK 1
#include "Configuration.h"
#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <string>
#include <sys/time.h>
#include <sys/resource.h>
#include <thread>
#include <android/media/IAudioPolicyService.h>
#include <android/os/IExternalVibratorService.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <binder/Parcel.h>
#include <media/audiohal/DeviceHalInterface.h>
#include <media/audiohal/DevicesFactoryHalInterface.h>
#include <media/audiohal/EffectsFactoryHalInterface.h>
#include <media/AudioParameter.h>
#include <media/MediaMetricsItem.h>
#include <media/TypeConverter.h>
#include <mediautils/TimeCheck.h>
#include <memunreachable/memunreachable.h>
#include <utils/String16.h>
#include <utils/threads.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>
#include <system/audio.h>
#include <audiomanager/AudioManager.h>
#include "AudioFlinger.h"
#include "NBAIO_Tee.h"
#include <media/AudioResamplerPublic.h>
#include <system/audio_effects/effect_visualizer.h>
#include <system/audio_effects/effect_ns.h>
#include <system/audio_effects/effect_aec.h>
#include <system/audio_effects/effect_hapticgenerator.h>
#include <system/audio_effects/effect_spatializer.h>
#include <audio_utils/primitives.h>
#include <powermanager/PowerManager.h>
#include <media/IMediaLogService.h>
#include <media/AidlConversion.h>
#include <media/AudioValidator.h>
#include <media/nbaio/Pipe.h>
#include <media/nbaio/PipeReader.h>
#include <mediautils/BatteryNotifier.h>
#include <mediautils/MemoryLeakTrackUtil.h>
#include <mediautils/ServiceUtilities.h>
#include <private/android_filesystem_config.h>
#include <BufLog.h>
#include "TypedLogger.h"
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif
namespace android {
using media::IEffectClient;
using android::content::AttributionSourceState;
static const char kDeadlockedString[] = "AudioFlinger may be deadlocked\n";
static const char kHardwareLockedString[] = "Hardware lock is taken\n";
static const char kClientLockedString[] = "Client lock is taken\n";
static const char kNoEffectsFactory[] = "Effects Factory is absent\n";
uint32_t AudioFlinger::mScreenState;
uint32_t AudioFlinger::mScreenState;
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
static sp<os::IExternalVibratorService> sExternalVibratorService;
static sp<os::IExternalVibratorService> getExternalVibratorService() {
    if (sExternalVibratorService == 0) {
        sp<IBinder> binder = defaultServiceManager()->getService(
            String16("external_vibrator_service"));
        if (binder != 0) {
            sExternalVibratorService =
                interface_cast<os::IExternalVibratorService>(binder);
        }
    }
    return sExternalVibratorService;
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
AttributionSourceState AudioFlinger::checkAttributionSourcePackage(
        const AttributionSourceState& attributionSource) {
    Vector<String16> packages;
    PermissionController{}.getPackagesForUid(attributionSource.uid, packages);
    AttributionSourceState checkedAttributionSource = attributionSource;
    if (!attributionSource.packageName.has_value()
            || attributionSource.packageName.value().size() == 0) {
        if (!packages.isEmpty()) {
            checkedAttributionSource.packageName =
                std::move(legacy2aidl_String16_string(packages[0]).value());
        }
    } else {
        String16 opPackageLegacy = VALUE_OR_FATAL(
            aidl2legacy_string_view_String16(attributionSource.packageName.value_or("")));
        if (std::find_if(packages.begin(), packages.end(),
                [&opPackageLegacy](const auto& package) {
                return opPackageLegacy == package; }) == packages.end()) {
            ALOGW("The package name(%s) provided does not correspond to the uid %d",
                    attributionSource.packageName.value_or("").c_str(), attributionSource.uid);
            checkedAttributionSource.packageName = std::optional<std::string>();
        }
    }
    return checkedAttributionSource;
}
std::string formatToString(audio_format_t format) {
    std::string result;
    FormatConverter::toString(format, result);
    return result;
}
void AudioFlinger::instantiate() {
    sp<IServiceManager> sm(defaultServiceManager());
    sm->addService(String16(IAudioFlinger::DEFAULT_SERVICE_NAME),
                   new AudioFlingerServerAdapter(new AudioFlinger()), false,
                   IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT);
}
AudioFlinger::AudioFlinger()
    : mMediaLogNotifier(new AudioFlinger::MediaLogNotifier()),
      mPrimaryHardwareDev(NULL),
      mAudioHwDevs(NULL),
      mHardwareStatus(AUDIO_HW_IDLE),
      mMasterVolume(1.0f),
      mMasterMute(false),
      mMode(AUDIO_MODE_INVALID),
      mBtNrecIsOff(false),
      mIsLowRamDevice(true),
      mIsDeviceTypeKnown(false),
      mTotalMemory(0),
      mClientSharedHeapSize(kMinimumClientSharedHeapSizeBytes),
      mGlobalEffectEnableTime(0),
      mPatchPanel(this),
      mDeviceEffectManager(this),
      mSystemReady(false)
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
    mDevicesFactoryHal = DevicesFactoryHalInterface::create();
    mEffectsFactoryHal = EffectsFactoryHalInterface::create();
    mMediaLogNotifier->run("MediaLogNotifier");
    std::vector<pid_t> halPids;
    mDevicesFactoryHal->getHalPids(&halPids);
    TimeCheck::setAudioHalPids(halPids);
    mediametrics::LogItem(mMetricsId)
        .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_CTOR)
        .record();
}
void AudioFlinger::onFirstRef()
{
    Mutex::Autolock _l(mLock);
    char val_str[PROPERTY_VALUE_MAX] = { 0 };
    if (property_get("ro.audio.flinger_standbytime_ms", val_str, NULL) >= 0) {
        uint32_t int_val;
        if (1 == sscanf(val_str, "%u", &int_val)) {
            mStandbyTimeInNsecs = milliseconds(int_val);
            ALOGI("Using %u mSec as standby time.", int_val);
        } else {
            mStandbyTimeInNsecs = kDefaultStandbyTimeInNsecs;
            ALOGI("Using default %u mSec as standby time.",
                    (uint32_t)(mStandbyTimeInNsecs / 1000000));
        }
    }
    mMode = AUDIO_MODE_NORMAL;
    gAudioFlinger = this;
    mDevicesFactoryHalCallback = new DevicesFactoryHalCallbackImpl;
    mDevicesFactoryHal->setCallbackOnce(mDevicesFactoryHalCallback);
}
status_t AudioFlinger::setAudioHalPids(const std::vector<pid_t>& pids) {
  TimeCheck::setAudioHalPids(pids);
  return NO_ERROR;
}
status_t AudioFlinger::setVibratorInfos(
        const std::vector<media::AudioVibratorInfo>& vibratorInfos) {
    Mutex::Autolock _l(mLock);
    mAudioVibratorInfos = vibratorInfos;
    return NO_ERROR;
}
status_t AudioFlinger::updateSecondaryOutputs(
        const TrackSecondaryOutputsMap& trackSecondaryOutputs) {
    Mutex::Autolock _l(mLock);
    for (const auto& [trackId, secondaryOutputs] : trackSecondaryOutputs) {
        size_t i = 0;
        for (; i < mPlaybackThreads.size(); ++i) {
            PlaybackThread *thread = mPlaybackThreads.valueAt(i).get();
            Mutex::Autolock _tl(thread->mLock);
            sp<PlaybackThread::Track> track = thread->getTrackById_l(trackId);
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
status_t AudioFlinger::setDeviceConnectedState(const struct audio_port_v7 *port, bool connected) {
    status_t final_result = NO_INIT;
    Mutex::Autolock _l(mLock);
    AutoMutex lock(mHardwareLock);
    mHardwareStatus = AUDIO_HW_SET_CONNECTED_STATE;
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        sp<DeviceHalInterface> dev = mAudioHwDevs.valueAt(i)->hwDevice();
        status_t result = dev->setConnectedState(port, connected);
        if (final_result != NO_ERROR) {
            final_result = result;
        }
    }
    mHardwareStatus = AUDIO_HW_IDLE;
    return final_result;
}
std::optional<media::AudioVibratorInfo> AudioFlinger::getDefaultVibratorInfo_l() {
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
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
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
    if (direction == MmapStreamInterface::DIRECTION_OUTPUT) {
        audio_config_t fullConfig = AUDIO_CONFIG_INITIALIZER;
        fullConfig.sample_rate = config->sample_rate;
        fullConfig.channel_mask = config->channel_mask;
        fullConfig.format = config->format;
        std::vector<audio_io_handle_t> secondaryOutputs;
        ret = AudioSystem::getOutputForAttr(&localAttr, &io,
                                            actualSessionId,
                                            &streamType, client.attributionSource,
                                            &fullConfig,
                                            (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_MMAP_NOIRQ |
                                                    AUDIO_OUTPUT_FLAG_DIRECT),
                                            deviceId, &portId, &secondaryOutputs);
        ALOGW_IF(!secondaryOutputs.empty(),
                 "%s does not support secondary outputs, ignoring them", __func__);
    } else {
        ret = AudioSystem::getInputForAttr(&localAttr, &io,
                                              RECORD_RIID_INVALID,
                                              actualSessionId,
                                              client.attributionSource,
                                              config,
                                              AUDIO_INPUT_FLAG_MMAP_NOIRQ, deviceId, &portId);
    }
    if (ret != NO_ERROR) {
        return ret;
    }
    sp<MmapThread> thread = mMmapThreads.valueFor(io);
    if (thread != 0) {
        interface = new MmapThreadHandle(thread);
        thread->configure(&localAttr, streamType, actualSessionId, callback, *deviceId, portId);
        *handle = portId;
        *sessionId = actualSessionId;
        config->sample_rate = thread->sampleRate();
        config->channel_mask = thread->channelMask();
        config->format = thread->format();
    } else {
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
int AudioFlinger::onExternalVibrationStart(const sp<os::ExternalVibration>& externalVibration) {
    sp<os::IExternalVibratorService> evs = getExternalVibratorService();
    if (evs != nullptr) {
        int32_t ret;
        binder::Status status = evs->onExternalVibrationStart(*externalVibration, &ret);
        if (status.isOk()) {
            ALOGD("%s, start external vibration with intensity as %d", __func__, ret);
            return ret;
        }
    }
    ALOGD("%s, start external vibration with intensity as MUTE due to %s",
            __func__,
            evs == nullptr ? "external vibration service not found"
                           : "error when querying intensity");
    return static_cast<int>(os::HapticScale::MUTE);
}
void AudioFlinger::onExternalVibrationStop(const sp<os::ExternalVibration>& externalVibration) {
    sp<os::IExternalVibratorService> evs = getExternalVibratorService();
    if (evs != 0) {
        evs->onExternalVibrationStop(*externalVibration);
    }
}
status_t AudioFlinger::addEffectToHal(audio_port_handle_t deviceId,
        audio_module_handle_t hwModuleId, sp<EffectHalInterface> effect) {
    AutoMutex lock(mHardwareLock);
    AudioHwDevice *audioHwDevice = mAudioHwDevs.valueFor(hwModuleId);
    if (audioHwDevice == nullptr) {
        return NO_INIT;
    }
    return audioHwDevice->hwDevice()->addDeviceEffect(deviceId, effect);
}
status_t AudioFlinger::removeEffectFromHal(audio_port_handle_t deviceId,
        audio_module_handle_t hwModuleId, sp<EffectHalInterface> effect) {
    AutoMutex lock(mHardwareLock);
    AudioHwDevice *audioHwDevice = mAudioHwDevs.valueFor(hwModuleId);
    if (audioHwDevice == nullptr) {
        return NO_INIT;
    }
    return audioHwDevice->hwDevice()->removeDeviceEffect(deviceId, effect);
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
    AutoMutex lock(mHardwareLock);
    if (module == 0) {
        ALOGW("findSuitableHwDev_l() loading well know audio hw modules");
        for (size_t i = 0; i < arraysize(audio_interfaces); i++) {
            loadHwModule_l(audio_interfaces[i]);
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
void AudioFlinger::dumpClients(int fd, const Vector<String16>& args __unused)
{
    String8 result;
    result.append("Clients:\n");
    for (size_t i = 0; i < mClients.size(); ++i) {
        sp<Client> client = mClients.valueAt(i).promote();
        if (client != 0) {
            result.appendFormat("  pid: %d\n", client->pid());
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
    write(fd, result.string(), result.size());
}
void AudioFlinger::dumpInternals(int fd, const Vector<String16>& args __unused)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    hardware_call_state hardwareStatus = mHardwareStatus;
    snprintf(buffer, SIZE, "Hardware status: %d\n"
                           "Standby Time mSec: %u\n",
                            hardwareStatus,
                            (uint32_t)(mStandbyTimeInNsecs / 1000000));
    result.append(buffer);
    write(fd, result.string(), result.size());
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
    write(fd, result.string(), result.size());
}
bool AudioFlinger::dumpTryLock(Mutex& mutex)
{
    status_t err = mutex.timedLock(kDumpLockTimeoutNs);
    return err == NO_ERROR;
}
status_t AudioFlinger::dump(int fd, const Vector<String16>& args)
{
    if (!dumpAllowed()) {
        dumpPermissionDenial(fd, args);
    } else {
        bool hardwareLocked = dumpTryLock(mHardwareLock);
        if (!hardwareLocked) {
            String8 result(kHardwareLockedString);
            write(fd, result.string(), result.size());
        } else {
            mHardwareLock.unlock();
        }
        const bool locked = dumpTryLock(mLock);
        if (!locked) {
            String8 result(kDeadlockedString);
            write(fd, result.string(), result.size());
        }
        bool clientLocked = dumpTryLock(mClientLock);
        if (!clientLocked) {
            String8 result(kClientLockedString);
            write(fd, result.string(), result.size());
        }
        if (mEffectsFactoryHal != 0) {
            mEffectsFactoryHal->dumpEffects(fd);
        } else {
            String8 result(kNoEffectsFactory);
            write(fd, result.string(), result.size());
        }
        dumpClients(fd, args);
        if (clientLocked) {
            mClientLock.unlock();
        }
        dumpInternals(fd, args);
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
        mPatchPanel.dump(fd);
        mDeviceEffectManager.dump(fd);
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
            mLock.unlock();
        }
#ifdef TEE_SINK
        NBAIO_Tee::dumpAll(fd, "_DUMP");
#endif
        if (sMediaLogServiceAsBinder != 0) {
            dprintf(fd, "\nmedia.log:\n");
            Vector<String16> args;
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
    }
    return NO_ERROR;
}
sp<AudioFlinger::Client> AudioFlinger::registerPid(pid_t pid)
{
    Mutex::Autolock _cl(mClientLock);
    sp<Client> client = mClients.valueFor(pid).promote();
    if (client == 0) {
        client = new Client(this, pid);
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
        Mutex::Autolock _l(mUnregisteredWritersLock);
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
    Mutex::Autolock _l(mUnregisteredWritersLock);
    mUnregisteredWriters.push(writer);
}
status_t AudioFlinger::createTrack(const media::CreateTrackRequest& _input,
                                   media::CreateTrackResponse& _output)
{
    CreateTrackInput input = VALUE_OR_RETURN_STATUS(CreateTrackInput::fromAidl(_input));
    CreateTrackOutput output;
    sp<PlaybackThread::Track> track;
    sp<TrackHandle> trackHandle;
    sp<Client> client;
    status_t lStatus;
    audio_stream_type_t streamType;
    audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE;
    std::vector<audio_io_handle_t> secondaryOutputs;
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
    if (!isAudioServerOrMediaServerUid(callingUid)) {
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
                                            &output.selectedDeviceId, &portId, &secondaryOutputs);
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
        Mutex::Autolock _l(mLock);
        PlaybackThread *thread = checkPlaybackThread_l(output.outputId);
        if (thread == NULL) {
            ALOGE("no playback thread found for output handle %d", output.outputId);
            lStatus = BAD_VALUE;
            goto Exit;
        }
        client = registerPid(clientPid);
        PlaybackThread *effectThread = NULL;
        for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
            sp<PlaybackThread> t = mPlaybackThreads.valueAt(i);
            if (mPlaybackThreads.keyAt(i) != output.outputId) {
                uint32_t sessions = t->hasAudioSession(sessionId);
                if (sessions & ThreadBase::EFFECT_SESSION) {
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
                                      &lStatus, portId, input.audioTrackCallback);
        LOG_ALWAYS_FATAL_IF((lStatus == NO_ERROR) && (track == 0));
        output.afFrameCount = thread->frameCount();
        output.afSampleRate = thread->sampleRate();
        output.afLatencyMs = thread->latency();
        output.portId = portId;
        if (lStatus == NO_ERROR) {
            updateSecondaryOutputsForTrack_l(track.get(), thread, secondaryOutputs);
        }
        if (lStatus == NO_ERROR && effectThread != NULL) {
            Mutex::Autolock _dl(thread->mLock);
            Mutex::Autolock _sl(effectThread->mLock);
            if (moveEffectChain_l(sessionId, effectThread, thread) == NO_ERROR) {
                effectThreadId = thread->id();
                effectIds = thread->getEffectIds_l(sessionId);
            }
        }
        for (size_t i = 0; i < mPendingSyncEvents.size(); i++) {
            if (mPendingSyncEvents[i]->triggerSession() == sessionId) {
                if (thread->isValidSyncEvent(mPendingSyncEvents[i])) {
                    if (lStatus == NO_ERROR) {
                        (void) track->setSyncEvent(mPendingSyncEvents[i]);
                    } else {
                        mPendingSyncEvents[i]->cancel();
                    }
                    mPendingSyncEvents.removeAt(i);
                    i--;
                }
            }
        }
        if ((output.flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC) == AUDIO_OUTPUT_FLAG_HW_AV_SYNC) {
            setAudioHwSyncForSession_l(thread, sessionId);
        }
    }
    if (lStatus != NO_ERROR) {
        {
            Mutex::Autolock _cl(mClientLock);
            client.clear();
        }
        track.clear();
        goto Exit;
    }
    if (effectThreadId != AUDIO_IO_HANDLE_NONE) {
        AudioSystem::moveEffectsToIo(effectIds, effectThreadId);
    }
    output.audioTrack = new TrackHandle(track);
    _output = VALUE_OR_FATAL(output.toAidl());
Exit:
    if (lStatus != NO_ERROR && output.outputId != AUDIO_IO_HANDLE_NONE) {
        AudioSystem::releaseOutput(portId);
    }
    return lStatus;
}
uint32_t AudioFlinger::sampleRate(audio_io_handle_t ioHandle) const
{
    Mutex::Autolock _l(mLock);
    ThreadBase *thread = checkThread_l(ioHandle);
    if (thread == NULL) {
        ALOGW("sampleRate() unknown thread %d", ioHandle);
        return 0;
    }
    return thread->sampleRate();
}
audio_format_t AudioFlinger::format(audio_io_handle_t output) const
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        ALOGW("format() unknown thread %d", output);
        return AUDIO_FORMAT_INVALID;
    }
    return thread->format();
}
size_t AudioFlinger::frameCount(audio_io_handle_t ioHandle) const
{
    Mutex::Autolock _l(mLock);
    ThreadBase *thread = checkThread_l(ioHandle);
    if (thread == NULL) {
        ALOGW("frameCount() unknown thread %d", ioHandle);
        return 0;
    }
    return thread->frameCount();
}
size_t AudioFlinger::frameCountHAL(audio_io_handle_t ioHandle) const
{
    Mutex::Autolock _l(mLock);
    ThreadBase *thread = checkThread_l(ioHandle);
    if (thread == NULL) {
        ALOGW("frameCountHAL() unknown thread %d", ioHandle);
        return 0;
    }
    return thread->frameCountHAL();
}
uint32_t AudioFlinger::latency(audio_io_handle_t output) const
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = checkPlaybackThread_l(output);
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
    Mutex::Autolock _l(mLock);
    mMasterVolume = value;
    {
        AutoMutex lock(mHardwareLock);
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
    Mutex::Autolock _l(mLock);
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
        AutoMutex lock(mHardwareLock);
        if (mPrimaryHardwareDev == nullptr) {
            return INVALID_OPERATION;
        }
        sp<DeviceHalInterface> dev = mPrimaryHardwareDev->hwDevice();
        mHardwareStatus = AUDIO_HW_SET_MODE;
        ret = dev->setMode(mode);
        mHardwareStatus = AUDIO_HW_IDLE;
    }
    if (NO_ERROR == ret) {
        Mutex::Autolock _l(mLock);
        mMode = mode;
        for (size_t i = 0; i < mPlaybackThreads.size(); i++)
            mPlaybackThreads.valueAt(i)->setMode(mode);
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
    AutoMutex lock(mHardwareLock);
    if (mPrimaryHardwareDev == nullptr) {
        return INVALID_OPERATION;
    }
    sp<DeviceHalInterface> primaryDev = mPrimaryHardwareDev->hwDevice();
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
    AutoMutex lock(mHardwareLock);
    if (mPrimaryHardwareDev == nullptr) {
        return false;
    }
    sp<DeviceHalInterface> primaryDev = mPrimaryHardwareDev->hwDevice();
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
    AutoMutex lock(mLock);
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
    Mutex::Autolock _l(mLock);
    mMasterMute = muted;
    {
        AutoMutex lock(mHardwareLock);
        for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
            AudioHwDevice *dev = mAudioHwDevs.valueAt(i);
            mHardwareStatus = AUDIO_HW_SET_MASTER_MUTE;
            if (dev->canSetMasterMute()) {
                dev->hwDevice()->setMasterMute(muted);
            }
            mHardwareStatus = AUDIO_HW_IDLE;
        }
    }
    Vector<VolumeInterface *> volumeInterfaces = getAllVolumeInterfaces_l();
    for (size_t i = 0; i < volumeInterfaces.size(); i++) {
        volumeInterfaces[i]->setMasterMute(muted);
    }
    return NO_ERROR;
}
float AudioFlinger::masterVolume() const
{
    Mutex::Autolock _l(mLock);
    return masterVolume_l();
}
status_t AudioFlinger::getMasterBalance(float *balance) const
{
    Mutex::Autolock _l(mLock);
    *balance = getMasterBalance_l();
    return NO_ERROR;
}
bool AudioFlinger::masterMute() const
{
    Mutex::Autolock _l(mLock);
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
status_t AudioFlinger::checkStreamType(audio_stream_type_t stream) const
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
    AutoMutex lock(mLock);
    VolumeInterface *volumeInterface = getVolumeInterface_l(output);
    if (volumeInterface == NULL) {
        return BAD_VALUE;
    }
    volumeInterface->setStreamVolume(stream, value);
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
    AutoMutex lock(mLock);
    mStreamTypes[stream].mute = muted;
    Vector<VolumeInterface *> volumeInterfaces = getAllVolumeInterfaces_l();
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
    AutoMutex lock(mLock);
    VolumeInterface *volumeInterface = getVolumeInterface_l(output);
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
    AutoMutex lock(mLock);
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
        std::function<bool(const sp<PlaybackThread>&)> useThread)
{
    std::vector<PatchPanel::SoftwarePatch> swPatches;
    if (mPatchPanel.getDownstreamSoftwarePatches(upStream, &swPatches) != OK) return;
    ALOGV_IF(!swPatches.empty(), "%s found %zu downstream patches for stream ID %d",
            __func__, swPatches.size(), upStream);
    for (const auto& swPatch : swPatches) {
        sp<PlaybackThread> downStream = checkPlaybackThread_l(swPatch.getPlaybackThreadHandle());
        if (downStream != NULL && (useThread == nullptr || useThread(downStream))) {
            downStream->setParameters(keyValuePairs);
        }
    }
}
void AudioFlinger::updateDownStreamPatches_l(const struct audio_patch *patch,
                                             const std::set<audio_io_handle_t> streams)
{
    for (const audio_io_handle_t stream : streams) {
        PlaybackThread *playbackThread = checkPlaybackThread_l(stream);
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
            ioHandle, keyValuePairs.string(),
            IPCThreadState::self()->getCallingPid(), IPCThreadState::self()->getCallingUid());
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    String8 filteredKeyValuePairs = keyValuePairs;
    filterReservedParameters(filteredKeyValuePairs, IPCThreadState::self()->getCallingUid());
    ALOGV("%s: filtered keyvalue %s", __func__, filteredKeyValuePairs.string());
    if (ioHandle == AUDIO_IO_HANDLE_NONE) {
        Mutex::Autolock _l(mLock);
        status_t final_result = NO_INIT;
        {
            AutoMutex lock(mHardwareLock);
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
            if (isOff != (AudioFlinger::mScreenState & 1)) {
                AudioFlinger::mScreenState = ((AudioFlinger::mScreenState & ~1) + 2) | isOff;
            }
        }
        return final_result;
    }
    sp<ThreadBase> thread;
    {
        Mutex::Autolock _l(mLock);
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
        forwardParametersToDownstreamPatches_l(thread->id(), filteredKeyValuePairs);
        return result;
    }
    return BAD_VALUE;
}
String8 AudioFlinger::getParameters(audio_io_handle_t ioHandle, const String8& keys) const
{
    ALOGVV("getParameters() io %d, keys %s, calling pid %d",
            ioHandle, keys.string(), IPCThreadState::self()->getCallingPid());
    Mutex::Autolock _l(mLock);
    if (ioHandle == AUDIO_IO_HANDLE_NONE) {
        String8 out_s8;
        AutoMutex lock(mHardwareLock);
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
    ThreadBase *thread = (ThreadBase *)checkPlaybackThread_l(ioHandle);
    if (thread == NULL) {
        thread = (ThreadBase *)checkRecordThread_l(ioHandle);
        if (thread == NULL) {
            thread = (ThreadBase *)checkMmapThread_l(ioHandle);
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
    AutoMutex lock(mHardwareLock);
    if (mPrimaryHardwareDev == nullptr) {
        return 0;
    }
    mHardwareStatus = AUDIO_HW_GET_INPUT_BUFFER_SIZE;
    sp<DeviceHalInterface> dev = mPrimaryHardwareDev->hwDevice();
    std::vector<audio_channel_mask_t> channelMasks = {channelMask};
    if (channelMask != AUDIO_CHANNEL_IN_MONO)
        channelMasks.push_back(AUDIO_CHANNEL_IN_MONO);
    if (channelMask != AUDIO_CHANNEL_IN_STEREO)
        channelMasks.push_back(AUDIO_CHANNEL_IN_STEREO);
    std::vector<audio_format_t> formats = {format};
    if (format != AUDIO_FORMAT_PCM_16_BIT)
        formats.push_back(AUDIO_FORMAT_PCM_16_BIT);
    std::vector<uint32_t> sampleRates = {sampleRate};
    static const uint32_t SR_44100 = 44100;
    static const uint32_t SR_48000 = 48000;
    if (sampleRate != SR_48000)
        sampleRates.push_back(SR_48000);
    if (sampleRate != SR_44100)
        sampleRates.push_back(SR_44100);
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
    Mutex::Autolock _l(mLock);
    RecordThread *recordThread = checkRecordThread_l(ioHandle);
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
    AutoMutex lock(mHardwareLock);
    if (mPrimaryHardwareDev == nullptr) {
        return INVALID_OPERATION;
    }
    sp<DeviceHalInterface> dev = mPrimaryHardwareDev->hwDevice();
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
    Mutex::Autolock _l(mLock);
    PlaybackThread *playbackThread = checkPlaybackThread_l(output);
    if (playbackThread != NULL) {
        return playbackThread->getRenderPosition(halFrames, dspFrames);
    }
    return BAD_VALUE;
}
void AudioFlinger::registerClient(const sp<media::IAudioFlingerClient>& client)
{
    Mutex::Autolock _l(mLock);
    if (client == 0) {
        return;
    }
    pid_t pid = IPCThreadState::self()->getCallingPid();
    const uid_t uid = IPCThreadState::self()->getCallingUid();
    {
        Mutex::Autolock _cl(mClientLock);
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
    std::vector< sp<AudioFlinger::EffectModule> > removedEffects;
    {
        Mutex::Autolock _l(mLock);
        {
            Mutex::Autolock _cl(mClientLock);
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
void AudioFlinger::ioConfigChanged(audio_io_config_event event,
                                   const sp<AudioIoDescriptor>& ioDesc,
                                   pid_t pid) {
    media::AudioIoDescriptor descAidl = VALUE_OR_FATAL(
            legacy2aidl_AudioIoDescriptor_AudioIoDescriptor(ioDesc));
    media::AudioIoConfigEvent eventAidl = VALUE_OR_FATAL(
            legacy2aidl_audio_io_config_event_AudioIoConfigEvent(event));
    Mutex::Autolock _l(mClientLock);
    size_t size = mNotificationClients.size();
    for (size_t i = 0; i < size; i++) {
        if ((pid == 0) || (mNotificationClients.keyAt(i) == pid)) {
            mNotificationClients.valueAt(i)->audioFlingerClient()->ioConfigChanged(eventAidl,
                                                                                   descAidl);
        }
    }
}
void AudioFlinger::removeClient_l(pid_t pid)
{
    ALOGV("removeClient_l() pid %d, calling pid %d", pid,
            IPCThreadState::self()->getCallingPid());
    mClients.removeItem(pid);
}
sp<AudioFlinger::ThreadBase> AudioFlinger::getEffectThread_l(audio_session_t sessionId,
        int effectId)
{
    sp<ThreadBase> thread;
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
AudioFlinger::Client::Client(const sp<AudioFlinger>& audioFlinger, pid_t pid)
    : RefBase(),
        mAudioFlinger(audioFlinger),
        mPid(pid)
{
    mMemoryDealer = new MemoryDealer(
            audioFlinger->getClientSharedHeapSize(),
            (std::string("AudioFlinger::Client(") + std::to_string(pid) + ")").c_str());
}
AudioFlinger::Client::~Client()
{
    mAudioFlinger->removeClient_l(mPid);
}
sp<MemoryDealer> AudioFlinger::Client::heap() const
{
    return mMemoryDealer;
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
    AutoMutex _l(mMutex);
    mPendingRequests = true;
    mCond.signal();
}
bool AudioFlinger::MediaLogNotifier::threadLoop() {
    if (sMediaLogService == 0) {
        return false;
    }
    {
        AutoMutex _l(mMutex);
        mPendingRequests = false;
        while (!mPendingRequests) {
            mCond.wait(mMutex);
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
    sp<RecordThread::RecordTrack> recordTrack;
    sp<RecordHandle> recordHandle;
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
    if (!isAudioServerOrMediaServerUid(callingUid)) {
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
        Mutex::Autolock _l(mLock);
        RecordThread *thread = checkRecordThread_l(output.inputId);
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
        sp<EffectChain> chain = getOrphanEffectChain_l(sessionId);
        if (chain != 0) {
            Mutex::Autolock _l(thread->mLock);
            thread->addEffectChain_l(chain);
        }
        break;
    }
    }
    output.cblk = recordTrack->getCblk();
    output.buffers = recordTrack->getBuffers();
    output.portId = portId;
    output.audioRecord = new RecordHandle(recordTrack);
    _output = VALUE_OR_FATAL(output.toAidl());
Exit:
    if (lStatus != NO_ERROR) {
        {
            Mutex::Autolock _cl(mClientLock);
            client.clear();
        }
        recordTrack.clear();
        if (output.inputId != AUDIO_IO_HANDLE_NONE) {
            AudioSystem::releaseInput(portId);
        }
    }
    return lStatus;
}
audio_module_handle_t AudioFlinger::loadHwModule(const char *name)
{
    if (name == NULL) {
        return AUDIO_MODULE_HANDLE_NONE;
    }
    if (!settingsAllowed()) {
        return AUDIO_MODULE_HANDLE_NONE;
    }
    Mutex::Autolock _l(mLock);
    AutoMutex lock(mHardwareLock);
    return loadHwModule_l(name);
}
audio_module_handle_t AudioFlinger::loadHwModule_l(const char *name)
{
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        if (strncmp(mAudioHwDevs.valueAt(i)->moduleName(), name, strlen(name)) == 0) {
            ALOGW("loadHwModule() module %s already loaded", name);
            return mAudioHwDevs.keyAt(i);
        }
    }
    sp<DeviceHalInterface> dev;
    int rc = mDevicesFactoryHal->openDevice(name, &dev);
    if (rc) {
        ALOGE("loadHwModule() error %d loading module %s", rc, name);
        return AUDIO_MODULE_HANDLE_NONE;
    }
    mHardwareStatus = AUDIO_HW_INIT;
    rc = dev->initCheck();
    mHardwareStatus = AUDIO_HW_IDLE;
    if (rc) {
        ALOGE("loadHwModule() init check error %d for module %s", rc, name);
        return AUDIO_MODULE_HANDLE_NONE;
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
    audio_module_handle_t handle = (audio_module_handle_t) nextUniqueId(AUDIO_UNIQUE_ID_USE_MODULE);
    AudioHwDevice *audioDevice = new AudioHwDevice(handle, name, dev, flags);
    if (strcmp(name, AUDIO_HARDWARE_MODULE_ID_PRIMARY) == 0) {
        mPrimaryHardwareDev = audioDevice;
        mHardwareStatus = AUDIO_HW_SET_MODE;
        mPrimaryHardwareDev->hwDevice()->setMode(mMode);
        mHardwareStatus = AUDIO_HW_IDLE;
    }
    mAudioHwDevs.add(handle, audioDevice);
    ALOGI("loadHwModule() Loaded %s audio interface, handle %d", name, handle);
    return handle;
}
uint32_t AudioFlinger::getPrimaryOutputSamplingRate()
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = fastPlaybackThread_l();
    return thread != NULL ? thread->sampleRate() : 0;
}
size_t AudioFlinger::getPrimaryOutputFrameCount()
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = fastPlaybackThread_l();
    return thread != NULL ? thread->frameCountHAL() : 0;
}
status_t AudioFlinger::setLowRamDevice(bool isLowRamDevice, int64_t totalMemory)
{
    uid_t uid = IPCThreadState::self()->getCallingUid();
    if (!isAudioServerOrSystemServerUid(uid)) {
        return PERMISSION_DENIED;
    }
    Mutex::Autolock _l(mLock);
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
    Mutex::Autolock _l(mLock);
    AutoMutex lock(mHardwareLock);
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
    Mutex::Autolock _l(mLock);
    ssize_t index = mHwAvSyncIds.indexOfKey(sessionId);
    if (index >= 0) {
        ALOGV("getAudioHwSyncForSession found ID %d for session %d",
              mHwAvSyncIds.valueAt(index), sessionId);
        return mHwAvSyncIds.valueAt(index);
    }
    sp<DeviceHalInterface> dev;
    {
        AutoMutex lock(mHardwareLock);
        if (mPrimaryHardwareDev == nullptr) {
            return AUDIO_HW_SYNC_INVALID;
        }
        dev = mPrimaryHardwareDev->hwDevice();
    }
    if (dev == nullptr) {
        return AUDIO_HW_SYNC_INVALID;
    }
    String8 reply;
    AudioParameter param;
    if (dev->getParameters(String8(AudioParameter::keyHwAvSync), &reply) == OK) {
        param = AudioParameter(reply);
    }
    int value;
    if (param.getInt(String8(AudioParameter::keyHwAvSync), value) != NO_ERROR) {
        ALOGW("getAudioHwSyncForSession error getting sync for session %d", sessionId);
        return AUDIO_HW_SYNC_INVALID;
    }
    for (size_t i = 0; i < mHwAvSyncIds.size(); i++) {
        if (mHwAvSyncIds.valueAt(i) == (audio_hw_sync_t)value) {
            ALOGV("getAudioHwSyncForSession removing ID %d for session %d",
                  value, mHwAvSyncIds.keyAt(i));
            mHwAvSyncIds.removeItemsAt(i);
            break;
        }
    }
    mHwAvSyncIds.add(sessionId, value);
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        sp<PlaybackThread> thread = mPlaybackThreads.valueAt(i);
        uint32_t sessions = thread->hasAudioSession(sessionId);
        if (sessions & ThreadBase::TRACK_SESSION) {
            AudioParameter param = AudioParameter();
            param.addInt(String8(AudioParameter::keyStreamHwAvSync), value);
            String8 keyValuePairs = param.toString();
            thread->setParameters(keyValuePairs);
            forwardParametersToDownstreamPatches_l(thread->id(), keyValuePairs,
                    [](const sp<PlaybackThread>& thread) { return thread->usesHwAvSync(); });
            break;
        }
    }
    ALOGV("getAudioHwSyncForSession adding ID %d for session %d", value, sessionId);
    return (audio_hw_sync_t)value;
}
status_t AudioFlinger::systemReady()
{
    Mutex::Autolock _l(mLock);
    ALOGI("%s", __FUNCTION__);
    if (mSystemReady) {
        ALOGW("%s called twice", __FUNCTION__);
        return NO_ERROR;
    }
    mSystemReady = true;
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        ThreadBase *thread = (ThreadBase *)mPlaybackThreads.valueAt(i).get();
        thread->systemReady();
    }
    for (size_t i = 0; i < mRecordThreads.size(); i++) {
        ThreadBase *thread = (ThreadBase *)mRecordThreads.valueAt(i).get();
        thread->systemReady();
    }
    for (size_t i = 0; i < mMmapThreads.size(); i++) {
        ThreadBase *thread = (ThreadBase *)mMmapThreads.valueAt(i).get();
        thread->systemReady();
    }
    return NO_ERROR;
}
status_t AudioFlinger::getMicrophones(std::vector<media::MicrophoneInfo> *microphones)
{
    AutoMutex lock(mHardwareLock);
    status_t status = INVALID_OPERATION;
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        std::vector<media::MicrophoneInfo> mics;
        AudioHwDevice *dev = mAudioHwDevs.valueAt(i);
        mHardwareStatus = AUDIO_HW_GET_MICROPHONES;
        status_t devStatus = dev->hwDevice()->getMicrophones(&mics);
        mHardwareStatus = AUDIO_HW_IDLE;
        if (devStatus == NO_ERROR) {
            microphones->insert(microphones->begin(), mics.begin(), mics.end());
            status = NO_ERROR;
        }
    }
    return status;
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}
}
