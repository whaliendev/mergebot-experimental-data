#include "VisualizerContext.h"
#include <algorithm>
#include <math.h>
#include <time.h>
#include <android/binder_status.h>
#include <audio_utils/primitives.h>
#include <system/audio.h>
#include <Utils.h>
#ifndef BUILD_FLOAT
        #error AIDL Visualizer only support float 32bits, make sure add cflags -DBUILD_FLOAT,
#endif
using aidl::android::hardware::audio::common::getChannelCount;
namespace aidl::android::hardware::audio::effect {
VisualizerContext::VisualizerContext(int statusDepth, const Parameter::Common& common)
    : EffectContext(statusDepth, common) {
}
VisualizerContext::~VisualizerContext() {
    mState = State::UNINITIALIZED;
}
RetCode VisualizerContext::initParams(const Parameter::Common& common) {
    if (common.input != common.output) {
        LOG(ERROR) << __func__ << " mismatch input: " << common.input.toString()
                   << " and output: " << common.output.toString();
        return RetCode::ERROR_ILLEGAL_PARAMETER;
    }
    mState = State::INITIALIZED;
    auto channelCount = getChannelCount(common.input.base.channelMask);
#ifdef SUPPORT_MC
    if (channelCount < 1 || channelCount > FCC_LIMIT) return RetCode::ERROR_ILLEGAL_PARAMETER;
#else
    if (channelCount != FCC_2) return RetCode::ERROR_ILLEGAL_PARAMETER;
#endif
    mChannelCount = channelCount;
    mCommon = common;
    std::fill(mCaptureBuf.begin(), mCaptureBuf.end(), 0x80);
    return RetCode::SUCCESS;
}
RetCode VisualizerContext::enable() {
    if (mState != State::INITIALIZED) {
        return RetCode::ERROR_EFFECT_LIB_ERROR;
    }
    mState = State::ACTIVE;
    return RetCode::SUCCESS;
}
RetCode VisualizerContext::disable() {
    if (mState != State::ACTIVE) {
        return RetCode::ERROR_EFFECT_LIB_ERROR;
    }
    mState = State::INITIALIZED;
    return RetCode::SUCCESS;
}
void VisualizerContext::reset() {
    std::fill(mCaptureBuf.begin(), mCaptureBuf.end(), 0x80);
}
RetCode VisualizerContext::setCaptureSamples(int samples) {
    mCaptureSamples = samples;
    return RetCode::SUCCESS;
}
<<<<<<< HEAD
int32_t VisualizerContext::getCaptureSamples() {
||||||| a77d2bd563
int VisualizerContext::getCaptureSamples() {
    std::lock_guard lg(mMutex);
=======
int32_t VisualizerContext::getCaptureSamples() {
    std::lock_guard lg(mMutex);
>>>>>>> 38b856f7
    return mCaptureSamples;
}
RetCode VisualizerContext::setMeasurementMode(Visualizer::MeasurementMode mode) {
    mMeasurementMode = mode;
    return RetCode::SUCCESS;
}
Visualizer::MeasurementMode VisualizerContext::getMeasurementMode() {
    return mMeasurementMode;
}
RetCode VisualizerContext::setScalingMode(Visualizer::ScalingMode mode) {
    mScalingMode = mode;
    return RetCode::SUCCESS;
}
Visualizer::ScalingMode VisualizerContext::getScalingMode() {
    return mScalingMode;
}
RetCode VisualizerContext::setDownstreamLatency(int latency) {
    mDownstreamLatency = latency;
    return RetCode::SUCCESS;
}
int VisualizerContext::getDownstreamLatency() {
    return mDownstreamLatency;
}
uint32_t VisualizerContext::getDeltaTimeMsFromUpdatedTime_l() {
    uint32_t deltaMs = 0;
    if (mBufferUpdateTime.tv_sec != 0) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            time_t secs = ts.tv_sec - mBufferUpdateTime.tv_sec;
            long nsec = ts.tv_nsec - mBufferUpdateTime.tv_nsec;
            if (nsec < 0) {
                --secs;
                nsec += 1000000000;
            }
            deltaMs = secs * 1000 + nsec / 1000000;
        }
    }
    return deltaMs;
}
Visualizer::Measurement VisualizerContext::getMeasure() {
    uint16_t peakU16 = 0;
    float sumRmsSquared = 0.0f;
    uint8_t nbValidMeasurements = 0;
    {
        const uint32_t delayMs = getDeltaTimeMsFromUpdatedTime_l();
        if (delayMs > kDiscardMeasurementsTimeMs) {
            LOG(INFO) << __func__ << " Discarding " << delayMs << " ms old measurements";
            for (uint32_t i = 0; i < mMeasurementWindowSizeInBuffers; i++) {
                mPastMeasurements[i].mIsValid = false;
                mPastMeasurements[i].mPeakU16 = 0;
                mPastMeasurements[i].mRmsSquared = 0;
            }
            mMeasurementBufferIdx = 0;
        } else {
            for (uint32_t i = 0; i < mMeasurementWindowSizeInBuffers; i++) {
                if (mPastMeasurements[i].mIsValid) {
                    if (mPastMeasurements[i].mPeakU16 > peakU16) {
                        peakU16 = mPastMeasurements[i].mPeakU16;
                    }
                    sumRmsSquared += mPastMeasurements[i].mRmsSquared;
                    nbValidMeasurements++;
                }
            }
        }
    }
    float rms = nbValidMeasurements == 0 ? 0.0f : sqrtf(sumRmsSquared / nbValidMeasurements);
    Visualizer::Measurement measure;
    measure.rms = (rms < 0.000016f) ? -9600 : (int32_t)(2000 * log10(rms / 32767.0f));
    measure.peak = (peakU16 == 0) ? -9600 : (int32_t)(2000 * log10(peakU16 / 32767.0f));
    LOG(VERBOSE) << __func__ << " peak " << peakU16 << " (" << measure.peak << "mB), rms " << rms
                 << " (" << measure.rms << "mB)";
    return measure;
}
std::vector<uint8_t> VisualizerContext::capture() {
    uint32_t captureSamples = mCaptureSamples;
    std::vector<uint8_t> result(captureSamples, 0x80);
    if (mState != State::ACTIVE) {
        return result;
    }
    const uint32_t deltaMs = getDeltaTimeMsFromUpdatedTime_l();
    if ((mLastCaptureIdx == mCaptureIdx) && (mBufferUpdateTime.tv_sec != 0) &&
        (deltaMs > kMaxStallTimeMs)) {
        mBufferUpdateTime.tv_sec = 0;
        return result;
    }
    int32_t latencyMs = mDownstreamLatency;
    latencyMs -= deltaMs;
    if (latencyMs < 0) {
        latencyMs = 0;
    }
    uint32_t deltaSamples = captureSamples + mCommon.input.base.sampleRate * latencyMs / 1000;
    if (deltaSamples > kMaxCaptureBufSize) {
        android_errorWriteLog(0x534e4554, "31781965");
        deltaSamples = kMaxCaptureBufSize;
    }
    int32_t capturePoint;
    __builtin_sub_overflow((int32_t) mCaptureIdx, deltaSamples, &capturePoint);
    if (capturePoint < 0) {
        uint32_t size = -capturePoint;
        if (size > captureSamples) {
            size = captureSamples;
        }
        std::copy(std::begin(mCaptureBuf) + kMaxCaptureBufSize - size,
                  std::begin(mCaptureBuf) + kMaxCaptureBufSize, result.begin());
        captureSamples -= size;
        capturePoint = 0;
    }
    std::copy(std::begin(mCaptureBuf) + capturePoint,
              std::begin(mCaptureBuf) + capturePoint + captureSamples,
              result.begin() + mCaptureSamples - captureSamples);
    mLastCaptureIdx = mCaptureIdx;
    return result;
}
IEffect::Status VisualizerContext::process(float* in, float* out, int samples) {
    IEffect::Status result = {STATUS_NOT_ENOUGH_DATA, 0, 0};
    RETURN_VALUE_IF(in == nullptr || out == nullptr || samples == 0, result, "dataBufferError");
    result.status = STATUS_INVALID_OPERATION;
    RETURN_VALUE_IF(mState != State::ACTIVE, result, "stateNotActive");
    if (mMeasurementMode == Visualizer::MeasurementMode::PEAK_RMS) {
        float rmsSqAcc = 0;
        float maxSample = 0.f;
        for (size_t inIdx = 0; inIdx < (unsigned) samples; ++inIdx) {
            maxSample = fmax(maxSample, fabs(in[inIdx]));
            rmsSqAcc += in[inIdx] * in[inIdx];
        }
        maxSample *= 1 << 15;
        rmsSqAcc *= 1 << 30;
        mPastMeasurements[mMeasurementBufferIdx] = {.mIsValid = true,
                                                    .mPeakU16 = (uint16_t)maxSample,
                                                    .mRmsSquared = rmsSqAcc / samples};
        if (++mMeasurementBufferIdx >= mMeasurementWindowSizeInBuffers) {
            mMeasurementBufferIdx = 0;
        }
    }
    float fscale;
    if (mScalingMode == Visualizer::ScalingMode::NORMALIZED) {
        float maxSample = 0.f;
        for (size_t inIdx = 0; inIdx < (unsigned)samples; ) {
            float smp = 0.f;
            for (int i = 0; i < mChannelCount; ++i) {
                smp += in[inIdx++];
            }
            maxSample = fmax(maxSample, fabs(smp));
        }
        if (maxSample > 0.f) {
            fscale = 0.99f / maxSample;
            int exp;
            const float significand = frexp(fscale, &exp);
            if (significand == 0.5f) {
                fscale *= 255.f / 256.f;
            }
        } else {
            fscale = 1.f;
        }
    } else {
        assert(mScalingMode == Visualizer::ScalingMode::AS_PLAYED);
        fscale = 1.f / mChannelCount;
    }
    uint32_t captIdx;
    uint32_t inIdx;
    for (inIdx = 0, captIdx = mCaptureIdx; inIdx < (unsigned)samples; captIdx++) {
        if (captIdx >= kMaxCaptureBufSize) {
            captIdx = 0;
        }
        float smp = 0.f;
        for (uint32_t i = 0; i < mChannelCount; ++i) {
            smp += in[inIdx++];
        }
        mCaptureBuf[captIdx] = clamp8_from_float(smp * fscale);
    }
    mCaptureIdx = captIdx;
    if (clock_gettime(CLOCK_MONOTONIC, &mBufferUpdateTime) < 0) {
        mBufferUpdateTime.tv_sec = 0;
    }
    memcpy(out, in, samples * sizeof(float));
    return {STATUS_OK, samples, samples};
}
}
