       
#include <mutex>
#include "Scheduler.h"
namespace android::scheduler {
class VSyncModulator {
private:
    static constexpr int MIN_EARLY_FRAME_COUNT_TRANSACTION = 2;
    static constexpr int MIN_EARLY_GL_FRAME_COUNT_TRANSACTION = 2;
    using RefreshRateType = RefreshRateConfigs::RefreshRateType;
public:
    struct Offsets {
        RefreshRateType fpsMode;
        nsecs_t sf;
        nsecs_t app;
    };
    struct OffsetsConfig {
        Offsets early;
        Offsets earlyGl;
        Offsets late;
        nsecs_t thresholdForNextVsync;
    };
    VSyncModulator(Scheduler&, ConnectionHandle appConnectionHandle,
                   ConnectionHandle sfConnectionHandle, const OffsetsConfig&);
    void setPhaseOffsets(const OffsetsConfig&) EXCLUDES(mMutex);
    void setTransactionStart(Scheduler::TransactionStart transactionStart);
    void onTransactionHandled();
    void onRefreshRateChangeInitiated();
    void onRefreshRateChangeCompleted();
    void onRefreshed(bool usedRenderEngine);
    Offsets getOffsets() const EXCLUDES(mMutex);
private:
    const Offsets& getNextOffsets() const REQUIRES(mMutex);
    void updateOffsets() EXCLUDES(mMutex);
    void updateOffsetsLocked() REQUIRES(mMutex);
    Scheduler& mScheduler;
    const ConnectionHandle mAppConnectionHandle;
    const ConnectionHandle mSfConnectionHandle;
    mutable std::mutex mMutex;
    OffsetsConfig mOffsetsConfig GUARDED_BY(mMutex);
    Offsets mOffsets GUARDED_BY(mMutex){mOffsetsConfig.late};
    std::atomic<Scheduler::TransactionStart> mTransactionStart =
            Scheduler::TransactionStart::NORMAL;
    std::atomic<bool> mRefreshRateChangePending = false;
    std::atomic<int> mRemainingEarlyFrameCount = 0;
    std::atomic<int> mRemainingRenderEngineUsageCount = 0;
    bool mTraceDetailedInfo = false;
};
}
