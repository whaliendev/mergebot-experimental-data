       
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <future>
#include <android-base/thread_annotations.h>
#include <audio_utils/mutex.h>
#include <cutils/misc.h>
#include <media/AudioEffect.h>
#include <media/audiohal/EffectsFactoryHalInterface.h>
#include <system/audio.h>
#include <utils/Vector.h>
#include <utils/SortedVector.h>
namespace android {
class AudioPolicyEffects : public RefBase
{
public:
    explicit AudioPolicyEffects(const sp<EffectsFactoryHalInterface>& effectsFactoryHal);
<<<<<<< HEAD
||||||| 99c77e683c
    virtual ~AudioPolicyEffects();
=======
    ~AudioPolicyEffects() override;
>>>>>>> b4efe510
    status_t queryDefaultInputEffects(audio_session_t audioSession,
                             effect_descriptor_t *descriptors,
                             uint32_t* count) EXCLUDES_AudioPolicyEffects_Mutex;
    status_t addInputEffects(audio_io_handle_t input,
                             audio_source_t inputSource,
                             audio_session_t audioSession) EXCLUDES_AudioPolicyEffects_Mutex;
    status_t releaseInputEffects(audio_io_handle_t input,
                                 audio_session_t audioSession) EXCLUDES_AudioPolicyEffects_Mutex;
    status_t queryDefaultOutputSessionEffects(audio_session_t audioSession,
                             effect_descriptor_t *descriptors,
                             uint32_t* count) EXCLUDES_AudioPolicyEffects_Mutex;
    status_t addOutputSessionEffects(audio_io_handle_t output,
                             audio_stream_type_t stream,
                             audio_session_t audioSession) EXCLUDES_AudioPolicyEffects_Mutex;
    status_t releaseOutputSessionEffects(audio_io_handle_t output,
                             audio_stream_type_t stream,
                             audio_session_t audioSession) EXCLUDES_AudioPolicyEffects_Mutex;
    status_t addSourceDefaultEffect(const effect_uuid_t *type,
                                    const String16& opPackageName,
                                    const effect_uuid_t *uuid,
                                    int32_t priority,
                                    audio_source_t source,
                                    audio_unique_id_t* id) EXCLUDES_AudioPolicyEffects_Mutex;
    status_t addStreamDefaultEffect(const effect_uuid_t *type,
                                    const String16& opPackageName,
                                    const effect_uuid_t *uuid,
                                    int32_t priority,
                                    audio_usage_t usage,
                                    audio_unique_id_t* id) EXCLUDES_AudioPolicyEffects_Mutex;
    status_t removeSourceDefaultEffect(audio_unique_id_t id) EXCLUDES_AudioPolicyEffects_Mutex;
    status_t removeStreamDefaultEffect(audio_unique_id_t id) EXCLUDES_AudioPolicyEffects_Mutex;
<<<<<<< HEAD
    void initDefaultDeviceEffects() EXCLUDES(mDeviceEffectsMutex) EXCLUDES_EffectHandle_Mutex;
||||||| 99c77e683c
    void setDefaultDeviceEffects();
=======
    void setDefaultDeviceEffects();
>>>>>>> b4efe510
private:
    class EffectDesc {
    public:
        EffectDesc(std::string_view name,
                   const effect_uuid_t& typeUuid,
                   const String16& opPackageName,
                   const effect_uuid_t& uuid,
                   uint32_t priority,
                   audio_unique_id_t id) :
                        mName(name),
                        mTypeUuid(typeUuid),
                        mOpPackageName(opPackageName),
                        mUuid(uuid),
                        mPriority(priority),
                        mId(id) { }
        EffectDesc(std::string_view name, const effect_uuid_t& uuid) :
                        EffectDesc(name,
                                   *EFFECT_UUID_NULL,
                                   String16(""),
                                   uuid,
                                   0,
                                   AUDIO_UNIQUE_ID_ALLOCATE) { }
        EffectDesc(const EffectDesc& orig) :
                        mName(orig.mName),
                        mTypeUuid(orig.mTypeUuid),
                        mOpPackageName(orig.mOpPackageName),
                        mUuid(orig.mUuid),
                        mPriority(orig.mPriority),
                        mId(orig.mId),
                        mParams(orig.mParams) { }
        const std::string mName;
        const effect_uuid_t mTypeUuid;
        const String16 mOpPackageName;
        const effect_uuid_t mUuid;
        const int32_t mPriority;
        const audio_unique_id_t mId;
        std::vector<std::shared_ptr<const effect_param_t>> mParams;
    };
    using EffectDescVector = std::vector<std::shared_ptr<EffectDesc>>;
    class EffectVector {
    public:
        explicit EffectVector(audio_session_t session) : mSessionId(session) {}
        void setProcessorEnabled(bool enabled);
        const audio_session_t mSessionId;
        int mRefCount = 0;
        std::vector<sp<AudioEffect>> mEffects;
    };
    class DeviceEffects {
    public:
        DeviceEffects(std::unique_ptr<EffectDescVector> effectDescriptors,
                               audio_devices_t device, std::string_view address) :
            mEffectDescriptors(std::move(effectDescriptors)),
            mDeviceType(device), mDeviceAddress(address) {}
        std::vector<sp<AudioEffect>> mEffects;
        audio_devices_t getDeviceType() const { return mDeviceType; }
        std::string getDeviceAddress() const { return mDeviceAddress; }
        const std::unique_ptr<EffectDescVector> mEffectDescriptors;
    private:
        const audio_devices_t mDeviceType;
        const std::string mDeviceAddress;
    };
<<<<<<< HEAD
    status_t loadAudioEffectConfig_ll(const sp<EffectsFactoryHalInterface>& effectsFactoryHal)
            REQUIRES(mMutex, mDeviceEffectsMutex);
    status_t loadAudioEffectConfigLegacy_l(const char* path) REQUIRES(mMutex);
    status_t loadInputEffectConfigurations_l(cnode* root,
            const EffectDescVector& effects) REQUIRES(mMutex);
    status_t loadStreamEffectConfigurations_l(cnode* root,
            const EffectDescVector& effects) REQUIRES(mMutex);
||||||| 99c77e683c
    static const char * const kInputSourceNames[AUDIO_SOURCE_CNT -1];
=======
    void initDefaultDeviceEffects() EXCLUDES(mDeviceEffectsMutex) EXCLUDES_EffectHandle_Mutex;
    status_t loadAudioEffectConfig_ll(const sp<EffectsFactoryHalInterface>& effectsFactoryHal)
            REQUIRES(mMutex, mDeviceEffectsMutex);
    status_t loadAudioEffectConfigLegacy_l(const char* path) REQUIRES(mMutex);
    status_t loadInputEffectConfigurations_l(
            cnode* root, const Vector<EffectDesc*>& effects) REQUIRES(mMutex);
    status_t loadStreamEffectConfigurations_l(
            cnode* root, const Vector<EffectDesc*>& effects) REQUIRES(mMutex);
>>>>>>> b4efe510
    static audio_source_t inputSourceNameToEnum(const char *name);
    static audio_stream_type_t streamNameToEnum(const char* name);
<<<<<<< HEAD
    static EffectDescVector loadEffects(cnode* root);
    static std::shared_ptr<AudioPolicyEffects::EffectDesc> loadEffect(cnode* root);
    static std::shared_ptr<EffectDescVector> loadEffectConfig(cnode* root,
            const EffectDescVector& effects);
||||||| 99c77e683c
    status_t loadEffects(cnode *root, Vector <EffectDesc *>& effects);
    EffectDesc *loadEffect(cnode *root);
    status_t loadInputEffectConfigurations(cnode *root, const Vector <EffectDesc *>& effects);
    status_t loadStreamEffectConfigurations(cnode *root, const Vector <EffectDesc *>& effects);
    EffectDescVector *loadEffectConfig(cnode *root, const Vector <EffectDesc *>& effects);
=======
    static status_t loadEffects(cnode* root, Vector<EffectDesc*>& effects);
    static EffectDesc* loadEffect(cnode* root);
    static EffectDescVector *loadEffectConfig(cnode *root, const Vector <EffectDesc *>& effects);
>>>>>>> b4efe510
<<<<<<< HEAD
    static void loadEffectParameters(
            cnode* root, std::vector<std::shared_ptr<const effect_param_t>>& params);
    static std::shared_ptr<const effect_param_t> loadEffectParameter(cnode* root);
    static size_t readParamValue(cnode* node,
||||||| 99c77e683c
    void loadEffectParameters(cnode *root, Vector <effect_param_t *>& params);
    effect_param_t *loadEffectParameter(cnode *root);
    size_t readParamValue(cnode *node,
=======
    static void loadEffectParameters(cnode* root, Vector<effect_param_t*>& params);
    static effect_param_t* loadEffectParameter(cnode* root);
    static size_t readParamValue(cnode* node,
>>>>>>> b4efe510
                          char **param,
                          size_t *curSize,
                          size_t *totSize);
    static size_t growParamSize(char** param,
                         size_t size,
                         size_t *curSize,
                         size_t *totSize);
<<<<<<< HEAD
||||||| 99c77e683c
=======
>>>>>>> b4efe510
    mutable audio_utils::mutex mMutex{audio_utils::MutexOrder::kAudioPolicyEffects_Mutex};
<<<<<<< HEAD
    std::map<audio_source_t, std::shared_ptr<EffectDescVector>> mInputSources
            GUARDED_BY(mMutex);
    std::map<audio_session_t, std::shared_ptr<EffectVector>> mInputSessions
            GUARDED_BY(mMutex);
||||||| 99c77e683c
    KeyedVector< audio_source_t, EffectDescVector* > mInputSources;
    KeyedVector< audio_session_t, EffectVector* > mInputSessions;
=======
    KeyedVector<audio_source_t, EffectDescVector*> mInputSources GUARDED_BY(mMutex);
    KeyedVector<audio_session_t, EffectVector*> mInputSessions GUARDED_BY(mMutex);
>>>>>>> b4efe510
<<<<<<< HEAD
    std::map<audio_stream_type_t, std::shared_ptr<EffectDescVector>> mOutputStreams
            GUARDED_BY(mMutex);
    std::map<audio_session_t, std::shared_ptr<EffectVector>> mOutputSessions
            GUARDED_BY(mMutex);
||||||| 99c77e683c
    KeyedVector< audio_stream_type_t, EffectDescVector* > mOutputStreams;
    KeyedVector< audio_session_t, EffectVector* > mOutputSessions;
=======
    KeyedVector<audio_stream_type_t, EffectDescVector*> mOutputStreams GUARDED_BY(mMutex);
    KeyedVector<audio_session_t, EffectVector*> mOutputSessions GUARDED_BY(mMutex);
>>>>>>> b4efe510
<<<<<<< HEAD
||||||| 99c77e683c
    std::map<std::string, std::unique_ptr<DeviceEffects>> mDeviceEffects GUARDED_BY(mMutex);
=======
    std::mutex mDeviceEffectsMutex;
    std::map<std::string, std::unique_ptr<DeviceEffects>> mDeviceEffects
            GUARDED_BY(mDeviceEffectsMutex);
>>>>>>> b4efe510
<<<<<<< HEAD
    std::mutex mDeviceEffectsMutex;
    std::map<std::string, std::unique_ptr<DeviceEffects>> mDeviceEffects
            GUARDED_BY(mDeviceEffectsMutex);
||||||| 99c77e683c
    std::future<void> mDefaultDeviceEffectFuture;
=======
    std::future<void> mDefaultDeviceEffectFuture;
>>>>>>> b4efe510
};
}
