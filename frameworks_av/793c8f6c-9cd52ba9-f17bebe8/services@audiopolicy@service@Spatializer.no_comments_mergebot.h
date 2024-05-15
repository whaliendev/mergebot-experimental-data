#ifndef ANDROID_MEDIA_SPATIALIZER_H
#define ANDROID_MEDIA_SPATIALIZER_H 
#include <android-base/stringprintf.h>
#include <android/media/BnEffect.h>
#include <android/media/BnSpatializer.h>
#include <android/media/SpatializationLevel.h>
#include <android/media/SpatializationMode.h>
#include <android/media/SpatializerHeadTrackingMode.h>
#include <android/media/audio/common/AudioLatencyMode.h>
#include <audio_utils/SimpleLog.h>
#include <math.h>
#include <media/AudioEffect.h>
#include <media/VectorRecorder.h>
#include <media/audiohal/EffectHalInterface.h>
#include <media/stagefright/foundation/ALooper.h>
#include <system/audio_effects/effect_spatializer.h>
#include <string>
#include "SpatializerPoseController.h"
namespace android {
class SpatializerPolicyCallback {
  public:
    virtual void onCheckSpatializer() = 0;
    virtual ~SpatializerPolicyCallback()
};
class Spatializer : public media::BnSpatializer,
                    public IBinder::DeathRecipient,
                    private SpatializerPoseController::Listener,
                    public virtual AudioSystem::SupportedLatencyModesCallback {
  public:
    static sp<Spatializer> create(SpatializerPolicyCallback* callback);
    ~Spatializer() override;
    void onFirstRef();
    binder::Status release() override;
    binder::Status getSupportedLevels(std::vector<media::SpatializationLevel>* levels) override;
    binder::Status setLevel(media::SpatializationLevel level) override;
    binder::Status getLevel(media::SpatializationLevel* level) override;
    binder::Status isHeadTrackingSupported(bool* supports);
    binder::Status getSupportedHeadTrackingModes(
            std::vector<media::SpatializerHeadTrackingMode>* modes) override;
    binder::Status setDesiredHeadTrackingMode(media::SpatializerHeadTrackingMode mode) override;
    binder::Status getActualHeadTrackingMode(media::SpatializerHeadTrackingMode* mode) override;
    binder::Status recenterHeadTracker() override;
    binder::Status setGlobalTransform(const std::vector<float>& screenToStage) override;
    binder::Status setHeadSensor(int sensorHandle) override;
    binder::Status setScreenSensor(int sensorHandle) override;
    binder::Status setDisplayOrientation(float physicalToLogicalAngle) override;
    binder::Status setHingeAngle(float hingeAngle) override;
    binder::Status getSupportedModes(std::vector<media::SpatializationMode>* modes) override;
    binder::Status registerHeadTrackingCallback(
            const sp<media::ISpatializerHeadTrackingCallback>& callback) override;
    binder::Status setParameter(int key, const std::vector<unsigned char>& value) override;
    binder::Status getParameter(int key, std::vector<unsigned char>* value) override;
    binder::Status getOutput(int* output);
    virtual void binderDied(const wp<IBinder>& who);
    void onSupportedLatencyModesChanged(audio_io_handle_t output,
                                        const std::vector<audio_latency_mode_t>& modes) override;
    status_t registerCallback(const sp<media::INativeSpatializerCallback>& callback);
    status_t loadEngineConfiguration(sp<EffectHalInterface> effect);
    media::SpatializationLevel getLevel() const {
        std::lock_guard lock(mLock);
        return mLevel;
    }
    status_t attachOutput(audio_io_handle_t output, size_t numActiveTracks);
    audio_io_handle_t detachOutput();
    audio_io_handle_t getOutput() const {
        std::lock_guard lock(mLock);
        return mOutput;
    }
    void updateActiveTracks(size_t numActiveTracks);
    audio_config_base_t getAudioInConfig() const;
    void calculateHeadPose();
    NO_THREAD_SAFETY_ANALYSIS;
    NO_THREAD_SAFETY_ANALYSIS;
    static std::string toString(audio_latency_mode_t mode) {
        const auto result = legacy2aidl_audio_latency_mode_t_AudioLatencyMode(mode);
        return result.has_value() ? media::audio::common::toString(*result)
                                  : "unknown_latency_mode";
    }
    static void sendEmptyCreateSpatializerMetricWithStatus(status_t status);
  private:
    Spatializer(effect_descriptor_t engineDescriptor, SpatializerPolicyCallback* callback);
    static void engineCallback(int32_t event, void* user, void* info);
    void onHeadToStagePose(const media::Pose3f& headToStage) override;
    void onActualModeChange(media::HeadTrackingMode mode) override;
    void onHeadToStagePoseMsg(const std::vector<float>& headToStage);
    void onActualModeChangeMsg(media::HeadTrackingMode mode);
    void onSupportedLatencyModesChangedMsg(audio_io_handle_t output,
                                           std::vector<audio_latency_mode_t>&& modes);
    static constexpr int kMaxEffectParamValues = 10;
    template <bool MULTI_VALUES, typename T>
    status_t getHalParameter(sp<EffectHalInterface> effect, uint32_t type, std::vector<T>* values) {
        static_assert(sizeof(T) <= sizeof(uint32_t), "The size of T must less than 32 bits");
        uint32_t cmd[sizeof(effect_param_t) / sizeof(uint32_t) + 1];
        uint32_t reply[sizeof(effect_param_t) / sizeof(uint32_t) + 2 + kMaxEffectParamValues];
        effect_param_t* p = (effect_param_t*)cmd;
        p->psize = sizeof(uint32_t);
        if (MULTI_VALUES) {
            p->vsize = (kMaxEffectParamValues + 1) * sizeof(T);
        } else {
            p->vsize = sizeof(T);
        }
        *(uint32_t*)p->data = type;
        uint32_t replySize = sizeof(effect_param_t) + p->psize + p->vsize;
        status_t status =
                effect->command(EFFECT_CMD_GET_PARAM, sizeof(effect_param_t) + sizeof(uint32_t),
                                cmd, &replySize, reply);
        if (status != NO_ERROR) {
            return status;
        }
        if (p->status != NO_ERROR) {
            return p->status;
        }
        if (replySize <
            sizeof(effect_param_t) + sizeof(uint32_t) + (MULTI_VALUES ? 2 : 1) * sizeof(T)) {
            return BAD_VALUE;
        }
        T* params = (T*)((uint8_t*)reply + sizeof(effect_param_t) + sizeof(uint32_t));
        int numParams = 1;
        if (MULTI_VALUES) {
            numParams = (int)*params++;
        }
        if (numParams > kMaxEffectParamValues) {
            return BAD_VALUE;
        }
        (*values).clear();
        std::copy(&params[0], &params[numParams], back_inserter(*values));
        return NO_ERROR;
    }
    REQUIRES(mLock) {
        static_assert(sizeof(T) <= sizeof(uint32_t), "The size of T must less than 32 bits");
        uint32_t cmd[sizeof(effect_param_t) / sizeof(uint32_t) + 1 + values.size()];
        effect_param_t* p = (effect_param_t*)cmd;
        p->psize = sizeof(uint32_t);
        p->vsize = sizeof(T) * values.size();
        *(uint32_t*)p->data = type;
        memcpy((uint32_t*)p->data + 1, values.data(), sizeof(T) * values.size());
        status_t status = mEngine->setParameter(p);
        if (status != NO_ERROR) {
            return status;
        }
        if (p->status != NO_ERROR) {
            return p->status;
        }
        return NO_ERROR;
    }
    REQUIRES(mLock) {
        static_assert(sizeof(T) <= sizeof(uint32_t), "The size of T must less than 32 bits");
        uint32_t cmd[sizeof(effect_param_t) / sizeof(uint32_t) + 1 + values->size()];
        effect_param_t* p = (effect_param_t*)cmd;
        p->psize = sizeof(uint32_t);
        p->vsize = sizeof(T) * values->size();
        *(uint32_t*)p->data = type;
        status_t status = mEngine->getParameter(p);
        if (status != NO_ERROR) {
            return status;
        }
        if (p->status != NO_ERROR) {
            return p->status;
        }
        int numValues = std::min(p->vsize / sizeof(T), values->size());
        (*values).clear();
        T* retValues = (T*)((uint8_t*)p->data + sizeof(uint32_t));
        std::copy(&retValues[0], &retValues[numValues], back_inserter(*values));
        return NO_ERROR;
    }
    void postFramesProcessedMsg(int frames);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    REQUIRES(mLock);
    const effect_descriptor_t mEngineDescriptor;
    SpatializerPolicyCallback* const mPolicyCallback;
    static constexpr const char* kDefaultMetricsId = AMEDIAMETRICS_KEY_PREFIX_AUDIO_SPATIALIZER "0";
    const std::string mMetricsId = kDefaultMetricsId;
    GUARDED_BY(mLock) = kDisplayOrientationInvalid;
    const std::vector<const char*> Spatializer::sHeadPoseKeys = {
            Spatializer::EngineCallbackHandler::kTranslation0Key,
            Spatializer::EngineCallbackHandler::kTranslation1Key,
            Spatializer::EngineCallbackHandler::kTranslation2Key,
            Spatializer::EngineCallbackHandler::kRotation0Key,
            Spatializer::EngineCallbackHandler::kRotation1Key,
            Spatializer::EngineCallbackHandler::kRotation2Key,
    };
    audio_io_handle_t mOutput GUARDED_BY(mLock) = AUDIO_IO_HANDLE_NONE;
    sp<media::INativeSpatializerCallback> mSpatializerCallbackconst std::vector<const char*>
            Spatializer::sHeadPoseKeys = {
                    Spatializer::EngineCallbackHandler::kTranslation0Key,
                    Spatializer::EngineCallbackHandler::kTranslation1Key,
                    Spatializer::EngineCallbackHandler::kTranslation2Key,
                    Spatializer::EngineCallbackHandler::kRotation0Key,
                    Spatializer::EngineCallbackHandler::kRotation1Key,
                    Spatializer::EngineCallbackHandler::kRotation2Key,
            };
    sp<media::ISpatializerHeadTrackingCallback> mHeadTrackingCallbackconst std::vector<const char*>
            Spatializer::sHeadPoseKeys = {
                    Spatializer::EngineCallbackHandler::kTranslation0Key,
                    Spatializer::EngineCallbackHandler::kTranslation1Key,
                    Spatializer::EngineCallbackHandler::kTranslation2Key,
                    Spatializer::EngineCallbackHandler::kRotation0Key,
                    Spatializer::EngineCallbackHandler::kRotation1Key,
                    Spatializer::EngineCallbackHandler::kRotation2Key,
            };
    const std::vector<const char*> Spatializer::sHeadPoseKeys = {
            Spatializer::EngineCallbackHandler::kTranslation0Key,
            Spatializer::EngineCallbackHandler::kTranslation1Key,
            Spatializer::EngineCallbackHandler::kTranslation2Key,
            Spatializer::EngineCallbackHandler::kRotation0Key,
            Spatializer::EngineCallbackHandler::kRotation1Key,
            Spatializer::EngineCallbackHandler::kRotation2Key,
    };
    std::shared_ptr<SpatializerPoseController> mPoseControllerconst std::vector<const char*>
            Spatializer::sHeadPoseKeys = {
                    Spatializer::EngineCallbackHandler::kTranslation0Key,
                    Spatializer::EngineCallbackHandler::kTranslation1Key,
                    Spatializer::EngineCallbackHandler::kTranslation2Key,
                    Spatializer::EngineCallbackHandler::kRotation0Key,
                    Spatializer::EngineCallbackHandler::kRotation1Key,
                    Spatializer::EngineCallbackHandler::kRotation2Key,
            };
    media::HeadTrackingMode mDesiredHeadTrackingMode GUARDED_BY(mLock) = kDisplayOrientationInvalid;
    media::SpatializerHeadTrackingMode mActualHeadTrackingModeconst std::vector<const char*>
            Spatializer::sHeadPoseKeys = {
                    Spatializer::EngineCallbackHandler::kTranslation0Key,
                    Spatializer::EngineCallbackHandler::kTranslation1Key,
                    Spatializer::EngineCallbackHandler::kTranslation2Key,
                    Spatializer::EngineCallbackHandler::kRotation0Key,
                    Spatializer::EngineCallbackHandler::kRotation1Key,
                    Spatializer::EngineCallbackHandler::kRotation2Key,
            };
    int32_t mHeadSensorconst std::vector<const char*> Spatializer::sHeadPoseKeys = {
            Spatializer::EngineCallbackHandler::kTranslation0Key,
            Spatializer::EngineCallbackHandler::kTranslation1Key,
            Spatializer::EngineCallbackHandler::kTranslation2Key,
            Spatializer::EngineCallbackHandler::kRotation0Key,
            Spatializer::EngineCallbackHandler::kRotation1Key,
            Spatializer::EngineCallbackHandler::kRotation2Key,
    };
    int32_t mScreenSensorconst std::vector<const char*> Spatializer::sHeadPoseKeys = {
            Spatializer::EngineCallbackHandler::kTranslation0Key,
            Spatializer::EngineCallbackHandler::kTranslation1Key,
            Spatializer::EngineCallbackHandler::kTranslation2Key,
            Spatializer::EngineCallbackHandler::kRotation0Key,
            Spatializer::EngineCallbackHandler::kRotation1Key,
            Spatializer::EngineCallbackHandler::kRotation2Key,
    };
    static constexpr float kDisplayOrientationInvalid = 1000;
    float mDisplayOrientation GUARDED_BY(mLock) = kDisplayOrientationInvalid;
    std::vector<media::SpatializationLevel> mLevels;
    std::vector<media::SpatializerHeadTrackingMode> mHeadTrackingModes;
    std::vector<media::SpatializationMode> mSpatializationModes;
    std::vector<audio_channel_mask_t> mChannelMasks;
    bool mSupportsHeadTracking;
    class EngineCallbackHandler;
    sp<ALooper> mLooper;
    sp<EngineCallbackHandler> mHandler;
    size_t mNumActiveTracksconst std::vector<const char*> Spatializer::sHeadPoseKeys = {
            Spatializer::EngineCallbackHandler::kTranslation0Key,
            Spatializer::EngineCallbackHandler::kTranslation1Key,
            Spatializer::EngineCallbackHandler::kTranslation2Key,
            Spatializer::EngineCallbackHandler::kRotation0Key,
            Spatializer::EngineCallbackHandler::kRotation1Key,
            Spatializer::EngineCallbackHandler::kRotation2Key,
    };
    std::vector<audio_latency_mode_t> mSupportedLatencyModesconst std::vector<const char*>
            Spatializer::sHeadPoseKeys = {
                    Spatializer::EngineCallbackHandler::kTranslation0Key,
                    Spatializer::EngineCallbackHandler::kTranslation1Key,
                    Spatializer::EngineCallbackHandler::kTranslation2Key,
                    Spatializer::EngineCallbackHandler::kRotation0Key,
                    Spatializer::EngineCallbackHandler::kRotation1Key,
                    Spatializer::EngineCallbackHandler::kRotation2Key,
            };
    static const std::vector<const char*> sHeadPoseKeys;
    static constexpr int mMaxLocalLogLine = 10;
    SimpleLog mLocalLog{mMaxLocalLogLine};
    media::VectorRecorder mPoseRecorderconst std::vector<const char*> Spatializer::sHeadPoseKeys = {
            Spatializer::EngineCallbackHandler::kTranslation0Key,
            Spatializer::EngineCallbackHandler::kTranslation1Key,
            Spatializer::EngineCallbackHandler::kTranslation2Key,
            Spatializer::EngineCallbackHandler::kRotation0Key,
            Spatializer::EngineCallbackHandler::kRotation1Key,
            Spatializer::EngineCallbackHandler::kRotation2Key,
    };
    media::VectorRecorder mPoseDurableRecorderconst std::vector<const char*>
            Spatializer::sHeadPoseKeys = {
                    Spatializer::EngineCallbackHandler::kTranslation0Key,
                    Spatializer::EngineCallbackHandler::kTranslation1Key,
                    Spatializer::EngineCallbackHandler::kTranslation2Key,
                    Spatializer::EngineCallbackHandler::kRotation0Key,
                    Spatializer::EngineCallbackHandler::kRotation1Key,
                    Spatializer::EngineCallbackHandler::kRotation2Key,
            };
};
}
#endif
