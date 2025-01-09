       
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
class AudioPolicyEffects : public RefBase {
  public:
    explicit AudioPolicyEffects(const sp<EffectsFactoryHalInterface>& effectsFactoryHal);
    status_t queryDefaultInputEffects(audio_session_t audioSession,
                                      effect_descriptor_t* descriptors, uint32_t* count) private
        : GUARDED_BY(mDeviceEffectsMutex);
  public:
    status_t addInputEffects(audio_io_handle_t input, audio_source_t inputSource,
                             audio_session_t audioSession) private
        : GUARDED_BY(mDeviceEffectsMutex);
  public:
    status_t releaseInputEffects(audio_io_handle_t input, audio_session_t audioSession) private
        : GUARDED_BY(mDeviceEffectsMutex);
  public:
    status_t queryDefaultOutputSessionEffects(audio_session_t audioSession,
                                              effect_descriptor_t* descriptors,
                                              uint32_t* count) private
        : GUARDED_BY(mDeviceEffectsMutex);
  public:
    status_t addOutputSessionEffects(audio_io_handle_t output, audio_stream_type_t stream,
                                     audio_session_t audioSession) private
        : GUARDED_BY(mDeviceEffectsMutex);
  public:
    status_t releaseOutputSessionEffects(audio_io_handle_t output, audio_stream_type_t stream,
                                         audio_session_t audioSession) private
        : GUARDED_BY(mDeviceEffectsMutex);
  public:
    status_t addSourceDefaultEffect(const effect_uuid_t* type, const String16& opPackageName,
                                    const effect_uuid_t* uuid, int32_t priority,
                                    audio_source_t source, audio_unique_id_t* id) private
        : GUARDED_BY(mDeviceEffectsMutex);
  public:
    status_t addStreamDefaultEffect(const effect_uuid_t* type, const String16& opPackageName,
                                    const effect_uuid_t* uuid, int32_t priority,
                                    audio_usage_t usage, audio_unique_id_t* id) private
        : GUARDED_BY(mDeviceEffectsMutex);
  public:
    status_t removeSourceDefaultEffect(audio_unique_id_t id) private
        : GUARDED_BY(mDeviceEffectsMutex);
  public:
    status_t removeStreamDefaultEffect(audio_unique_id_t id) private
        : GUARDED_BY(mDeviceEffectsMutex);
    class EffectDesc {
      public:
        EffectDesc(std::string_view name, const effect_uuid_t& typeUuid,
                   const String16& opPackageName, const effect_uuid_t& uuid, uint32_t priority,
                   audio_unique_id_t id)
            : mName(name),
              mTypeUuid(typeUuid),
              mOpPackageName(opPackageName),
              mUuid(uuid),
              mPriority(priority),
              mId(id) {}
        EffectDesc(std::string_view name, const effect_uuid_t& uuid)
            : EffectDesc(name, *EFFECT_UUID_NULL, String16(""), uuid, 0, AUDIO_UNIQUE_ID_ALLOCATE) {
        }
        EffectDesc(const EffectDesc& orig)
            : mName(orig.mName),
              mTypeUuid(orig.mTypeUuid),
              mOpPackageName(orig.mOpPackageName),
              mUuid(orig.mUuid),
              mPriority(orig.mPriority),
              mId(orig.mId),
              mParams(orig.mParams) {}
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
        DeviceEffects(std::unique_ptr<EffectDescVector> effectDescriptors, audio_devices_t device,
                      std::string_view address)
            : mEffectDescriptors(std::move(effectDescriptors)),
              mDeviceType(device),
              mDeviceAddress(address) {}
        std::vector<sp<AudioEffect>> mEffects;
        audio_devices_t getDeviceType() const { return mDeviceType; }
        std::string getDeviceAddress() const { return mDeviceAddress; }
        const std::unique_ptr<EffectDescVector> mEffectDescriptors;
      private:
        const audio_devices_t mDeviceType;
        const std::string mDeviceAddress;
    };
    EXCLUDES_EffectHandle_Mutex;
    EXCLUDES(mDeviceEffectsMutex) EXCLUDES_EffectHandle_Mutex;
    status_t loadAudioEffectConfig_ll(const sp<EffectsFactoryHalInterface>& effectsFactoryHal)
            REQUIRES(mMutex, mDeviceEffectsMutex);
    status_t loadAudioEffectConfigLegacy_l(const char* path) REQUIRES(mMutex);
    REQUIRES(mMutex);
    REQUIRES(mMutex);
    status_t loadInputEffectConfigurations_l(cnode* root, const Vector<EffectDesc*>& effects)
            REQUIRES(mMutex);
    status_t loadStreamEffectConfigurations_l(cnode* root, const Vector<EffectDesc*>& effects)
            REQUIRES(mMutex);
    static audio_source_t inputSourceNameToEnum(const char* name);
    static audio_stream_type_t streamNameToEnum(const char* name);
    static EffectDescVector loadEffects(cnode* root);
    static status_t loadEffects(cnode* root, Vector<EffectDesc*>& effects);
    static EffectDesc* loadEffect(cnode* root);
    static std::shared_ptr<EffectDescVector> loadEffectConfig(cnode* root,
                                                              const EffectDescVector& effects);
    static void loadEffectParameters(cnode* root,
                                     std::vector<std::shared_ptr<const effect_param_t>>& params);
    static void loadEffectParameters(cnode* root, Vector<effect_param_t*>& params);
<<<<<<< HEAD
    static std::shared_ptr<const effect_param_t> loadEffectParameter(cnode* root);
||||||| 99c77e683c
    effect_param_t* loadEffectParameter(cnode* root);
=======
    static effect_param_t* loadEffectParameter(cnode* root);
>>>>>>> b4efe510d24eaa8040fdc77aac895a7fdfa1cb3c
    static size_t readParamValue(cnode* node, char** param, size_t* curSize, size_t* totSize);
    static size_t growParamSize(char** param, size_t size, size_t* curSize, size_t* totSize);
<<<<<<< HEAD
||||||| 99c77e683c
=======
>>>>>>> b4efe510d24eaa8040fdc77aac895a7fdfa1cb3c
    mutable audio_utils::mutex mMutex{audio_utils::MutexOrder::kAudioPolicyEffects_Mutex};
<<<<<<< HEAD
    std::map<audio_source_t, std::shared_ptr<EffectDescVector>> mInputSources
||||||| 99c77e683c
    KeyedVector<audio_source_t, EffectDescVector*> mInputSources;
=======
    KeyedVector<audio_source_t, EffectDescVector*> mInputSources
>>>>>>> b4efe510d24eaa8040fdc77aac895a7fdfa1cb3c
            GUARDED_BY(mDeviceEffectsMutex);
<<<<<<< HEAD
    std::map<audio_session_t, std::shared_ptr<EffectVector>> mInputSessions
||||||| 99c77e683c
    KeyedVector<audio_session_t, EffectVector*> mInputSessions;
=======
    KeyedVector<audio_session_t, EffectVector*> mInputSessions
>>>>>>> b4efe510d24eaa8040fdc77aac895a7fdfa1cb3c
            GUARDED_BY(mDeviceEffectsMutex);
<<<<<<< HEAD
    std::map<audio_stream_type_t, std::shared_ptr<EffectDescVector>> mOutputStreams
||||||| 99c77e683c
    KeyedVector<audio_stream_type_t, EffectDescVector*> mOutputStreams;
=======
    KeyedVector<audio_stream_type_t, EffectDescVector*> mOutputStreams
>>>>>>> b4efe510d24eaa8040fdc77aac895a7fdfa1cb3c
            GUARDED_BY(mDeviceEffectsMutex);
<<<<<<< HEAD
    std::map<audio_session_t, std::shared_ptr<EffectVector>> mOutputSessions
||||||| 99c77e683c
    KeyedVector<audio_session_t, EffectVector*> mOutputSessions;
=======
    KeyedVector<audio_session_t, EffectVector*> mOutputSessions
>>>>>>> b4efe510d24eaa8040fdc77aac895a7fdfa1cb3c
            GUARDED_BY(mDeviceEffectsMutex);
    std::mutex mDeviceEffectsMutex;
    std::map<std::string, std::unique_ptr<DeviceEffects>> mDeviceEffects
            GUARDED_BY(mDeviceEffectsMutex);
};
}
