#ifndef ANDROID_MEDIA_SPATIALIZER_H
#define ANDROID_MEDIA_SPATIALIZER_H 
#include <android-base/stringprintf.h>
#include <android/media/BnEffect.h>
#include <android/media/BnSpatializer.h>
#include <android/media/audio/common/AudioLatencyMode.h>
#include <android/media/audio/common/HeadTracking.h>
#include <android/media/audio/common/Spatialization.h>
#include <audio_utils/mutex.h>
#include <audio_utils/SimpleLog.h>
#include <math.h>
#include <media/AudioEffect.h>
#include <media/MediaMetricsItem.h>
#include <media/audiohal/EffectsFactoryHalInterface.h>
#include <media/VectorRecorder.h>
#include <media/audiohal/EffectHalInterface.h>
#include <media/stagefright/foundation/ALooper.h>
#include <system/audio_effects/effect_spatializer.h>
#include <string>
#include <unordered_set>
#include "SpatializerPoseController.h"
namespace android {
class SpatializerPolicyCallback {
public:
    virtual void onCheckSpatializer() = 0;
    virtual ~SpatializerPolicyCallback() = default;
};
class Spatializer : public media::BnSpatializer,
                    public AudioEffect::IAudioEffectCallback,
                    public IBinder::DeathRecipient,
                    private SpatializerPoseController::Listener,
                    public virtual AudioSystem::SupportedLatencyModesCallback {
  public:
    static sp<Spatializer> create(SpatializerPolicyCallback* callback,
                                  const sp<EffectsFactoryHalInterface>& effectsFactoryHal);
           ~Spatializer() override;
    void onFirstRef();
    binder::Status release() override;
    binder::Status getSupportedLevels(
            std::vector<media::audio::common::Spatialization::Level>* levels) override;
    binder::Status setLevel(media::audio::common::Spatialization::Level level) override;
    binder::Status getLevel(media::audio::common::Spatialization::Level *level) override;
    binder::Status isHeadTrackingSupported(bool *supports);
    binder::Status getSupportedHeadTrackingModes(
            std::vector<media::audio::common::HeadTracking::Mode>* modes) override;
    binder::Status setDesiredHeadTrackingMode(
            media::audio::common::HeadTracking::Mode mode) override;
    binder::Status getActualHeadTrackingMode(
            media::audio::common::HeadTracking::Mode* mode) override;
    binder::Status recenterHeadTracker() override;
    binder::Status setGlobalTransform(const std::vector<float>& screenToStage) override;
    binder::Status setHeadSensor(int sensorHandle) override;
    binder::Status setScreenSensor(int sensorHandle) override;
    binder::Status setDisplayOrientation(float physicalToLogicalAngle) override;
    binder::Status setHingeAngle(float hingeAngle) override;
    binder::Status setFoldState(bool folded) override;
    binder::Status getSupportedModes(
            std::vector<media::audio::common::Spatialization::Mode>* modes) override;
    binder::Status registerHeadTrackingCallback(
        const sp<media::ISpatializerHeadTrackingCallback>& callback) override;
    binder::Status setParameter(int key, const std::vector<unsigned char>& value) override;
    binder::Status getParameter(int key, std::vector<unsigned char> *value) override;
    binder::Status getOutput(int *output);
    virtual void binderDied(const wp<IBinder>& who);
    void onSupportedLatencyModesChanged(
            audio_io_handle_t output, const std::vector<audio_latency_mode_t>& modes) override;
    status_t registerCallback(const sp<media::INativeSpatializerCallback>& callback);
    status_t loadEngineConfiguration(sp<EffectHalInterface> effect);
    media::audio::common::Spatialization::Level getLevel() const {
        audio_utils::lock_guard lock(mMutex);
        return mLevel;
    }
    std::unordered_set<media::audio::common::HeadTracking::ConnectionMode>
            getSupportedHeadtrackingConnectionModes() const {
        return mSupportedHeadtrackingConnectionModes;
    }
    media::audio::common::HeadTracking::ConnectionMode getHeadtrackingConnectionMode() const {
        return mHeadtrackingConnectionMode;
    }
    std::vector<audio_latency_mode_t> getSupportedLatencyModes() const {
        audio_utils::lock_guard lock(mMutex);
        return mSupportedLatencyModes;
    }
    std::vector<audio_latency_mode_t> getOrderedLowLatencyModes() const {
        return mOrderedLowLatencyModes;
    }
    audio_latency_mode_t getRequestedLatencyMode() const {
        audio_utils::lock_guard lock(mMutex);
        return mRequestedLatencyMode;
    }
    status_t attachOutput(audio_io_handle_t output, size_t numActiveTracks);
    audio_io_handle_t detachOutput();
    audio_io_handle_t getOutput() const { audio_utils::lock_guard lock(mMutex); return mOutput; }
    void setOutput(audio_io_handle_t output) {
        audio_utils::lock_guard lock(mMutex);
        mOutput = output;
    }
    void updateActiveTracks(size_t numActiveTracks);
    audio_config_base_t getAudioInConfig() const;
    void calculateHeadPose();
    std::string toString(unsigned level) const NO_THREAD_SAFETY_ANALYSIS;
    static std::string toString(audio_latency_mode_t mode) {
        const auto result = legacy2aidl_audio_latency_mode_t_AudioLatencyMode(mode);
        return result.has_value() ?
                media::audio::common::toString(*result) : "unknown_latency_mode";
    }
    static void sendEmptyCreateSpatializerMetricWithStatus(status_t status);
    void onSupportedLatencyModesChangedMsg(
            audio_io_handle_t output, std::vector<audio_latency_mode_t>&& modes);
private:
    Spatializer(effect_descriptor_t engineDescriptor,
                     SpatializerPolicyCallback *callback);
    static void engineCallback(int32_t event, void* user, void *info);
    void onHeadToStagePose(const media::Pose3f& headToStage) override;
    void onActualModeChange(media::HeadTrackingMode mode) override;
    void onHeadToStagePoseMsg(const std::vector<float>& headToStage);
    void onActualModeChangeMsg(media::HeadTrackingMode mode);
    static constexpr int kMaxEffectParamValues = 10;
    template<bool MULTI_VALUES, typename T>
    status_t getHalParameter(sp<EffectHalInterface> effect, uint32_t type,
                                          std::vector<T> *values) {
        static_assert(sizeof(T) <= sizeof(uint32_t), "The size of T must less than 32 bits");
        uint32_t cmd[sizeof(effect_param_t) / sizeof(uint32_t) + 1];
        uint32_t reply[sizeof(effect_param_t) / sizeof(uint32_t) + 2 + kMaxEffectParamValues];
        effect_param_t *p = (effect_param_t *)cmd;
        p->psize = sizeof(uint32_t);
        if (MULTI_VALUES) {
            p->vsize = (kMaxEffectParamValues + 1) * sizeof(T);
        } else {
            p->vsize = sizeof(T);
        }
        *(uint32_t *)p->data = type;
        uint32_t replySize = sizeof(effect_param_t) + p->psize + p->vsize;
        status_t status = effect->command(EFFECT_CMD_GET_PARAM,
                                          sizeof(effect_param_t) + sizeof(uint32_t), cmd,
                                          &replySize, reply);
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
        T *params = (T *)((uint8_t *)reply + sizeof(effect_param_t) + sizeof(uint32_t));
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
    template<typename T>
    status_t setEffectParameter_l(uint32_t type, const std::vector<T>& values) REQUIRES(mMutex) {
        static_assert(sizeof(T) <= sizeof(uint32_t), "The size of T must less than 32 bits");
        uint32_t cmd[sizeof(effect_param_t) / sizeof(uint32_t) + 1 + values.size()];
        effect_param_t *p = (effect_param_t *)cmd;
        p->psize = sizeof(uint32_t);
        p->vsize = sizeof(T) * values.size();
        *(uint32_t *)p->data = type;
        memcpy((uint32_t *)p->data + 1, values.data(), sizeof(T) * values.size());
        status_t status = mEngine->setParameter(p);
        if (status != NO_ERROR) {
            return status;
        }
        if (p->status != NO_ERROR) {
            return p->status;
        }
        return NO_ERROR;
    }
    template<typename P1, typename P2>
    status_t setEffectParameter_l(uint32_t type, const P1 val1, const P2 val2) REQUIRES(mMutex) {
        static_assert(sizeof(P1) <= sizeof(uint32_t), "The size of P1 must less than 32 bits");
        static_assert(sizeof(P2) <= sizeof(uint32_t), "The size of P2 must less than 32 bits");
        uint32_t cmd[sizeof(effect_param_t) / sizeof(uint32_t) + 3];
        effect_param_t *p = (effect_param_t *)cmd;
        p->psize = sizeof(uint32_t);
        p->vsize = 2 * sizeof(uint32_t);
        *(uint32_t *)p->data = type;
        *((uint32_t *)p->data + 1) = static_cast<uint32_t>(val1);
        *((uint32_t *)p->data + 2) = static_cast<uint32_t>(val2);
        status_t status = mEngine->setParameter(p);
        if (status != NO_ERROR) {
            return status;
        }
        if (p->status != NO_ERROR) {
            return p->status;
        }
        return NO_ERROR;
    }
    template<typename T>
    status_t getEffectParameter_l(uint32_t type, std::vector<T> *values) REQUIRES(mMutex) {
        static_assert(sizeof(T) <= sizeof(uint32_t), "The size of T must less than 32 bits");
        uint32_t cmd[sizeof(effect_param_t) / sizeof(uint32_t) + 1 + values->size()];
        effect_param_t *p = (effect_param_t *)cmd;
        p->psize = sizeof(uint32_t);
        p->vsize = sizeof(T) * values->size();
        *(uint32_t *)p->data = type;
        status_t status = mEngine->getParameter(p);
        if (status != NO_ERROR) {
            return status;
        }
        if (p->status != NO_ERROR) {
            return p->status;
        }
        int numValues = std::min(p->vsize / sizeof(T), values->size());
        (*values).clear();
        T *retValues = (T *)((uint8_t *)p->data + sizeof(uint32_t));
        std::copy(&retValues[0], &retValues[numValues], back_inserter(*values));
        return NO_ERROR;
    }
    template<typename P1, typename P2>
    status_t getEffectParameter_l(uint32_t type, P1 *val1, P2 *val2) REQUIRES(mMutex) {
        static_assert(sizeof(P1) <= sizeof(uint32_t), "The size of P1 must less than 32 bits");
        static_assert(sizeof(P2) <= sizeof(uint32_t), "The size of P2 must less than 32 bits");
        uint32_t cmd[sizeof(effect_param_t) / sizeof(uint32_t) + 3];
        effect_param_t *p = (effect_param_t *)cmd;
        p->psize = sizeof(uint32_t);
        p->vsize = 2 * sizeof(uint32_t);
        *(uint32_t *)p->data = type;
        status_t status = mEngine->getParameter(p);
        if (status != NO_ERROR) {
            return status;
        }
        if (p->status != NO_ERROR) {
            return p->status;
        }
        *val1 = static_cast<P1>(*((uint32_t *)p->data + 1));
        *val2 = static_cast<P2>(*((uint32_t *)p->data + 2));
        return NO_ERROR;
    }
    virtual void onFramesProcessed(int32_t framesProcessed) override;
    void checkSensorsState_l() REQUIRES(mMutex);
    void checkPoseController_l() REQUIRES(mMutex);
    void checkEngineState_l() REQUIRES(mMutex);
    void resetEngineHeadPose_l() REQUIRES(mMutex);
    void loadOrderedLowLatencyModes();
    void sortSupportedLatencyModes_l() REQUIRES(mMutex);
    void setEngineHeadtrackingConnectionMode_l() REQUIRES(mMutex);
    audio_latency_mode_t selectHeadtrackingConnectionMode_l() REQUIRES(mMutex);
    const effect_descriptor_t mEngineDescriptor;
    SpatializerPolicyCallback* const mPolicyCallback;
    static constexpr const char* kDefaultMetricsId =
            AMEDIAMETRICS_KEY_PREFIX_AUDIO_SPATIALIZER "0";
    const std::string mMetricsId = kDefaultMetricsId;
    mutable audio_utils::mutex mMutex{audio_utils::MutexOrder::kSpatializer_Mutex};
    sp<AudioEffect> mEngine GUARDED_BY(mMutex);
    audio_io_handle_t mOutput GUARDED_BY(mMutex) = AUDIO_IO_HANDLE_NONE;
    sp<media::INativeSpatializerCallback> mSpatializerCallback GUARDED_BY(mMutex);
    sp<media::ISpatializerHeadTrackingCallback> mHeadTrackingCallback GUARDED_BY(mMutex);
    media::audio::common::Spatialization::Level mLevel GUARDED_BY(mMutex) =
            media::audio::common::Spatialization::Level::NONE;
    std::shared_ptr<SpatializerPoseController> mPoseController GUARDED_BY(mMutex);
    media::HeadTrackingMode mDesiredHeadTrackingMode GUARDED_BY(mMutex)
            = media::HeadTrackingMode::STATIC;
    media::audio::common::HeadTracking::Mode mActualHeadTrackingMode GUARDED_BY(mMutex)
            = media::audio::common::HeadTracking::Mode::DISABLED;
    int32_t mHeadSensor GUARDED_BY(mMutex) = SpatializerPoseController::INVALID_SENSOR;
    int32_t mScreenSensor GUARDED_BY(mMutex) = SpatializerPoseController::INVALID_SENSOR;
    float mDisplayOrientation GUARDED_BY(mMutex) = 0.f;
    bool mFoldedState GUARDED_BY(mMutex) = false;
    float mHingeAngle GUARDED_BY(mMutex) = 0.f;
    std::vector<media::audio::common::Spatialization::Level> mLevels;
    std::vector<media::audio::common::HeadTracking::Mode> mHeadTrackingModes;
    std::vector<media::audio::common::Spatialization::Mode> mSpatializationModes;
    std::vector<audio_channel_mask_t> mChannelMasks;
    bool mSupportsHeadTracking;
    std::unordered_set<media::audio::common::HeadTracking::ConnectionMode>
            mSupportedHeadtrackingConnectionModes;
    media::audio::common::HeadTracking::ConnectionMode mHeadtrackingConnectionMode =
            media::audio::common::HeadTracking::ConnectionMode::FRAMEWORK_PROCESSED;
    class EngineCallbackHandler;
    sp<ALooper> mLooper;
    sp<EngineCallbackHandler> mHandler;
    size_t mNumActiveTracks GUARDED_BY(mMutex) = 0;
    std::vector<audio_latency_mode_t> mSupportedLatencyModes GUARDED_BY(mMutex);
    std::vector<audio_latency_mode_t> mOrderedLowLatencyModes;
    audio_latency_mode_t mRequestedLatencyMode GUARDED_BY(mMutex) = AUDIO_LATENCY_MODE_FREE;
    static const std::map<std::string, audio_latency_mode_t> sStringToLatencyModeMap;
    static const std::vector<const char*> sHeadPoseKeys;
    static constexpr int mMaxLocalLogLine = 10;
    SimpleLog mLocalLog{mMaxLocalLogLine};
    media::VectorRecorder mPoseRecorder GUARDED_BY(mMutex) {
        6 , std::chrono::seconds(1), mMaxLocalLogLine, { 3 } };
    media::VectorRecorder mPoseDurableRecorder GUARDED_BY(mMutex) {
        6 , std::chrono::minutes(1), mMaxLocalLogLine, { 3 } };
};
};
#endif
