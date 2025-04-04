#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include "VSyncModulator.h"
#include <cutils/properties.h>
#include <utils/Trace.h>
#include <cinttypes>
#include <mutex>
namespace android::scheduler {
VSyncModulator::VSyncModulator(Scheduler& scheduler,
                               Scheduler::ConnectionHandle appConnectionHandle,
                               Scheduler::ConnectionHandle sfConnectionHandle,
                               const OffsetsConfig& config)
      : mScheduler(scheduler),
        mAppConnectionHandle(appConnectionHandle),
        mSfConnectionHandle(sfConnectionHandle),
        mOffsetsConfig(config) {
    char value[PROPERTY_VALUE_MAX];
    property_get("debug.sf.vsync_trace_detailed_info", value, "0");
    mTraceDetailedInfo = atoi(value);
}
void VSyncModulator::setPhaseOffsets(const OffsetsConfig& config) {
    std::lock_guard<std::mutex> lock(mMutex);
    mOffsetsConfig = config;
    updateOffsetsLocked();
}
void VSyncModulator::setTransactionStart(Scheduler::TransactionStart transactionStart) {
    if (transactionStart == Scheduler::TransactionStart::EARLY) {
        mRemainingEarlyFrameCount = MIN_EARLY_FRAME_COUNT_TRANSACTION;
    }
    if (transactionStart == mTransactionStart ||
        mTransactionStart == Scheduler::TransactionStart::EARLY) {
        return;
    }
    mTransactionStart = transactionStart;
    updateOffsets();
}
void VSyncModulator::onTransactionHandled() {
    if (mTransactionStart == Scheduler::TransactionStart::NORMAL) return;
    mTransactionStart = Scheduler::TransactionStart::NORMAL;
    updateOffsets();
}
void VSyncModulator::onRefreshRateChangeInitiated() {
    if (mRefreshRateChangePending) {
        return;
    }
    mRefreshRateChangePending = true;
    updateOffsets();
}
void VSyncModulator::onRefreshRateChangeCompleted() {
    if (!mRefreshRateChangePending) {
        return;
    }
    mRefreshRateChangePending = false;
    updateOffsets();
}
void VSyncModulator::onRefreshed(bool usedRenderEngine) {
    bool updateOffsetsNeeded = false;
    if (mRemainingEarlyFrameCount > 0) {
        mRemainingEarlyFrameCount--;
        updateOffsetsNeeded = true;
    }
    if (usedRenderEngine) {
        mRemainingRenderEngineUsageCount = MIN_EARLY_GL_FRAME_COUNT_TRANSACTION;
        updateOffsetsNeeded = true;
    } else if (mRemainingRenderEngineUsageCount > 0) {
        mRemainingRenderEngineUsageCount--;
        updateOffsetsNeeded = true;
    }
    if (updateOffsetsNeeded) {
        updateOffsets();
    }
}
VSyncModulator::Offsets VSyncModulator::getOffsets() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mOffsets;
}
const VSyncModulator::Offsets& VSyncModulator::getNextOffsets() const {
    if (mTransactionStart == Scheduler::TransactionStart::EARLY || mRemainingEarlyFrameCount > 0 ||
        mRefreshRateChangePending) {
        return mOffsetsConfig.early;
    } else if (mRemainingRenderEngineUsageCount > 0) {
        return mOffsetsConfig.earlyGl;
    } else {
        return mOffsetsConfig.late;
    }
}
void VSyncModulator::updateOffsets() {
    std::lock_guard<std::mutex> lock(mMutex);
    updateOffsetsLocked();
}
void VSyncModulator::updateOffsetsLocked() {
<<<<<<< HEAD
    const Offsets& offsets = getNextOffsets();
||||||| 3264ef9aa2
    const Offsets desired = getNextOffsets();
    const Offsets current = mOffsets;
=======
    const Offsets desired = getNextOffsets();
>>>>>>> 267b4c49
<<<<<<< HEAD
    mScheduler.setPhaseOffset(mSfConnectionHandle, offsets.sf);
    mScheduler.setPhaseOffset(mAppConnectionHandle, offsets.app);
||||||| 3264ef9aa2
    bool changed = false;
    if (desired.sf != current.sf) {
        if (mSfConnectionHandle != nullptr) {
            mScheduler->setPhaseOffset(mSfConnectionHandle, desired.sf);
        } else if (mSfEventThread != nullptr) {
            mSfEventThread->setPhaseOffset(desired.sf);
        }
        changed = true;
    }
    if (desired.app != current.app) {
        if (mAppConnectionHandle != nullptr) {
            mScheduler->setPhaseOffset(mAppConnectionHandle, desired.app);
        } else if (mAppEventThread != nullptr) {
            mAppEventThread->setPhaseOffset(desired.app);
        }
        changed = true;
    }
=======
    if (mSfConnectionHandle != nullptr) {
        mScheduler->setPhaseOffset(mSfConnectionHandle, desired.sf);
    }
>>>>>>> 267b4c49
<<<<<<< HEAD
    mOffsets = offsets;
||||||| 3264ef9aa2
    if (changed) {
        flushOffsets();
    }
}
=======
    if (mAppConnectionHandle != nullptr) {
        mScheduler->setPhaseOffset(mAppConnectionHandle, desired.app);
    }
    flushOffsets();
}
>>>>>>> 267b4c49
    if (!mTraceDetailedInfo) {
        return;
    }
    const bool isDefault = mOffsets.fpsMode == RefreshRateType::DEFAULT;
    const bool isPerformance = mOffsets.fpsMode == RefreshRateType::PERFORMANCE;
    const bool isEarly = &offsets == &mOffsetsConfig.early;
    const bool isEarlyGl = &offsets == &mOffsetsConfig.earlyGl;
    const bool isLate = &offsets == &mOffsetsConfig.late;
    ATRACE_INT("Vsync-EarlyOffsetsOn", isDefault && isEarly);
    ATRACE_INT("Vsync-EarlyGLOffsetsOn", isDefault && isEarlyGl);
    ATRACE_INT("Vsync-LateOffsetsOn", isDefault && isLate);
    ATRACE_INT("Vsync-HighFpsEarlyOffsetsOn", isPerformance && isEarly);
    ATRACE_INT("Vsync-HighFpsEarlyGLOffsetsOn", isPerformance && isEarlyGl);
    ATRACE_INT("Vsync-HighFpsLateOffsetsOn", isPerformance && isLate);
}
}
