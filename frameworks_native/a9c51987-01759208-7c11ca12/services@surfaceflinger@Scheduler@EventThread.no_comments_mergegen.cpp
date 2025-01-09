#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>
#include <chrono>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <android-base/stringprintf.h>
#include <binder/IPCThreadState.h>
#include <cutils/compiler.h>
#include <cutils/sched_policy.h>
#include <gui/DisplayEventReceiver.h>
#include <gui/SchedulingPolicy.h>
#include <utils/Errors.h>
#include <utils/Trace.h>
#include <common/FlagManager.h>
#include <scheduler/VsyncConfig.h>
#include "DisplayHardware/DisplayMode.h"
#include "FrameTimeline.h"
#include "VSyncDispatch.h"
#include "VSyncTracker.h"
#include "EventThread.h"
#undef LOG_TAG
#define LOG_TAG "EventThread"
using namespace std::chrono_literals;
namespace android {
using base::StringAppendF;
using base::StringPrintf;
namespace {
auto vsyncPeriod(VSyncRequest request) {
    return static_cast<std::underlying_type_t<VSyncRequest>>(request);
}
std::string toString(VSyncRequest request) {
    switch (request) {
        case VSyncRequest::None:
            return "VSyncRequest::None";
        case VSyncRequest::Single:
            return "VSyncRequest::Single";
        case VSyncRequest::SingleSuppressCallback:
            return "VSyncRequest::SingleSuppressCallback";
        default:
            return StringPrintf("VSyncRequest::Periodic{period=%d}", vsyncPeriod(request));
    }
}
std::string toString(const EventThreadConnection& connection) {
    return StringPrintf("Connection{%p, %s}", &connection,
                        toString(connection.vsyncRequest).c_str());
}
std::string toString(const DisplayEventReceiver::Event& event) {
    switch (event.header.type) {
        case DisplayEventReceiver::DISPLAY_EVENT_HOTPLUG:
            return StringPrintf("Hotplug{displayId=%s, %s}",
                                to_string(event.header.displayId).c_str(),
                                event.hotplug.connected ? "connected" : "disconnected");
        case DisplayEventReceiver::DISPLAY_EVENT_VSYNC:
            return StringPrintf("VSync{displayId=%s, count=%u, expectedPresentationTime=%" PRId64
                                "}",
                                to_string(event.header.displayId).c_str(), event.vsync.count,
                                event.vsync.vsyncData.preferredExpectedPresentationTime());
        case DisplayEventReceiver::DISPLAY_EVENT_MODE_CHANGE:
            return StringPrintf("ModeChanged{displayId=%s, modeId=%u}",
                                to_string(event.header.displayId).c_str(), event.modeChange.modeId);
        case DisplayEventReceiver::DISPLAY_EVENT_HDCP_LEVELS_CHANGE:
            return StringPrintf("HdcpLevelsChange{displayId=%s, connectedLevel=%d, maxLevel=%d}",
                                to_string(event.header.displayId).c_str(),
                                event.hdcpLevelsChange.connectedLevel,
                                event.hdcpLevelsChange.maxLevel);
        default:
            return "Event{}";
    }
}
DisplayEventReceiver::Event makeHotplug(PhysicalDisplayId displayId, nsecs_t timestamp,
                                        bool connected) {
    DisplayEventReceiver::Event event;
    event.header = {DisplayEventReceiver::DISPLAY_EVENT_HOTPLUG, displayId, timestamp};
    event.hotplug.connected = connected;
    return event;
}
DisplayEventReceiver::Event makeHotplugError(nsecs_t timestamp, int32_t connectionError) {
    DisplayEventReceiver::Event event;
    PhysicalDisplayId unusedDisplayId;
    event.header = {DisplayEventReceiver::DISPLAY_EVENT_HOTPLUG, unusedDisplayId, timestamp};
    event.hotplug.connected = false;
    event.hotplug.connectionError = connectionError;
    return event;
}
DisplayEventReceiver::Event makeVSync(PhysicalDisplayId displayId, nsecs_t timestamp,
                                      uint32_t count, nsecs_t expectedPresentationTime,
                                      nsecs_t deadlineTimestamp) {
    DisplayEventReceiver::Event event;
    event.header = {DisplayEventReceiver::DISPLAY_EVENT_VSYNC, displayId, timestamp};
    event.vsync.count = count;
    event.vsync.vsyncData.preferredFrameTimelineIndex = 0;
    event.vsync.vsyncData.frameTimelines[0] = {.vsyncId = FrameTimelineInfo::INVALID_VSYNC_ID,
                                               .deadlineTimestamp = deadlineTimestamp,
                                               .expectedPresentationTime =
                                                       expectedPresentationTime};
    return event;
}
DisplayEventReceiver::Event makeModeChanged(const scheduler::FrameRateMode& mode) {
    DisplayEventReceiver::Event event;
    event.header = {DisplayEventReceiver::DISPLAY_EVENT_MODE_CHANGE,
                    mode.modePtr->getPhysicalDisplayId(), systemTime()};
    event.modeChange.modeId = mode.modePtr->getId().value();
    event.modeChange.vsyncPeriod = mode.fps.getPeriodNsecs();
    return event;
}
DisplayEventReceiver::Event makeFrameRateOverrideEvent(PhysicalDisplayId displayId,
                                                       FrameRateOverride frameRateOverride) {
    return DisplayEventReceiver::Event{
            .header =
                    DisplayEventReceiver::Event::Header{
                            .type = DisplayEventReceiver::DISPLAY_EVENT_FRAME_RATE_OVERRIDE,
                            .displayId = displayId,
                            .timestamp = systemTime(),
                    },
            .frameRateOverride = frameRateOverride,
    };
}
DisplayEventReceiver::Event makeFrameRateOverrideFlushEvent(PhysicalDisplayId displayId) {
    return DisplayEventReceiver::Event{
            .header = DisplayEventReceiver::Event::Header{
                    .type = DisplayEventReceiver::DISPLAY_EVENT_FRAME_RATE_OVERRIDE_FLUSH,
                    .displayId = displayId,
                    .timestamp = systemTime(),
            }};
}
DisplayEventReceiver::Event makeHdcpLevelsChange(PhysicalDisplayId displayId,
                                                 int32_t connectedLevel, int32_t maxLevel) {
    return DisplayEventReceiver::Event{
            .header =
                    DisplayEventReceiver::Event::Header{
                            .type = DisplayEventReceiver::DISPLAY_EVENT_HDCP_LEVELS_CHANGE,
                            .displayId = displayId,
                            .timestamp = systemTime(),
                    },
            .hdcpLevelsChange.connectedLevel = connectedLevel,
            .hdcpLevelsChange.maxLevel = maxLevel,
    };
}
}
EventThreadConnection::EventThreadConnection(EventThread* eventThread, uid_t callingUid,
                                             EventRegistrationFlags eventRegistration)
      : mOwnerUid(callingUid),
        mEventRegistration(eventRegistration),
        mEventThread(eventThread),
        mChannel(gui::BitTube::DefaultSize) {}
EventThreadConnection::~EventThreadConnection() {
}
void EventThreadConnection::onFirstRef() {
    mEventThread->registerDisplayEventConnection(sp<EventThreadConnection>::fromExisting(this));
}
binder::Status EventThreadConnection::stealReceiveChannel(gui::BitTube* outChannel) {
    std::scoped_lock lock(mLock);
    if (mChannel.initCheck() != NO_ERROR) {
        return binder::Status::fromStatusT(NAME_NOT_FOUND);
    }
    outChannel->setReceiveFd(mChannel.moveReceiveFd());
    outChannel->setSendFd(base::unique_fd(dup(mChannel.getSendFd())));
    return binder::Status::ok();
}
binder::Status EventThreadConnection::setVsyncRate(int rate) {
    mEventThread->setVsyncRate(static_cast<uint32_t>(rate),
                               sp<EventThreadConnection>::fromExisting(this));
    return binder::Status::ok();
}
binder::Status EventThreadConnection::requestNextVsync() {
    ATRACE_CALL();
    mEventThread->requestNextVsync(sp<EventThreadConnection>::fromExisting(this));
    return binder::Status::ok();
}
binder::Status EventThreadConnection::getLatestVsyncEventData(
        ParcelableVsyncEventData* outVsyncEventData) {
    ATRACE_CALL();
    outVsyncEventData->vsync =
            mEventThread->getLatestVsyncEventData(sp<EventThreadConnection>::fromExisting(this));
    return binder::Status::ok();
}
binder::Status EventThreadConnection::getSchedulingPolicy(gui::SchedulingPolicy* outPolicy) {
    return gui::getSchedulingPolicy(outPolicy);
}
status_t EventThreadConnection::postEvent(const DisplayEventReceiver::Event& event) {
    constexpr auto toStatus = [](ssize_t size) {
        return size < 0 ? status_t(size) : status_t(NO_ERROR);
    };
    if (event.header.type == DisplayEventReceiver::DISPLAY_EVENT_FRAME_RATE_OVERRIDE ||
        event.header.type == DisplayEventReceiver::DISPLAY_EVENT_FRAME_RATE_OVERRIDE_FLUSH) {
        mPendingEvents.emplace_back(event);
        if (event.header.type == DisplayEventReceiver::DISPLAY_EVENT_FRAME_RATE_OVERRIDE) {
            return status_t(NO_ERROR);
        }
        auto size = DisplayEventReceiver::sendEvents(&mChannel, mPendingEvents.data(),
                                                     mPendingEvents.size());
        mPendingEvents.clear();
        return toStatus(size);
    }
    auto size = DisplayEventReceiver::sendEvents(&mChannel, &event, 1);
    return toStatus(size);
}
EventThread::~EventThread() = default;
namespace impl {
EventThread::EventThread(const char* name, std::shared_ptr<scheduler::VsyncSchedule> vsyncSchedule,
                         android::frametimeline::TokenManager* tokenManager,
                         IEventThreadCallback& callback, std::chrono::nanoseconds workDuration,
                         std::chrono::nanoseconds readyDuration)
      : mThreadName(name),
        mVsyncTracer(base::StringPrintf("VSYNC-%s", name), 0),
        mWorkDuration(base::StringPrintf("VsyncWorkDuration-%s", name), workDuration),
        mReadyDuration(readyDuration),
        mVsyncSchedule(std::move(vsyncSchedule)),
        mVsyncRegistration(mVsyncSchedule->getDispatch(), createDispatchCallback(), name),
        mTokenManager(tokenManager),
        mCallback(callback) {
    mThread = std::thread([this]() NO_THREAD_SAFETY_ANALYSIS {
        std::unique_lock<std::mutex> lock(mMutex);
        threadMain(lock);
    });
    pthread_setname_np(mThread.native_handle(), mThreadName);
    pid_t tid = pthread_gettid_np(mThread.native_handle());
    constexpr int EVENT_THREAD_PRIORITY = 2;
    struct sched_param param = {0};
    param.sched_priority = EVENT_THREAD_PRIORITY;
    if (pthread_setschedparam(mThread.native_handle(), SCHED_FIFO, &param) != 0) {
        ALOGE("Couldn't set SCHED_FIFO for EventThread");
    }
    set_sched_policy(tid, SP_FOREGROUND);
}
EventThread::~EventThread() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mState = State::Quit;
        mCondition.notify_all();
    }
    mThread.join();
}
void EventThread::setDuration(std::chrono::nanoseconds workDuration,
                              std::chrono::nanoseconds readyDuration) {
    std::lock_guard<std::mutex> lock(mMutex);
    mWorkDuration = workDuration;
    mReadyDuration = readyDuration;
    mVsyncRegistration.update({.workDuration = mWorkDuration.get().count(),
                               .readyDuration = mReadyDuration.count(),
                               .lastVsync = mLastVsyncCallbackTime.ns()});
}
sp<EventThreadConnection> EventThread::createEventConnection(
auto connection = sp<EventThreadConnection>::make(const_cast<EventThread*>(this), IPCThreadState::self()->getCallingUid(), eventRegistration); if (FlagManager::getInstance().misc1()) { const int policy = SCHED_FIFO; connection->setMinSchedulerPolicy(policy, sched_get_priority_min(policy)); } return connection;
}
status_t EventThread::registerDisplayEventConnection(const sp<EventThreadConnection>& connection) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = std::find(mDisplayEventConnections.cbegin(),
            mDisplayEventConnections.cend(), connection);
    if (it != mDisplayEventConnections.cend()) {
        ALOGW("DisplayEventConnection %p already exists", connection.get());
        mCondition.notify_all();
        return ALREADY_EXISTS;
    }
    mDisplayEventConnections.push_back(connection);
    mCondition.notify_all();
    return NO_ERROR;
}
void EventThread::removeDisplayEventConnectionLocked(const wp<EventThreadConnection>& connection) {
    auto it = std::find(mDisplayEventConnections.cbegin(),
            mDisplayEventConnections.cend(), connection);
    if (it != mDisplayEventConnections.cend()) {
        mDisplayEventConnections.erase(it);
    }
}
void EventThread::setVsyncRate(uint32_t rate, const sp<EventThreadConnection>& connection) {
    if (static_cast<std::underlying_type_t<VSyncRequest>>(rate) < 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    const auto request = rate == 0 ? VSyncRequest::None : static_cast<VSyncRequest>(rate);
    if (connection->vsyncRequest != request) {
        connection->vsyncRequest = request;
        mCondition.notify_all();
    }
}
void EventThread::requestNextVsync(const sp<EventThreadConnection>& connection) {
    mCallback.resync();
    std::lock_guard<std::mutex> lock(mMutex);
    if (connection->vsyncRequest == VSyncRequest::None) {
        connection->vsyncRequest = VSyncRequest::Single;
        mCondition.notify_all();
    } else if (connection->vsyncRequest == VSyncRequest::SingleSuppressCallback) {
        connection->vsyncRequest = VSyncRequest::Single;
    }
}
VsyncEventData EventThread::getLatestVsyncEventData(
        const sp<EventThreadConnection>& connection) const {
    mCallback.resync();
    VsyncEventData vsyncEventData;
    const Period frameInterval = mCallback.getVsyncPeriod(connection->mOwnerUid);
    vsyncEventData.frameInterval = frameInterval.ns();
    const auto [presentTime, deadline] = [&]() -> std::pair<nsecs_t, nsecs_t> {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto vsyncTime = mVsyncSchedule->getTracker().nextAnticipatedVSyncTimeFrom(
                systemTime() + mWorkDuration.get().count() + mReadyDuration.count());
        return {vsyncTime, vsyncTime - mReadyDuration.count()};
    }();
    generateFrameTimeline(vsyncEventData, frameInterval.ns(), systemTime(SYSTEM_TIME_MONOTONIC),
                          presentTime, deadline);
    return vsyncEventData;
}
void EventThread::enableSyntheticVsync(bool enable) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mVSyncState || mVSyncState->synthetic == enable) {
        return;
    }
    mVSyncState->synthetic = enable;
    mCondition.notify_all();
}
void EventThread::onVsync(nsecs_t vsyncTime, nsecs_t wakeupTime, nsecs_t readyTime) {
    std::lock_guard<std::mutex> lock(mMutex);
    mLastVsyncCallbackTime = TimePoint::fromNs(vsyncTime);
    LOG_FATAL_IF(!mVSyncState);
    mVsyncTracer = (mVsyncTracer + 1) % 2;
    mPendingEvents.push_back(makeVSync(mVSyncState->displayId, wakeupTime, ++mVSyncState->count,
                                       vsyncTime, readyTime));
    mCondition.notify_all();
}
void EventThread::onHotplugReceived(PhysicalDisplayId displayId, bool connected) {
    std::lock_guard<std::mutex> lock(mMutex);
    mPendingEvents.push_back(makeHotplug(displayId, systemTime(), connected));
    mCondition.notify_all();
}
void EventThread::onHotplugConnectionError(int32_t errorCode) {
    std::lock_guard<std::mutex> lock(mMutex);
    mPendingEvents.push_back(makeHotplugError(systemTime(), errorCode));
    mCondition.notify_all();
}
void EventThread::onModeChanged(const scheduler::FrameRateMode& mode) {
    std::lock_guard<std::mutex> lock(mMutex);
    mPendingEvents.push_back(makeModeChanged(mode));
    mCondition.notify_all();
}
void EventThread::onFrameRateOverridesChanged(PhysicalDisplayId displayId,
                                              std::vector<FrameRateOverride> overrides) {
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto frameRateOverride : overrides) {
        mPendingEvents.push_back(makeFrameRateOverrideEvent(displayId, frameRateOverride));
    }
    mPendingEvents.push_back(makeFrameRateOverrideFlushEvent(displayId));
    mCondition.notify_all();
}
void EventThread::onHdcpLevelsChanged(PhysicalDisplayId displayId, int32_t connectedLevel,
                                      int32_t maxLevel) {
    std::lock_guard<std::mutex> lock(mMutex);
    mPendingEvents.push_back(makeHdcpLevelsChange(displayId, connectedLevel, maxLevel));
    mCondition.notify_all();
}
void EventThread::threadMain(std::unique_lock<std::mutex>& lock) {
    DisplayEventConsumers consumers;
    while (mState != State::Quit) {
        std::optional<DisplayEventReceiver::Event> event;
        if (!mPendingEvents.empty()) {
            event = mPendingEvents.front();
            mPendingEvents.pop_front();
            if (event->header.type == DisplayEventReceiver::DISPLAY_EVENT_HOTPLUG) {
                if (event->hotplug.connectionError == 0) {
                    if (event->hotplug.connected && !mVSyncState) {
                        mVSyncState.emplace(event->header.displayId);
                    } else if (!event->hotplug.connected && mVSyncState &&
                               mVSyncState->displayId == event->header.displayId) {
                        mVSyncState.reset();
                    }
                } else {
                }
            }
        }
        bool vsyncRequested = false;
        auto it = mDisplayEventConnections.begin();
        while (it != mDisplayEventConnections.end()) {
            if (const auto connection = it->promote()) {
                if (event && shouldConsumeEvent(*event, connection)) {
                    consumers.push_back(connection);
                }
                vsyncRequested |= connection->vsyncRequest != VSyncRequest::None;
                ++it;
            } else {
                it = mDisplayEventConnections.erase(it);
            }
        }
        if (!consumers.empty()) {
            dispatchEvent(*event, consumers);
            consumers.clear();
        }
        if (mVSyncState && vsyncRequested) {
            mState = mVSyncState->synthetic ? State::SyntheticVSync : State::VSync;
        } else {
            ALOGW_IF(!mVSyncState, "Ignoring VSYNC request while display is disconnected");
            mState = State::Idle;
        }
        if (mState == State::VSync) {
            const auto scheduleResult =
                    mVsyncRegistration.schedule({.workDuration = mWorkDuration.get().count(),
                                                 .readyDuration = mReadyDuration.count(),
                                                 .lastVsync = mLastVsyncCallbackTime.ns()});
            LOG_ALWAYS_FATAL_IF(!scheduleResult, "Error scheduling callback");
        } else {
            mVsyncRegistration.cancel();
        }
        if (!mPendingEvents.empty()) {
            continue;
        }
        if (mState == State::Idle) {
            mCondition.wait(lock);
        } else {
            const std::chrono::nanoseconds timeout =
                    mState == State::SyntheticVSync ? 16ms : 1000ms;
            if (mCondition.wait_for(lock, timeout) == std::cv_status::timeout) {
                if (mState == State::VSync) {
                    ALOGW("Faking VSYNC due to driver stall for thread %s", mThreadName);
                }
                LOG_FATAL_IF(!mVSyncState);
                const auto now = systemTime(SYSTEM_TIME_MONOTONIC);
                const auto deadlineTimestamp = now + timeout.count();
                const auto expectedVSyncTime = deadlineTimestamp + timeout.count();
                mPendingEvents.push_back(makeVSync(mVSyncState->displayId, now,
                                                   ++mVSyncState->count, expectedVSyncTime,
                                                   deadlineTimestamp));
            }
        }
    }
    mVsyncRegistration.cancel();
}
bool EventThread::shouldConsumeEvent(const DisplayEventReceiver::Event& event,
                                     const sp<EventThreadConnection>& connection) const {
    const auto throttleVsync = [&]() REQUIRES(mMutex) {
        const auto& vsyncData = event.vsync.vsyncData;
        if (connection->frameRate.isValid()) {
            return !mVsyncSchedule->getTracker()
                            .isVSyncInPhase(vsyncData.preferredExpectedPresentationTime(),
                                            connection->frameRate);
        }
        const auto expectedPresentTime =
                TimePoint::fromNs(event.vsync.vsyncData.preferredExpectedPresentationTime());
        return mCallback.throttleVsync(expectedPresentTime, connection->mOwnerUid);
    };
    switch (event.header.type) {
        case DisplayEventReceiver::DISPLAY_EVENT_HOTPLUG:
            return true;
        case DisplayEventReceiver::DISPLAY_EVENT_HDCP_LEVELS_CHANGE:
            return true;
        case DisplayEventReceiver::DISPLAY_EVENT_MODE_CHANGE: {
            return connection->mEventRegistration.test(
                    gui::ISurfaceComposer::EventRegistration::modeChanged);
        }
        case DisplayEventReceiver::DISPLAY_EVENT_VSYNC:
            switch (connection->vsyncRequest) {
                case VSyncRequest::None:
                    return false;
                case VSyncRequest::SingleSuppressCallback:
                    connection->vsyncRequest = VSyncRequest::None;
                    return false;
                case VSyncRequest::Single: {
                    if (throttleVsync()) {
                        return false;
                    }
                    connection->vsyncRequest = VSyncRequest::SingleSuppressCallback;
                    return true;
                }
                case VSyncRequest::Periodic:
                    if (throttleVsync()) {
                        return false;
                    }
                    return true;
                default:
                    return event.vsync.count % vsyncPeriod(connection->vsyncRequest) == 0;
            }
        case DisplayEventReceiver::DISPLAY_EVENT_FRAME_RATE_OVERRIDE:
            [[fallthrough]];
        case DisplayEventReceiver::DISPLAY_EVENT_FRAME_RATE_OVERRIDE_FLUSH:
            return connection->mEventRegistration.test(
                    gui::ISurfaceComposer::EventRegistration::frameRateOverride);
        default:
            return false;
    }
}
int64_t EventThread::generateToken(nsecs_t timestamp, nsecs_t deadlineTimestamp,
                                   nsecs_t expectedPresentationTime) const {
    if (mTokenManager != nullptr) {
        return mTokenManager->generateTokenForPredictions(
                {timestamp, deadlineTimestamp, expectedPresentationTime});
    }
    return FrameTimelineInfo::INVALID_VSYNC_ID;
}
void EventThread::generateFrameTimeline(VsyncEventData& outVsyncEventData, nsecs_t frameInterval,
                                        nsecs_t timestamp,
                                        nsecs_t preferredExpectedPresentationTime,
                                        nsecs_t preferredDeadlineTimestamp) const {
    uint32_t currentIndex = 0;
    for (int64_t multiplier = -VsyncEventData::kFrameTimelinesCapacity + 1;
         currentIndex < VsyncEventData::kFrameTimelinesCapacity; multiplier++) {
        nsecs_t deadlineTimestamp = preferredDeadlineTimestamp + multiplier * frameInterval;
        if (deadlineTimestamp <= timestamp) {
            continue;
        }
        nsecs_t expectedPresentationTime =
                preferredExpectedPresentationTime + multiplier * frameInterval;
        if (expectedPresentationTime >= preferredExpectedPresentationTime +
                    scheduler::VsyncConfig::kEarlyLatchMaxThreshold.count()) {
            if (currentIndex == 0) {
                ALOGW("%s: Expected present time is too far in the future but no timelines are "
                      "valid. preferred EPT=%" PRId64 ", Calculated EPT=%" PRId64
                      ", multiplier=%" PRId64 ", frameInterval=%" PRId64 ", threshold=%" PRId64,
                      __func__, preferredExpectedPresentationTime, expectedPresentationTime,
                      multiplier, frameInterval,
                      static_cast<int64_t>(
                              scheduler::VsyncConfig::kEarlyLatchMaxThreshold.count()));
            }
            break;
        }
        if (multiplier == 0) {
            outVsyncEventData.preferredFrameTimelineIndex = currentIndex;
        }
        outVsyncEventData.frameTimelines[currentIndex] =
                {.vsyncId = generateToken(timestamp, deadlineTimestamp, expectedPresentationTime),
                 .deadlineTimestamp = deadlineTimestamp,
                 .expectedPresentationTime = expectedPresentationTime};
        currentIndex++;
    }
    if (currentIndex == 0) {
        ALOGW("%s: No timelines are valid. preferred EPT=%" PRId64 ", frameInterval=%" PRId64
              ", threshold=%" PRId64,
              __func__, preferredExpectedPresentationTime, frameInterval,
              static_cast<int64_t>(scheduler::VsyncConfig::kEarlyLatchMaxThreshold.count()));
        outVsyncEventData.frameTimelines[currentIndex] =
                {.vsyncId = generateToken(timestamp, preferredDeadlineTimestamp,
                                          preferredExpectedPresentationTime),
                 .deadlineTimestamp = preferredDeadlineTimestamp,
                 .expectedPresentationTime = preferredExpectedPresentationTime};
        currentIndex++;
    }
    outVsyncEventData.frameTimelinesLength = currentIndex;
}
void EventThread::dispatchEvent(const DisplayEventReceiver::Event& event,
                                const DisplayEventConsumers& consumers) {
    for (const auto& consumer : consumers) {
        DisplayEventReceiver::Event copy = event;
        if (event.header.type == DisplayEventReceiver::DISPLAY_EVENT_VSYNC) {
            const Period frameInterval = mCallback.getVsyncPeriod(consumer->mOwnerUid);
            copy.vsync.vsyncData.frameInterval = frameInterval.ns();
            generateFrameTimeline(copy.vsync.vsyncData, frameInterval.ns(), copy.header.timestamp,
                                  event.vsync.vsyncData.preferredExpectedPresentationTime(),
                                  event.vsync.vsyncData.preferredDeadlineTimestamp());
        }
        switch (consumer->postEvent(copy)) {
            case NO_ERROR:
                break;
            case -EAGAIN:
                ALOGW("Failed dispatching %s for %s", toString(event).c_str(),
                      toString(*consumer).c_str());
                break;
            default:
                removeDisplayEventConnectionLocked(consumer);
        }
    }
}
void EventThread::dump(std::string& result) const {
    std::lock_guard<std::mutex> lock(mMutex);
    StringAppendF(&result, "%s: state=%s VSyncState=", mThreadName, toCString(mState));
    if (mVSyncState) {
        StringAppendF(&result, "{displayId=%s, count=%u%s}\n",
                      to_string(mVSyncState->displayId).c_str(), mVSyncState->count,
                      mVSyncState->synthetic ? ", synthetic" : "");
    } else {
        StringAppendF(&result, "none\n");
    }
    const auto relativeLastCallTime =
            ticks<std::milli, float>(mLastVsyncCallbackTime - TimePoint::now());
    StringAppendF(&result, "mWorkDuration=%.2f mReadyDuration=%.2f last vsync time ",
                  mWorkDuration.get().count() / 1e6f, mReadyDuration.count() / 1e6f);
    StringAppendF(&result, "%.2fms relative to now\n", relativeLastCallTime);
    StringAppendF(&result, "  pending events (count=%zu):\n", mPendingEvents.size());
    for (const auto& event : mPendingEvents) {
        StringAppendF(&result, "    %s\n", toString(event).c_str());
    }
    StringAppendF(&result, "  connections (count=%zu):\n", mDisplayEventConnections.size());
    for (const auto& ptr : mDisplayEventConnections) {
        if (const auto connection = ptr.promote()) {
            StringAppendF(&result, "    %s\n", toString(*connection).c_str());
        }
    }
    result += '\n';
}
const char* EventThread::toCString(State state) {
    switch (state) {
        case State::Idle:
            return "Idle";
        case State::Quit:
            return "Quit";
        case State::SyntheticVSync:
            return "SyntheticVSync";
        case State::VSync:
            return "VSync";
    }
}
void EventThread::onNewVsyncSchedule(std::shared_ptr<scheduler::VsyncSchedule> schedule) {
    scheduler::VSyncCallbackRegistration oldRegistration =
            onNewVsyncScheduleInternal(std::move(schedule));
}
scheduler::VSyncCallbackRegistration EventThread::onNewVsyncScheduleInternal(
        std::shared_ptr<scheduler::VsyncSchedule> schedule) {
    std::lock_guard<std::mutex> lock(mMutex);
    const bool reschedule = mVsyncRegistration.cancel() == scheduler::CancelResult::Cancelled;
    mVsyncSchedule = std::move(schedule);
    auto oldRegistration =
            std::exchange(mVsyncRegistration,
                          scheduler::VSyncCallbackRegistration(mVsyncSchedule->getDispatch(),
                                                               createDispatchCallback(),
                                                               mThreadName));
    if (reschedule) {
        mVsyncRegistration.schedule({.workDuration = mWorkDuration.get().count(),
                                     .readyDuration = mReadyDuration.count(),
                                     .lastVsync = mLastVsyncCallbackTime.ns()});
    }
    return oldRegistration;
}
scheduler::VSyncDispatch::Callback EventThread::createDispatchCallback() {
    return [this](nsecs_t vsyncTime, nsecs_t wakeupTime, nsecs_t readyTime) {
        onVsync(vsyncTime, wakeupTime, readyTime);
    };
}
}
}
#pragma clang diagnostic pop
