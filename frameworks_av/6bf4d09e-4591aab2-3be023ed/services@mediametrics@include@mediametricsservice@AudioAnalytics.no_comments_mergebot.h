       
#include <android-base/thread_annotations.h>
#include "AnalyticsActions.h"
#include "AnalyticsState.h"
#include "AudioPowerUsage.h"
#include "HeatMap.h"
#include "StatsdLog.h"
#include "TimedAction.h"
#include "Wrap.h"
namespace android::mediametrics {
class AudioAnalytics {
    friend AudioPowerUsage;
  public:
    explicit AudioAnalytics(const std::shared_ptr<StatsdLog>& statsdLog);
    ~AudioAnalytics();
    status_t submit(const std::shared_ptr<const mediametrics::Item>& item, bool isTrusted);
    std::pair<std::string, int32_t> dump(int32_t lines = INT32_MAX, int64_t sinceNs = 0,
                                         const char* prefix = nullptr) const;
    std::pair<std::string, int32_t> dumpHeatMap(int32_t lines = INT32_MAX) const {
        return mHeatMap.dump(lines);
    }
    std::pair<std::string, int32_t> dumpHealth(int32_t lines = INT32_MAX) const {
        return mHealth.dump(lines);
    }
    std::pair<std::string, int32_t> dumpSpatializer(int32_t lines = INT32_MAX) const {
        return mSpatializer.dump(lines);
    }
    void clear() {
        mPreviousAnalyticsState->clear();
        mAnalyticsState->clear();
        mHeatMap.clear();
        mAudioPowerUsage.clear();
    }
  private:
    void processActions(const std::shared_ptr<const mediametrics::Item>& item);
    void processStatus(const std::shared_ptr<const mediametrics::Item>& item);
    bool reportAudioRecordStatus(const std::shared_ptr<const mediametrics::Item>& item,
                                 const std::string& key, const std::string& eventStr,
                                 const std::string& statusString, uid_t uid,
                                 const std::string& message, int32_t subCode) const;
    bool reportAudioTrackStatus(const std::shared_ptr<const mediametrics::Item>& item,
                                const std::string& key, const std::string& eventStr,
                                const std::string& statusString, uid_t uid,
                                const std::string& message, int32_t subCode) const;
    std::string getThreadFromTrack(const std::string& track) const;
    const bool mDeliverStatistics;
    AnalyticsActions mActions;
    SharedPtrWrap<AnalyticsState> mAnalyticsState;
    SharedPtrWrap<AnalyticsState> mPreviousAnalyticsState;
    TimedAction mTimedAction;
    const std::shared_ptr<StatsdLog> mStatsdLog;
    static constexpr size_t kHeatEntries = 100;
    HeatMap mHeatMap{kHeatEntries};
    class DeviceUse {
      public:
        enum ItemType {
            RECORD = 0,
            THREAD = 1,
            TRACK = 2,
        };
        explicit DeviceUse(AudioAnalytics& audioAnalytics) : mAudioAnalytics{audioAnalytics} {}
        void endAudioIntervalGroup(const std::shared_ptr<const android::mediametrics::Item>& item,
                                   ItemType itemType) const;
      private:
        AudioAnalytics& mAudioAnalytics;
    } mDeviceUse{*this};
    class DeviceConnection {
      public:
        explicit DeviceConnection(AudioAnalytics& audioAnalytics)
            : mAudioAnalytics{audioAnalytics} {}
        void a2dpConnected(const std::shared_ptr<const android::mediametrics::Item>& item);
        void createPatch(const std::shared_ptr<const android::mediametrics::Item>& item);
        void postBluetoothA2dpDeviceConnectionStateSuppressNoisyIntent(
                const std::shared_ptr<const android::mediametrics::Item>& item);
        void expire();
      private:
        AudioAnalytics& mAudioAnalytics;
        mutable std::mutex mLock;
        std::string mA2dpDeviceName;
        int64_t mA2dpConnectionRequestNs GUARDED_BY(mLock) = 0;
        int64_t mA2dpConnectionServiceNs GUARDED_BY(mLock) = 0;
        int32_t mA2dpConnectionRequests GUARDED_BY(mLock) = 0;
        int32_t mA2dpConnectionServices GUARDED_BY(mLock) = 0;
        int32_t mA2dpConnectionSuccesses GUARDED_BY(mLock) = 0;
        int32_t mA2dpConnectionJavaServiceCancels GUARDED_BY(mLock) = 0;
        int32_t mA2dpConnectionUnknowns GUARDED_BY(mLock) = 0;
    } mDeviceConnection{*this};
    class AAudioStreamInfo {
      public:
        enum CallerPath {
            CALLER_PATH_UNKNOWN = 0,
            CALLER_PATH_LEGACY = 1,
            CALLER_PATH_MMAP = 2,
        };
        explicit AAudioStreamInfo(AudioAnalytics& audioAnalytics)
            : mAudioAnalytics(audioAnalytics) {}
        void endAAudioStream(const std::shared_ptr<const android::mediametrics::Item>& item,
                             CallerPath path) const;
      private:
        AudioAnalytics& mAudioAnalytics;
    } mAAudioStreamInfo{*this};
    void newState();
    class Health {
      public:
        explicit Health(AudioAnalytics& audioAnalytics) : mAudioAnalytics(audioAnalytics) {}
        enum class Module {
            AUDIOFLINGER,
            AUDIOPOLICY,
        };
        const char* getModuleName(Module module) {
            switch (module) {
                case Module::AUDIOFLINGER:
                    return "AudioFlinger";
                case Module::AUDIOPOLICY:
                    return "AudioPolicy";
            }
            return "Unknown";
        }
        void onAudioServerStart(Module module,
                                const std::shared_ptr<const android::mediametrics::Item>& item);
        void onAudioServerTimeout(Module module,
                                  const std::shared_ptr<const android::mediametrics::Item>& item);
        std::pair<std::string, int32_t> dump(int32_t lines = INT32_MAX,
                                             const char* prefix = nullptr) const;
      private:
        AudioAnalytics& mAudioAnalytics;
        mutable std::mutex mLock;
        std::chrono::system_clock::time_point mAudioFlingerCtorTime GUARDED_BY(mLock);
        std::chrono::system_clock::time_point mAudioPolicyCtorTime GUARDED_BY(mLock);
        std::chrono::system_clock::time_point mAudioPolicyCtorDoneTime GUARDED_BY(mLock);
        std::chrono::system_clock::time_point mStopTime GUARDED_BY(mLock);
        int64_t mStartCount GUARDED_BY(mLock) = 0;
        int64_t mStopCount GUARDED_BY(mLock) = 0;
        SimpleLog mSimpleLog GUARDED_BY(mLock){64};
    } mHealth{*this};
    class Spatializer {
      public:
        explicit Spatializer(AudioAnalytics& audioAnalytics) : mAudioAnalytics(audioAnalytics) {}
        void onEvent(const std::shared_ptr<const android::mediametrics::Item>& item);
        std::pair<std::string, int32_t> dump(int32_t lines = INT32_MAX,
                                             const char* prefix = nullptr) const;
      private:
        struct DeviceState {
            std::string enabled;
            std::string hasHeadTracker;
            std::string headTrackerEnabled;
        };
        AudioAnalytics& mAudioAnalytics;
        mutable std::mutex mLock;
        std::map<std::string, DeviceState> mDeviceStateMap GUARDED_BY(mLock);
        SimpleLog mSimpleLog GUARDED_BY(mLock){64};
    } mSpatializer{*this};
    AudioPowerUsage mAudioPowerUsage;
};
}
