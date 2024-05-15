       
#include <unordered_set>
#include <android/gui/BnWindowInfosReportedListener.h>
#include <android/gui/IWindowInfosListener.h>
#include <android/gui/IWindowInfosReportedListener.h>
#include <binder/IBinder.h>
#include <ftl/small_map.h>
#include <gui/SpHash.h>
#include <utils/Mutex.h>
namespace android {
using WindowInfosReportedListenerSet =
        std::unordered_set<sp<gui::IWindowInfosReportedListener>,
                           gui::SpHash<gui::IWindowInfosReportedListener>>;
class WindowInfosListenerInvoker : public gui::BnWindowInfosReportedListener,
                                   public IBinder::DeathRecipient {
public:
    void addWindowInfosListener(sp<gui::IWindowInfosListener>);
    void removeWindowInfosListener(const sp<gui::IWindowInfosListener>& windowInfosListener);
    void windowInfosChanged(std::vector<gui::WindowInfo>, std::vector<gui::DisplayInfo>,
                            WindowInfosReportedListenerSet windowInfosReportedListeners,
                            bool forceImmediateCall);
    binder::Status onWindowInfosReported() override;
    void windowInfosChanged(std::vector<gui::WindowInfo>, std::vector<gui::DisplayInfo>,
                            bool shouldSync, bool forceImmediateCall);
protected:
    void binderDied(const wp<IBinder>& who) override;
private:
    std::mutex mListenersMutex;
    static constexpr size_t kStaticCapacity = 3;
    ftl::SmallMap<wp<IBinder>, const sp<gui::IWindowInfosListener>, kStaticCapacity>
            mWindowInfosListeners GUARDED_BY(mMessagesMutex);
    std::mutex mMessagesMutex;
    uint32_t mActiveMessageCount GUARDED_BY(mMessagesMutex);
    std::function<void(bool)> mWindowInfosChangedDelayed GUARDED_BY(mMessagesMutex);
    bool mShouldSyncDelayed;
};
}
