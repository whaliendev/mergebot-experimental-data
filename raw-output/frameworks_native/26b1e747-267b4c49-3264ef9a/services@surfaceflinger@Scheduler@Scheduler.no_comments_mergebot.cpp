#undef LOG_TAG
#define LOG_TAG "Scheduler"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include "Scheduler.h"
#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <functional>
#include <memory>
#include <numeric>
#include <android/hardware/configstore/1.0/ISurfaceFlingerConfigs.h>
#include <android/hardware/configstore/1.1/ISurfaceFlingerConfigs.h>
#include <configstore/Utils.h>
#include <cutils/properties.h>
#include <input/InputWindow.h>
#include <system/window.h>
#include <ui/DisplayStatInfo.h>
#include <utils/Timers.h>
#include <utils/Trace.h>
#include "DispSync.h"
#include "DispSyncSource.h"
#include "EventControlThread.h"
#include "EventThread.h"
#include "IdleTimer.h"
#include "InjectVSyncSource.h"
#include "LayerInfo.h"
#include "OneShotTimer.h"
#include "SchedulerUtils.h"
#include "SurfaceFlingerProperties.h"
#define RETURN_IF_INVALID_HANDLE(handle,...) \
    do { \
        if (mConnections.count(handle) == 0) { \
            ALOGE("Invalid connection handle %" PRIuPTR, handle.id); \
            return __VA_ARGS__; \
        } \
    } while (false)
namespace android {
using namespace android::hardware::configstore;
using namespace android::hardware::configstore::V1_0;
using namespace android::sysprop;
Scheduler::Scheduler(impl::EventControlThread::SetVSyncEnabledFunction function,
                     const scheduler::RefreshRateConfigs& refreshRateConfig)
      : mPrimaryDispSync(new impl::DispSync("SchedulerDispSync",
                                            sysprop::running_without_sync_framework(true))),
        mEventControlThread(new impl::EventControlThread(std::move(function))),
        mSupportKernelTimer(sysprop::support_kernel_idle_timer(false)),
        mRefreshRateConfigs(refreshRateConfig) {
    using namespace sysprop;
    char value[PROPERTY_VALUE_MAX];
    property_get("debug.sf.set_idle_timer_ms", value, "0");
    const int setIdleTimerMs = atoi(value);
    if (const auto millis = setIdleTimerMs ? setIdleTimerMs : set_idle_timer_ms(0); millis > 0) {
        const auto callback = mSupportKernelTimer ? &Scheduler::kernelIdleTimerCallback
                                                  : &Scheduler::idleTimerCallback;
        mIdleTimer.emplace(
                std::chrono::milliseconds(millis),
                [this, callback] { std::invoke(callback, this, TimerState::Reset); },
                [this, callback] { std::invoke(callback, this, TimerState::Expired); });
        mIdleTimer->start();
    }
    if (const int64_t millis = set_touch_timer_ms(0); millis > 0) {
        mTouchTimer.emplace(
                std::chrono::milliseconds(millis),
                [this] { touchTimerCallback(TimerState::Reset); },
                [this] { touchTimerCallback(TimerState::Expired); });
        mTouchTimer->start();
    }
    if (const int64_t millis = set_display_power_timer_ms(0); millis > 0) {
        mDisplayPowerTimer.emplace(
                std::chrono::milliseconds(millis),
                [this] { displayPowerTimerCallback(TimerState::Reset); },
                [this] { displayPowerTimerCallback(TimerState::Expired); });
        mDisplayPowerTimer->start();
    }
}
Scheduler::Scheduler(std::unique_ptr<DispSync> primaryDispSync, std::unique_ptr<EventControlThread> eventControlThread, const scheduler::RefreshRateConfigs& configs): mPrimaryDispSync(std::move(primaryDispSync)), mEventControlThread(std::move(eventControlThread)), mSupportKernelTimer(false), mRefreshRateConfigs(configs) {}
Scheduler::~Scheduler() {
    mDisplayPowerTimer.reset();
    mTouchTimer.reset();
    mIdleTimer.reset();
}
DispSync& Scheduler::getPrimaryDispSync() {
    return *mPrimaryDispSync;
}
Scheduler::ConnectionHandle Scheduler::createConnection(
        const char* connectionName, nsecs_t phaseOffsetNs, nsecs_t offsetThresholdForNextVsync,
        ResyncCallback resyncCallback,
        impl::EventThread::InterceptVSyncsCallback interceptCallback) {
    auto eventThread = makeEventThread(connectionName, phaseOffsetNs, offsetThresholdForNextVsync,
                                       std::move(interceptCallback));
    return createConnection(std::move(eventThread), std::move(resyncCallback));
}
Scheduler::ConnectionHandle Scheduler::createConnection(std::unique_ptr<EventThread> eventThread, ResyncCallback&& resyncCallback) {
    const ConnectionHandle handle = ConnectionHandle{mNextConnectionHandleId++};
    ALOGV("Creating a connection handle with ID %" PRIuPTR, handle.id);
    auto connection = createConnectionInternal(eventThread.get(), std::move(resyncCallback),
                                               ISurfaceComposer::eConfigChangedSuppress);
    mConnections.emplace(handle, Connection{connection, std::move(eventThread)});
    return handle;
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}
}
