       
#include <audio_effects/effect_dynamicsprocessing.h>
#include <system/audio_effects/effect_visualizer.h>
#include "effect-impl/EffectContext.h"
namespace aidl::android::hardware::audio::effect {
class VisualizerContext final : public EffectContext {
  public:
    static constexpr int32_t kMinCaptureBufSize = VISUALIZER_CAPTURE_SIZE_MIN;
    static constexpr int32_t kMaxCaptureBufSize = VISUALIZER_CAPTURE_SIZE_MAX;
    static constexpr uint32_t kMaxLatencyMs = 3000;
    VisualizerContext(int statusDepth, const Parameter::Common& common);
    ~VisualizerContext();
    RetCode initParams(const Parameter::Common& common);
    RetCode enable();
    RetCode disable();
    void reset();
    RetCode setCaptureSamples(int32_t captureSize);
    int32_t getCaptureSamples();
    RetCode setMeasurementMode(Visualizer::MeasurementMode mode);
    Visualizer::MeasurementMode getMeasurementMode();
    RetCode setScalingMode(Visualizer::ScalingMode mode);
    Visualizer::ScalingMode getScalingMode();
    RetCode setDownstreamLatency(int latency);
    int getDownstreamLatency();
    IEffect::Status process(float* in, float* out, int samples);
    Visualizer::Measurement getMeasure();
    std::vector<uint8_t> capture();
    struct BufferStats {
        bool mIsValid;
        uint16_t mPeakU16;
        float mRmsSquared;
    };
    enum State {
        UNINITIALIZED,
        INITIALIZED,
        ACTIVE,
    };
  private:
    static const uint32_t kMaxStallTimeMs = 1000;
    static const uint32_t kDiscardMeasurementsTimeMs = 2000;
    static const uint32_t kMeasurementWindowMaxSizeInBuffers = 25;
    Parameter::Common mCommon;
    State mState = State::UNINITIALIZED;
    uint32_t mCaptureIdx = 0;
    uint32_t mLastCaptureIdx = 0;
    Visualizer::ScalingMode mScalingMode = Visualizer::ScalingMode::NORMALIZED;
    struct timespec mBufferUpdateTime;
<<<<<<< HEAD
    std::array<uint8_t, kMaxCaptureBufSize> mCaptureBuf;
    uint32_t mDownstreamLatency = 0;
    int32_t mCaptureSamples = kMaxCaptureBufSize;
||||||| a77d2bd563
    std::array<uint8_t, kMaxCaptureBufSize> mCaptureBuf GUARDED_BY(mMutex);
    uint32_t mDownstreamLatency GUARDED_BY(mMutex) = 0;
    uint32_t mCaptureSamples GUARDED_BY(mMutex) = kMaxCaptureBufSize;
=======
    std::array<uint8_t, kMaxCaptureBufSize> mCaptureBuf GUARDED_BY(mMutex);
    uint32_t mDownstreamLatency GUARDED_BY(mMutex) = 0;
    int32_t mCaptureSamples GUARDED_BY(mMutex) = kMaxCaptureBufSize;
>>>>>>> 38b856f7
    uint8_t mChannelCount = 0;
    Visualizer::MeasurementMode mMeasurementMode =
            Visualizer::MeasurementMode::NONE;
    uint8_t mMeasurementWindowSizeInBuffers = kMeasurementWindowMaxSizeInBuffers;
    uint8_t mMeasurementBufferIdx = 0;
    std::array<BufferStats, kMeasurementWindowMaxSizeInBuffers> mPastMeasurements;
    void init_params();
    uint32_t getDeltaTimeMsFromUpdatedTime_l();
};
}
