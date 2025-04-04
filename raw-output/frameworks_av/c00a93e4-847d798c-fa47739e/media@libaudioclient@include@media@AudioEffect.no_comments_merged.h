#ifndef ANDROID_AUDIOEFFECT_H
#define ANDROID_AUDIOEFFECT_H 
#include <stdint.h>
#include <sys/types.h>
#include <media/IAudioFlinger.h>
#include <media/IAudioPolicyService.h>
#include <media/IEffect.h>
#include <media/IEffectClient.h>
#include <media/AudioSystem.h>
#include <system/audio_effect.h>
#include <utils/RefBase.h>
#include <utils/Errors.h>
#include <binder/IInterface.h>
namespace android {
struct effect_param_cblk_t;
class AudioEffect : public RefBase
{
public:
    static status_t queryNumberEffects(uint32_t *numEffects);
    static status_t queryEffect(uint32_t index, effect_descriptor_t *descriptor);
    static status_t getEffectDescriptor(const effect_uuid_t *uuid,
                                        const effect_uuid_t *type,
                                        uint32_t preferredTypeFlag,
                                        effect_descriptor_t *descriptor);
    static status_t queryDefaultPreProcessing(audio_session_t audioSession,
                                              effect_descriptor_t *descriptors,
                                              uint32_t *count);
    static status_t newEffectUniqueId(audio_unique_id_t* id);
    static status_t addSourceDefaultEffect(const char* typeStr,
                                           const String16& opPackageName,
                                           const char* uuidStr,
                                           int32_t priority,
                                           audio_source_t source,
                                           audio_unique_id_t* id);
    static status_t addStreamDefaultEffect(const char* typeStr,
                                           const String16& opPackageName,
                                           const char* uuidStr,
                                           int32_t priority,
                                           audio_usage_t usage,
                                           audio_unique_id_t* id);
    static status_t removeSourceDefaultEffect(audio_unique_id_t id);
    static status_t removeStreamDefaultEffect(audio_unique_id_t id);
    enum event_type {
        EVENT_CONTROL_STATUS_CHANGED = 0,
        EVENT_ENABLE_STATUS_CHANGED = 1,
        EVENT_PARAMETER_CHANGED = 2,
        EVENT_ERROR = 3
    };
    typedef void (*effect_callback_t)(int32_t event, void* user, void *info);
    explicit AudioEffect(const String16& opPackageName);
                        ~AudioEffect();
            status_t set(const effect_uuid_t *type,
                            const effect_uuid_t *uuid = NULL,
                            int32_t priority = 0,
                            effect_callback_t cbf = NULL,
                            void* user = NULL,
                            audio_session_t sessionId = AUDIO_SESSION_OUTPUT_MIX,
                            audio_io_handle_t io = AUDIO_IO_HANDLE_NONE,
<<<<<<< HEAD
                            const AudioDeviceTypeAddr& device = {},
                            bool probe = false);
            status_t set(const char *typeStr,
                            const char *uuidStr = NULL,
                            int32_t priority = 0,
                            effect_callback_t cbf = NULL,
                            void* user = NULL,
                            audio_session_t sessionId = AUDIO_SESSION_OUTPUT_MIX,
                            audio_io_handle_t io = AUDIO_IO_HANDLE_NONE,
                            const AudioDeviceTypeAddr& device = {},
                            bool probe = false);
||||||| fa47739ebe
                            const AudioDeviceTypeAddr& device = {}
                            );
=======
                            const AudioDeviceTypeAddr& device = {});
            status_t set(const char *typeStr,
                            const char *uuidStr = NULL,
                            int32_t priority = 0,
                            effect_callback_t cbf = NULL,
                            void* user = NULL,
                            audio_session_t sessionId = AUDIO_SESSION_OUTPUT_MIX,
                            audio_io_handle_t io = AUDIO_IO_HANDLE_NONE,
                            const AudioDeviceTypeAddr& device = {});
>>>>>>> 847d798c
            status_t initCheck() const;
            int32_t id() const { return mId; }
            effect_descriptor_t descriptor() const;
            int32_t priority() const { return mPriority; }
    virtual status_t setEnabled(bool enabled);
            bool getEnabled() const;
     virtual status_t setParameter(effect_param_t *param);
     virtual status_t setParameterDeferred(effect_param_t *param);
     virtual status_t setParameterCommit();
     virtual status_t getParameter(effect_param_t *param);
     virtual status_t command(uint32_t cmdCode,
                              uint32_t cmdSize,
                              void *cmdData,
                              uint32_t *replySize,
                              void *replyData);
     static status_t stringToGuid(const char *str, effect_uuid_t *guid);
     static status_t guidToString(const effect_uuid_t *guid, char *str, size_t maxLen);
     static const uint32_t kMaxPreProcessing = 10;
protected:
<<<<<<< HEAD
     const String16 mOpPackageName;
     bool mEnabled = false;
     audio_session_t mSessionId = AUDIO_SESSION_OUTPUT_MIX;
     int32_t mPriority = 0;
     status_t mStatus = NO_INIT;
     bool mProbe = false;
     effect_callback_t mCbf = nullptr;
||||||| fa47739ebe
     bool mEnabled;
     audio_session_t mSessionId;
     int32_t mPriority;
     status_t mStatus;
     effect_callback_t mCbf;
=======
     const String16 mOpPackageName;
     bool mEnabled = false;
     audio_session_t mSessionId = AUDIO_SESSION_OUTPUT_MIX;
     int32_t mPriority = 0;
     status_t mStatus = NO_INIT;
     effect_callback_t mCbf = nullptr;
>>>>>>> 847d798c
     void* mUserData = nullptr;
     effect_descriptor_t mDescriptor = {};
     int32_t mId = -1;
     Mutex mLock;
     virtual void controlStatusChanged(bool controlGranted);
     virtual void enableStatusChanged(bool enabled);
     virtual void commandExecuted(uint32_t cmdCode,
             uint32_t cmdSize,
             void *pCmdData,
             uint32_t replySize,
             void *pReplyData);
private:
    class EffectClient :
        public android::BnEffectClient, public android::IBinder::DeathRecipient
    {
    public:
        EffectClient(AudioEffect *effect) : mEffect(effect){}
        virtual void controlStatusChanged(bool controlGranted) {
            sp<AudioEffect> effect = mEffect.promote();
            if (effect != 0) {
                effect->controlStatusChanged(controlGranted);
            }
        }
        virtual void enableStatusChanged(bool enabled) {
            sp<AudioEffect> effect = mEffect.promote();
            if (effect != 0) {
                effect->enableStatusChanged(enabled);
            }
        }
        virtual void commandExecuted(uint32_t cmdCode,
                                     uint32_t cmdSize,
                                     void *pCmdData,
                                     uint32_t replySize,
                                     void *pReplyData) {
            sp<AudioEffect> effect = mEffect.promote();
            if (effect != 0) {
                effect->commandExecuted(
                    cmdCode, cmdSize, pCmdData, replySize, pReplyData);
            }
        }
        virtual void binderDied(const wp<IBinder>& ) {
            sp<AudioEffect> effect = mEffect.promote();
            if (effect != 0) {
                effect->binderDied();
            }
        }
    private:
        wp<AudioEffect> mEffect;
    };
    void binderDied();
    sp<IEffect> mIEffect;
    sp<EffectClient> mIEffectClient;
    sp<IMemory> mCblkMemory;
<<<<<<< HEAD
    effect_param_cblk_t* mCblk = nullptr;
    pid_t mClientPid = (pid_t)-1;
    uid_t mClientUid = (uid_t)-1;
||||||| fa47739ebe
    effect_param_cblk_t* mCblk;
    pid_t mClientPid;
=======
    effect_param_cblk_t* mCblk = nullptr;
    pid_t mClientPid = (pid_t)-1;
>>>>>>> 847d798c
};
};
#endif
