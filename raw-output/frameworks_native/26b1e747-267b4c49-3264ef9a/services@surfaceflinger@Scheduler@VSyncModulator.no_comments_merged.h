       
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
<<<<<<< HEAD
    void setPhaseOffsets(const OffsetsConfig&) EXCLUDES(mMutex);
||||||| 3264ef9aa2
    void setEventThreads(EventThread* sfEventThread, EventThread* appEventThread) {
        mSfEventThread = sfEventThread;
        mAppEventThread = appEventThread;
    }
    void setSchedulerAndHandles(Scheduler* scheduler,
                                Scheduler::ConnectionHandle* appConnectionHandle,
                                Scheduler::ConnectionHandle* sfConnectionHandle) {
        mScheduler = scheduler;
        mAppConnectionHandle = appConnectionHandle;
        mSfConnectionHandle = sfConnectionHandle;
    }
=======
    void setSchedulerAndHandles(Scheduler* scheduler,
                                Scheduler::ConnectionHandle* appConnectionHandle,
                                Scheduler::ConnectionHandle* sfConnectionHandle) {
        mScheduler = scheduler;
        mAppConnectionHandle = appConnectionHandle;
        mSfConnectionHandle = sfConnectionHandle;
    }
>>>>>>> 267b4c49
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
<<<<<<< HEAD
    Offsets mOffsets GUARDED_BY(mMutex){mOffsetsConfig.late};
||||||| 3264ef9aa2
    EventThread* mSfEventThread = nullptr;
    EventThread* mAppEventThread = nullptr;
    Scheduler* mScheduler = nullptr;
    Scheduler::ConnectionHandle* mAppConnectionHandle = nullptr;
    Scheduler::ConnectionHandle* mSfConnectionHandle = nullptr;
    Offsets mOffsets GUARDED_BY(mMutex) = {Scheduler::RefreshRateType::DEFAULT, 0, 0};
=======
    Scheduler* mScheduler = nullptr;
    Scheduler::ConnectionHandle* mAppConnectionHandle = nullptr;
    Scheduler::ConnectionHandle* mSfConnectionHandle = nullptr;
    Offsets mOffsets GUARDED_BY(mMutex) = {Scheduler::RefreshRateType::DEFAULT, 0, 0};
>>>>>>> 267b4c49
    std::atomic<Scheduler::TransactionStart> mTransactionStart =
            Scheduler::TransactionStart::NORMAL;
    std::atomic<bool> mRefreshRateChangePending = false;
    std::atomic<int> mRemainingEarlyFrameCount = 0;
    std::atomic<int> mRemainingRenderEngineUsageCount = 0;
    bool mTraceDetailedInfo = false;
};
}
