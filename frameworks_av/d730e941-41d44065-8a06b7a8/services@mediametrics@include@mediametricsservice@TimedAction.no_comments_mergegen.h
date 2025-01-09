       
#include <android-base/thread_annotations.h>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
namespace android::mediametrics {
class TimedAction {
public:
    TimedAction() : mThread{[this](){threadLoop();}} {}
    ~TimedAction() {
        quit();
    }
    template <typename T>
    void postIn(const T& time, std::function<void()> f) {
        postAt(TimerClock::now() + time, f);
    }
    template <typename T>
    void postAt(const T& targetTime, std::function<void()> f) {
        std::lock_guard l(mLock);
        if (mQuit) return;
        if (mMap.empty() || targetTime < mMap.begin()->first) {
            mMap.emplace_hint(mMap.begin(), targetTime, std::move(f));
            mCondition.notify_one();
        } else {
            mMap.emplace(targetTime, std::move(f));
        }
    }
    void clear() {
        std::lock_guard l(mLock);
        mMap.clear();
    }
    void quit() {
        {
            std::lock_guard l(mLock);
            if (mQuit) return;
            mQuit = true;
            mMap.clear();
            mCondition.notify_all();
        }
        mThread.join();
    }
    size_t size() const {
        std::lock_guard l(mLock);
        return mMap.size();
    }
private:
    void threadLoop() NO_THREAD_SAFETY_ANALYSIS {
        std::unique_lock l(mLock);
        while (!mQuit) {
            auto sleepUntilTime = std::chrono::time_point<TimerClock>::max();
            if (!mMap.empty()) {
                sleepUntilTime = mMap.begin()->first;
                const auto now = TimerClock::now();
                if (sleepUntilTime <= now) {
                    auto node = mMap.extract(mMap.begin());
                    l.unlock();
                    node.mapped()();
                    l.lock();
                    continue;
                }
                sleepUntilTime = std::min(sleepUntilTime, now + kWakeupInterval);
            }
            mCondition.wait_until(l, sleepUntilTime);
        }
    }
    mutable std::mutex mLock;
    std::condition_variable mCondition GUARDED_BY(mLock);
    bool mQuit GUARDED_BY(mLock) = false;
    std::multimap<std::chrono::time_point<TimerClock>, std::function<void()>>
            mMap GUARDED_BY(mLock);
    std::thread mThread;
};
}
