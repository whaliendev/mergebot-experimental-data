#define LOG_TAG "AudioPolicyService"
#include "Configuration.h"
#undef __STRICT_ANSI__
#define __STDINT_LIMITS 
#define __STDC_LIMIT_MACROS 
#include <stdint.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <audio_utils/clock.h>
#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include <cutils/properties.h>
#include <binder/IPCThreadState.h>
#include <binder/PermissionController.h>
#include <binder/IResultReceiver.h>
#include <utils/String16.h>
#include <utils/threads.h>
#include "AudioPolicyService.h"
#include <hardware_legacy/power.h>
#include <media/AidlConversion.h>
#include <media/AudioEffect.h>
#include <media/AudioParameter.h>
#include <mediautils/ServiceUtilities.h>
#include <mediautils/TimeCheck.h>
#include <sensorprivacy/SensorPrivacyManager.h>
#include <system/audio.h>
#include <system/audio_policy.h>
#include <AudioPolicyManager.h>
namespace android {
using binder::Status;
static const char kDeadlockedString[] = "AudioPolicyService may be deadlocked\n";
static const char kCmdDeadlockedString[] = "AudioPolicyService command thread may be deadlocked\n";
static const char kAudioPolicyManagerCustomPath[] = "libaudiopolicymanagercustom.so";
static const int kDumpLockTimeoutNs = 1 * NANOS_PER_SECOND;
static const nsecs_t kAudioCommandTimeoutNs = seconds(3);
static const String16 sManageAudioPolicyPermission("android.permission.MANAGE_AUDIO_POLICY");
static AudioPolicyInterface* createAudioPolicyManager(AudioPolicyClientInterface *clientInterface)
{
    AudioPolicyManager *apm = new AudioPolicyManager(clientInterface);
    status_t status = apm->initialize();
    if (status != NO_ERROR) {
        delete apm;
        apm = nullptr;
    }
    return apm;
}
static void destroyAudioPolicyManager(AudioPolicyInterface *interface)
{
    delete interface;
}
AudioPolicyService::AudioPolicyService()
    : BnAudioPolicyService(),
      mAudioPolicyManager(NULL),
      mAudioPolicyClient(NULL),
      mPhoneState(AUDIO_MODE_INVALID),
      mCaptureStateNotifier(false),
      mCreateAudioPolicyManager(createAudioPolicyManager),
      mDestroyAudioPolicyManager(destroyAudioPolicyManager) {
}
void AudioPolicyService::loadAudioPolicyManager()
{
    mLibraryHandle = dlopen(kAudioPolicyManagerCustomPath, RTLD_NOW);
    if (mLibraryHandle != nullptr) {
        ALOGI("%s loading %s", __func__, kAudioPolicyManagerCustomPath);
        mCreateAudioPolicyManager = reinterpret_cast<CreateAudioPolicyManagerInstance>
                                            (dlsym(mLibraryHandle, "createAudioPolicyManager"));
        const char *lastError = dlerror();
        ALOGW_IF(mCreateAudioPolicyManager == nullptr, "%s createAudioPolicyManager is null %s",
                    __func__, lastError != nullptr ? lastError : "no error");
        mDestroyAudioPolicyManager = reinterpret_cast<DestroyAudioPolicyManagerInstance>(
                                        dlsym(mLibraryHandle, "destroyAudioPolicyManager"));
        lastError = dlerror();
        ALOGW_IF(mDestroyAudioPolicyManager == nullptr, "%s destroyAudioPolicyManager is null %s",
                    __func__, lastError != nullptr ? lastError : "no error");
        if (mCreateAudioPolicyManager == nullptr || mDestroyAudioPolicyManager == nullptr){
            unloadAudioPolicyManager();
            LOG_ALWAYS_FATAL("could not find audiopolicymanager interface methods");
        }
    }
}
void AudioPolicyService::onFirstRef()
{
    {
        Mutex::Autolock _l(mLock);
        mAudioCommandThread = new AudioCommandThread(String8("ApmAudio"), this);
        mOutputCommandThread = new AudioCommandThread(String8("ApmOutput"), this);
        mAudioPolicyClient = new AudioPolicyClient(this);
        loadAudioPolicyManager();
        mAudioPolicyManager = mCreateAudioPolicyManager(mAudioPolicyClient);
    }
    sp<AudioPolicyEffects> audioPolicyEffects = new AudioPolicyEffects();
    sp<UidPolicy> uidPolicy = new UidPolicy(this);
    sp<SensorPrivacyPolicy> sensorPrivacyPolicy = new SensorPrivacyPolicy(this);
    {
        Mutex::Autolock _l(mLock);
        mAudioPolicyEffects = audioPolicyEffects;
        mUidPolicy = uidPolicy;
        mSensorPrivacyPolicy = sensorPrivacyPolicy;
    }
    uidPolicy->registerSelf();
    sensorPrivacyPolicy->registerSelf();
<<<<<<< HEAD
    AudioSystem::audioPolicyReady();
||||||| a2f863e050
=======
    if (mAudioPolicyManager != nullptr) {
        Mutex::Autolock _l(mLock);
        const audio_attributes_t attr = attributes_initializer(AUDIO_USAGE_MEDIA);
        AudioDeviceTypeAddrVector devices;
        bool hasSpatializer = mAudioPolicyManager->canBeSpatialized(&attr, nullptr, devices);
        if (hasSpatializer) {
            mSpatializer = Spatializer::create(this);
        }
    }
    AudioSystem::audioPolicyReady();
>>>>>>> 48588698
}
void AudioPolicyService::unloadAudioPolicyManager()
{
    ALOGV("%s ", __func__);
    if (mLibraryHandle != nullptr) {
        dlclose(mLibraryHandle);
    }
    mLibraryHandle = nullptr;
    mCreateAudioPolicyManager = nullptr;
    mDestroyAudioPolicyManager = nullptr;
}
AudioPolicyService::~AudioPolicyService()
{
    mAudioCommandThread->exit();
    mOutputCommandThread->exit();
    mDestroyAudioPolicyManager(mAudioPolicyManager);
    unloadAudioPolicyManager();
    delete mAudioPolicyClient;
    mNotificationClients.clear();
    mAudioPolicyEffects.clear();
    mUidPolicy->unregisterSelf();
    mSensorPrivacyPolicy->unregisterSelf();
    mUidPolicy.clear();
    mSensorPrivacyPolicy.clear();
}{
    mAudioCommandThread->exit();
    mOutputCommandThread->exit();
    mDestroyAudioPolicyManager(mAudioPolicyManager);
    unloadAudioPolicyManager();
    delete mAudioPolicyClient;
    mNotificationClients.clear();
    mAudioPolicyEffects.clear();
    mUidPolicy->unregisterSelf();
    mSensorPrivacyPolicy->unregisterSelf();
    mUidPolicy.clear();
    mSensorPrivacyPolicy.clear();
}
Status AudioPolicyService::registerClient(const sp<media::IAudioPolicyServiceClient>& client)
{
    if (client == 0) {
        ALOGW("%s got NULL client", __FUNCTION__);
        return Status::ok();
    }
    Mutex::Autolock _l(mNotificationClientsLock);
    uid_t uid = IPCThreadState::self()->getCallingUid();
    pid_t pid = IPCThreadState::self()->getCallingPid();
    int64_t token = ((int64_t)uid<<32) | pid;
    if (mNotificationClients.indexOfKey(token) < 0) {
        sp<NotificationClient> notificationClient = new NotificationClient(this,
                                                                           client,
                                                                           uid,
                                                                           pid);
        ALOGV("registerClient() client %p, uid %d pid %d", client.get(), uid, pid);
        mNotificationClients.add(token, notificationClient);
        sp<IBinder> binder = IInterface::asBinder(client);
        binder->linkToDeath(notificationClient);
    }
        return Status::ok();
}
Status AudioPolicyService::setAudioPortCallbacksEnabled(bool enabled)
{
    Mutex::Autolock _l(mNotificationClientsLock);
    uid_t uid = IPCThreadState::self()->getCallingUid();
    pid_t pid = IPCThreadState::self()->getCallingPid();
    int64_t token = ((int64_t)uid<<32) | pid;
    if (mNotificationClients.indexOfKey(token) < 0) {
        return Status::ok();
    }
    mNotificationClients.valueFor(token)->setAudioPortCallbacksEnabled(enabled);
    return Status::ok();
}
Status AudioPolicyService::setAudioVolumeGroupCallbacksEnabled(bool enabled)
{
    Mutex::Autolock _l(mNotificationClientsLock);
    uid_t uid = IPCThreadState::self()->getCallingUid();
    pid_t pid = IPCThreadState::self()->getCallingPid();
    int64_t token = ((int64_t)uid<<32) | pid;
    if (mNotificationClients.indexOfKey(token) < 0) {
        return Status::ok();
    }
    mNotificationClients.valueFor(token)->setAudioVolumeGroupCallbacksEnabled(enabled);
    return Status::ok();
}
void AudioPolicyService::removeNotificationClient(uid_t uid, pid_t pid)
{
    bool hasSameUid = false;
    {
        Mutex::Autolock _l(mNotificationClientsLock);
        int64_t token = ((int64_t)uid<<32) | pid;
        mNotificationClients.removeItem(token);
        for (size_t i = 0; i < mNotificationClients.size(); i++) {
            if (mNotificationClients.valueAt(i)->uid() == uid) {
                hasSameUid = true;
                break;
            }
        }
    }
    {
        Mutex::Autolock _l(mLock);
        if (mAudioPolicyManager && !hasSameUid) {
            mAudioPolicyManager->releaseResourcesForUid(uid);
        }
    }
}
void AudioPolicyService::onAudioPortListUpdate()
{
    mOutputCommandThread->updateAudioPortListCommand();
}
void AudioPolicyService::doOnAudioPortListUpdate()
{
    Mutex::Autolock _l(mNotificationClientsLock);
    for (size_t i = 0; i < mNotificationClients.size(); i++) {
        mNotificationClients.valueAt(i)->onAudioPortListUpdate();
    }
}
void AudioPolicyService::onAudioPatchListUpdate()
{
    mOutputCommandThread->updateAudioPatchListCommand();
}
void AudioPolicyService::doOnAudioPatchListUpdate()
{
    Mutex::Autolock _l(mNotificationClientsLock);
    for (size_t i = 0; i < mNotificationClients.size(); i++) {
        mNotificationClients.valueAt(i)->onAudioPatchListUpdate();
    }
}
void AudioPolicyService::onAudioVolumeGroupChanged(volume_group_t group, int flags)
{
    mOutputCommandThread->changeAudioVolumeGroupCommand(group, flags);
}
void AudioPolicyService::doOnAudioVolumeGroupChanged(volume_group_t group, int flags)
{
    Mutex::Autolock _l(mNotificationClientsLock);
    for (size_t i = 0; i < mNotificationClients.size(); i++) {
        mNotificationClients.valueAt(i)->onAudioVolumeGroupChanged(group, flags);
    }
}
void AudioPolicyService::onDynamicPolicyMixStateUpdate(const String8& regId, int32_t state)
{
    ALOGV("AudioPolicyService::onDynamicPolicyMixStateUpdate(%s, %d)",
            regId.string(), state);
    mOutputCommandThread->dynamicPolicyMixStateUpdateCommand(regId, state);
}
void AudioPolicyService::doOnDynamicPolicyMixStateUpdate(const String8& regId, int32_t state)
{
    Mutex::Autolock _l(mNotificationClientsLock);
    for (size_t i = 0; i < mNotificationClients.size(); i++) {
        mNotificationClients.valueAt(i)->onDynamicPolicyMixStateUpdate(regId, state);
    }
}
void AudioPolicyService::onRecordingConfigurationUpdate(
                                                    int event,
                                                    const record_client_info_t *clientInfo,
                                                    const audio_config_base_t *clientConfig,
                                                    std::vector<effect_descriptor_t> clientEffects,
                                                    const audio_config_base_t *deviceConfig,
                                                    std::vector<effect_descriptor_t> effects,
                                                    audio_patch_handle_t patchHandle,
                                                    audio_source_t source)
{
    mOutputCommandThread->recordingConfigurationUpdateCommand(event, clientInfo,
            clientConfig, clientEffects, deviceConfig, effects, patchHandle, source);
}
void AudioPolicyService::doOnRecordingConfigurationUpdate(
                                                  int event,
                                                  const record_client_info_t *clientInfo,
                                                  const audio_config_base_t *clientConfig,
                                                  std::vector<effect_descriptor_t> clientEffects,
                                                  const audio_config_base_t *deviceConfig,
                                                  std::vector<effect_descriptor_t> effects,
                                                  audio_patch_handle_t patchHandle,
                                                  audio_source_t source)
{
    Mutex::Autolock _l(mNotificationClientsLock);
    for (size_t i = 0; i < mNotificationClients.size(); i++) {
        mNotificationClients.valueAt(i)->onRecordingConfigurationUpdate(event, clientInfo,
                clientConfig, clientEffects, deviceConfig, effects, patchHandle, source);
    }
}
void AudioPolicyService::onRoutingUpdated()
{
    mOutputCommandThread->routingChangedCommand();
}
void AudioPolicyService::doOnRoutingUpdated()
{
  Mutex::Autolock _l(mNotificationClientsLock);
    for (size_t i = 0; i < mNotificationClients.size(); i++) {
        mNotificationClients.valueAt(i)->onRoutingUpdated();
    }
}
void AudioPolicyService::onCheckSpatializer()
{
    Mutex::Autolock _l(mLock);
    onCheckSpatializer_l();
}
void AudioPolicyService::onCheckSpatializer_l()
{
    if (mSpatializer != nullptr) {
        mOutputCommandThread->checkSpatializerCommand();
    }
}
void AudioPolicyService::doOnCheckSpatializer()
{
    Mutex::Autolock _l(mLock);
    if (mSpatializer != nullptr) {
        if (mSpatializer->getLevel() != media::SpatializationLevel::NONE) {
            audio_io_handle_t currentOutput = mSpatializer->getOutput();
            audio_io_handle_t newOutput;
            const audio_attributes_t attr = attributes_initializer(AUDIO_USAGE_MEDIA);
            audio_config_base_t config = mSpatializer->getAudioInConfig();
            status_t status =
                    mAudioPolicyManager->getSpatializerOutput(&config, &attr, &newOutput);
            if (status == NO_ERROR && currentOutput == newOutput) {
                return;
            }
            mLock.unlock();
            mSpatializer->detachOutput();
            if (status != NO_ERROR || newOutput == AUDIO_IO_HANDLE_NONE) {
                mLock.lock();
                return;
            }
            status = mSpatializer->attachOutput(newOutput);
            mLock.lock();
            if (status != NO_ERROR) {
                mAudioPolicyManager->releaseSpatializerOutput(newOutput);
            }
        } else if (mSpatializer->getLevel() == media::SpatializationLevel::NONE
                               && mSpatializer->getOutput() != AUDIO_IO_HANDLE_NONE) {
            mLock.unlock();
            audio_io_handle_t output = mSpatializer->detachOutput();
            mLock.lock();
            if (output != AUDIO_IO_HANDLE_NONE) {
                mAudioPolicyManager->releaseSpatializerOutput(output);
            }
        }
    }
}
status_t AudioPolicyService::clientCreateAudioPatch(const struct audio_patch *patch,
                                                audio_patch_handle_t *handle,
                                                int delayMs)
{
    return mAudioCommandThread->createAudioPatchCommand(patch, handle, delayMs);
}
status_t AudioPolicyService::clientReleaseAudioPatch(audio_patch_handle_t handle,
                                                 int delayMs)
{
    return mAudioCommandThread->releaseAudioPatchCommand(handle, delayMs);
}
status_t AudioPolicyService::clientSetAudioPortConfig(const struct audio_port_config *config,
                                                      int delayMs)
{
    return mAudioCommandThread->setAudioPortConfigCommand(config, delayMs);
}
AudioPolicyService::NotificationClient::NotificationClient(
        const sp<AudioPolicyService>& service,
        const sp<media::IAudioPolicyServiceClient>& client,
        uid_t uid,
        pid_t pid)
    : mService(service), mUid(uid), mPid(pid), mAudioPolicyServiceClient(client),
      mAudioPortCallbacksEnabled(false), mAudioVolumeGroupCallbacksEnabled(false)
{
}
AudioPolicyService::NotificationClient::~NotificationClient()
{
}
void AudioPolicyService::NotificationClient::binderDied(const wp<IBinder>& who __unused)
{
    sp<NotificationClient> keep(this);
    sp<AudioPolicyService> service = mService.promote();
    if (service != 0) {
        service->removeNotificationClient(mUid, mPid);
    }
}
void AudioPolicyService::NotificationClient::onAudioPortListUpdate()
{
    if (mAudioPolicyServiceClient != 0 && mAudioPortCallbacksEnabled) {
        mAudioPolicyServiceClient->onAudioPortListUpdate();
    }
}
void AudioPolicyService::NotificationClient::onAudioPatchListUpdate()
{
    if (mAudioPolicyServiceClient != 0 && mAudioPortCallbacksEnabled) {
        mAudioPolicyServiceClient->onAudioPatchListUpdate();
    }
}
void AudioPolicyService::NotificationClient::onAudioVolumeGroupChanged(volume_group_t group,
                                                                      int flags)
{
    if (mAudioPolicyServiceClient != 0 && mAudioVolumeGroupCallbacksEnabled) {
        mAudioPolicyServiceClient->onAudioVolumeGroupChanged(group, flags);
    }
}
void AudioPolicyService::NotificationClient::onDynamicPolicyMixStateUpdate(
        const String8& regId, int32_t state)
{
    if (mAudioPolicyServiceClient != 0 && isServiceUid(mUid)) {
        mAudioPolicyServiceClient->onDynamicPolicyMixStateUpdate(
                legacy2aidl_String8_string(regId).value(), state);
    }
}
void AudioPolicyService::NotificationClient::onRecordingConfigurationUpdate(
                                            int event,
                                            const record_client_info_t *clientInfo,
                                            const audio_config_base_t *clientConfig,
                                            std::vector<effect_descriptor_t> clientEffects,
                                            const audio_config_base_t *deviceConfig,
                                            std::vector<effect_descriptor_t> effects,
                                            audio_patch_handle_t patchHandle,
                                            audio_source_t source)
{
    if (mAudioPolicyServiceClient != 0 && isServiceUid(mUid)) {
        status_t status = [&]() -> status_t {
            int32_t eventAidl = VALUE_OR_RETURN_STATUS(convertIntegral<int32_t>(event));
            media::RecordClientInfo clientInfoAidl = VALUE_OR_RETURN_STATUS(
                    legacy2aidl_record_client_info_t_RecordClientInfo(*clientInfo));
            media::AudioConfigBase clientConfigAidl = VALUE_OR_RETURN_STATUS(
                    legacy2aidl_audio_config_base_t_AudioConfigBase(*clientConfig));
            std::vector<media::EffectDescriptor> clientEffectsAidl = VALUE_OR_RETURN_STATUS(
                    convertContainer<std::vector<media::EffectDescriptor>>(
                            clientEffects,
                            legacy2aidl_effect_descriptor_t_EffectDescriptor));
            media::AudioConfigBase deviceConfigAidl = VALUE_OR_RETURN_STATUS(
                    legacy2aidl_audio_config_base_t_AudioConfigBase(*deviceConfig));
            std::vector<media::EffectDescriptor> effectsAidl = VALUE_OR_RETURN_STATUS(
                    convertContainer<std::vector<media::EffectDescriptor>>(
                            effects,
                            legacy2aidl_effect_descriptor_t_EffectDescriptor));
            int32_t patchHandleAidl = VALUE_OR_RETURN_STATUS(
                    legacy2aidl_audio_patch_handle_t_int32_t(patchHandle));
            media::AudioSourceType sourceAidl = VALUE_OR_RETURN_STATUS(
                    legacy2aidl_audio_source_t_AudioSourceType(source));
            return aidl_utils::statusTFromBinderStatus(
                    mAudioPolicyServiceClient->onRecordingConfigurationUpdate(eventAidl,
                                                                              clientInfoAidl,
                                                                              clientConfigAidl,
                                                                              clientEffectsAidl,
                                                                              deviceConfigAidl,
                                                                              effectsAidl,
                                                                              patchHandleAidl,
                                                                              sourceAidl));
        }();
        ALOGW_IF(status != OK, "onRecordingConfigurationUpdate() failed: %d", status);
    }
}
void AudioPolicyService::NotificationClient::setAudioPortCallbacksEnabled(bool enabled)
{
    mAudioPortCallbacksEnabled = enabled;
}
void AudioPolicyService::NotificationClient::setAudioVolumeGroupCallbacksEnabled(bool enabled)
{
    mAudioVolumeGroupCallbacksEnabled = enabled;
}
void AudioPolicyService::NotificationClient::onRoutingUpdated()
{
    if (mAudioPolicyServiceClient != 0 && isServiceUid(mUid)) {
        mAudioPolicyServiceClient->onRoutingUpdated();
    }
}
void AudioPolicyService::binderDied(const wp<IBinder>& who) {
    ALOGW("binderDied() %p, calling pid %d", who.unsafe_get(),
            IPCThreadState::self()->getCallingPid());
}
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
AudioPolicyService::AudioCommandThread::~AudioCommandThread()
{
    if (!mAudioCommands.isEmpty()) {
        release_wake_lock(mName.string());
    }
    mAudioCommands.clear();
}
AudioPolicyService::AudioCommandThread::~AudioCommandThread()
{
    if (!mAudioCommands.isEmpty()) {
        release_wake_lock(mName.string());
    }
    mAudioCommands.clear();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
AudioPolicyService::AudioCommandThread::~AudioCommandThread()
{
    if (!mAudioCommands.isEmpty()) {
        release_wake_lock(mName.string());
    }
    mAudioCommands.clear();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
AudioPolicyService::AudioCommandThread::~AudioCommandThread()
{
    if (!mAudioCommands.isEmpty()) {
        release_wake_lock(mName.string());
    }
    mAudioCommands.clear();
}
AudioPolicyService::AudioCommandThread::~AudioCommandThread()
{
    if (!mAudioCommands.isEmpty()) {
        release_wake_lock(mName.string());
    }
    mAudioCommands.clear();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
Status AudioPolicyService::onNewAudioModulesAvailable()
{
    mOutputCommandThread->audioModulesUpdateCommand();
    return Status::ok();
}
extern "C" {
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
}
