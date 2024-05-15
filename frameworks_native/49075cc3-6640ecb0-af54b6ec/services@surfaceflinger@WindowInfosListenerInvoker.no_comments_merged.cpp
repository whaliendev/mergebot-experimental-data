#include <ftl/small_vector.h>
#include <gui/ISurfaceComposer.h>
#include "WindowInfosListenerInvoker.h"
namespace android {
using gui::DisplayInfo;
using gui::IWindowInfosListener;
using gui::WindowInfo;
struct WindowInfosReportedListenerInvoker : gui::BnWindowInfosReportedListener,
                                            IBinder::DeathRecipient {
    WindowInfosReportedListenerInvoker(size_t callbackCount,
                                       WindowInfosReportedListenerSet windowInfosReportedListeners)
          : mCallbacksPending(callbackCount),
            mWindowInfosReportedListeners(std::move(windowInfosReportedListeners)) {}
    binder::Status onWindowInfosReported() override {
        if (--mCallbacksPending == 0) {
            for (const auto& listener : mWindowInfosReportedListeners) {
                sp<IBinder> asBinder = IInterface::asBinder(listener);
                if (asBinder->isBinderAlive()) {
                    listener->onWindowInfosReported();
                }
            }
        }
        return binder::Status::ok();
    }
    void binderDied(const wp<IBinder>&) { onWindowInfosReported(); }
private:
    std::atomic<size_t> mCallbacksPending;
    WindowInfosReportedListenerSet mWindowInfosReportedListeners;
};
void WindowInfosListenerInvoker::addWindowInfosListener(sp<IWindowInfosListener> listener) {
    sp<IBinder> asBinder = IInterface::asBinder(listener);
    asBinder->linkToDeath(sp<DeathRecipient>::fromExisting(this));
    std::scoped_lock lock(mListenersMutex);
    mWindowInfosListeners.try_emplace(asBinder, std::move(listener));
}
void WindowInfosListenerInvoker::removeWindowInfosListener(
        const sp<IWindowInfosListener>& listener) {
    sp<IBinder> asBinder = IInterface::asBinder(listener);
    std::scoped_lock lock(mListenersMutex);
    asBinder->unlinkToDeath(sp<DeathRecipient>::fromExisting(this));
    mWindowInfosListeners.erase(asBinder);
}
void WindowInfosListenerInvoker::binderDied(const wp<IBinder>& who) {
    std::scoped_lock lock(mListenersMutex);
    mWindowInfosListeners.erase(who);
}
void WindowInfosListenerInvoker::windowInfosChanged(
        std::vector<WindowInfo> windowInfos, std::vector<DisplayInfo> displayInfos,
        WindowInfosReportedListenerSet reportedListeners, bool forceImmediateCall) {
    reportedListeners.insert(sp<WindowInfosListenerInvoker>::fromExisting(this));
    auto callListeners = [this, windowInfos = std::move(windowInfos),
                          displayInfos = std::move(displayInfos),
                          reportedListeners = std::move(reportedListeners)]() mutable {
        ftl::SmallVector<const sp<IWindowInfosListener>, kStaticCapacity> windowInfosListeners;
        {
            std::scoped_lock lock(mListenersMutex);
            for (const auto& [_, listener] : mWindowInfosListeners) {
                windowInfosListeners.push_back(listener);
            }
        }
        auto reportedInvoker =
                sp<WindowInfosReportedListenerInvoker>::make(windowInfosListeners.size(),
                                                             std::move(reportedListeners));
        for (const auto& listener : windowInfosListeners) {
            sp<IBinder> asBinder = IInterface::asBinder(listener);
            asBinder->linkToDeath(reportedInvoker);
            auto status =
                    listener->onWindowInfosChanged(windowInfos, displayInfos, reportedInvoker);
            if (!status.isOk()) {
                reportedInvoker->onWindowInfosReported();
            }
        }
    };
    {
        std::scoped_lock lock(mMessagesMutex);
        if (mActiveMessageCount > 0 && !forceImmediateCall) {
            mWindowInfosChangedDelayed = std::move(callListeners);
            return;
        }
        mWindowInfosChangedDelayed = nullptr;
        mActiveMessageCount++;
    }
    callListeners();
}
binder::Status WindowInfosListenerInvoker::onWindowInfosReported() {
    std::function<void()> callListeners;
    {
        std::scoped_lock lock{mMessagesMutex};
        mActiveMessageCount--;
        if (!mWindowInfosChangedDelayed || mActiveMessageCount > 0) {
            return binder::Status::ok();
        }
        mActiveMessageCount++;
        callListeners = std::move(mWindowInfosChangedDelayed);
        mWindowInfosChangedDelayed = nullptr;
    }
    callListeners();
    return binder::Status::ok();
}
}
