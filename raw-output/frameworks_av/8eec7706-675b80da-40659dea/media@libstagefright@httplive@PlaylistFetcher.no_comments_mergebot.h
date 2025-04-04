#ifndef PLAYLIST_FETCHER_H_
#define PLAYLIST_FETCHER_H_ 
#include <media/stagefright/foundation/AHandler.h>
#include "mpeg2ts/ATSParser.h"
#include "LiveSession.h"
namespace android {
struct ABuffer;
struct AnotherPacketSource;
struct DataSource;
struct HTTPBase;
struct LiveDataSource;
struct M3UParser;
struct String8;
struct PlaylistFetcher : public AHandler {
    enum {
        kWhatStarted,
        kWhatPaused,
        kWhatStopped,
        kWhatError,
        kWhatDurationUpdate,
        kWhatTemporarilyDoneFetching,
        kWhatPrepared,
        kWhatPreparationFailed,
        kWhatStartedAt,
    };
    PlaylistFetcher(
            const sp<AMessage> &notify,
            const sp<LiveSession> &session,
            const char *uri);
    sp<DataSource> getDataSource();
    void startAsync(
            const sp<AnotherPacketSource> &audioSource,
            const sp<AnotherPacketSource> &videoSource,
            const sp<AnotherPacketSource> &subtitleSource,
            int64_t startTimeUs = -1ll,
            int64_t minStartTimeUs = 0ll ,
            int32_t startSeqNumberHint = -1 );
    void pauseAsync();
    void stopAsync();
    void resumeUntilAsync(const sp<AMessage> &params);
protected:
    virtual ~PlaylistFetcher();
    virtual void onMessageReceived(const sp<AMessage> &msg);
private:
    enum {
        kMaxNumRetries = 5,
    };
    enum {
        kWhatStart = 'strt',
        kWhatPause = 'paus',
        kWhatStop = 'stop',
        kWhatMonitorQueue = 'moni',
        kWhatResumeUntil = 'rsme',
        kWhatDownloadNext = 'dlnx',
    };
    static const int64_t kMinBufferedDurationUs;
    static const int64_t kMaxMonitorDelayUs;
    static const int32_t kNumSkipFrames;
    sp<AMessage> mNotify;
    sp<AMessage> mStartTimeUsNotify;
    sp<LiveSession> mSession;
    AString mURI;
    uint32_t mStreamTypeMask;
    int64_t mStartTimeUs;
int64_t mMinStartTimeUs;sp<AMessage> mStopParams;
    KeyedVector<LiveSession::StreamType, sp<AnotherPacketSource> >
        mPacketSources;
    int64_t mLastPlaylistFetchTimeUs;
    sp<M3UParser> mPlaylist;
    int32_t mSeqNumber;
    int32_t mNumRetries;
    bool mStartup;
    bool mPrepared;
    int64_t mNextPTSTimeUs;
    int32_t mMonitorQueueGeneration;
    enum RefreshState {
        INITIAL_MINIMUM_RELOAD_DELAY,
        FIRST_UNCHANGED_RELOAD_ATTEMPT,
        SECOND_UNCHANGED_RELOAD_ATTEMPT,
        THIRD_UNCHANGED_RELOAD_ATTEMPT
    };
    RefreshState mRefreshState;
<<<<<<< HEAD
const int32_t PlaylistFetcher::kNumSkipFrames = 10;
||||||| 40659dea8e
<<<<<<< HEAD
const int32_t PlaylistFetcher::kNumSkipFrames = 10;
||||||| 40659dea8e
const int64_t PlaylistFetcher::kMinBufferedDurationUs = 10000000ll;
=======
const int64_t PlaylistFetcher::kMaxMonitorDelayUs = 3000000ll;
>>>>>>> 675b80da
=======
const int64_t PlaylistFetcher::kMaxMonitorDelayUs = 3000000ll;
>>>>>>> 675b80da
    sp<ATSParser> mTSParser;
    bool mFirstPTSValid;
    uint64_t mFirstPTS;
    int64_t mAbsoluteTimeAnchorUs;
    unsigned char mAESInitVec[16];
    status_t decryptBuffer(
            size_t playlistIndex, const sp<ABuffer> &buffer,
            bool first = true);
    status_t checkDecryptPadding(const sp<ABuffer> &buffer);
    void postMonitorQueue(int64_t delayUs = 0, int64_t minDelayUs = 0);
    void cancelMonitorQueue();
    int64_t delayUsToRefreshPlaylist() const;
    status_t refreshPlaylist();
    int64_t getSegmentStartTimeUs(int32_t seqNumber) const;
    status_t onStart(const sp<AMessage> &msg);
    void onPause();
    void onStop();
    void onMonitorQueue();
    void onDownloadNext();
    status_t onResumeUntil(const sp<AMessage> &msg);
    status_t extractAndQueueAccessUnits(
            const sp<ABuffer> &buffer, const sp<AMessage> &itemMeta);
    void notifyError(status_t err);
    void queueDiscontinuity(
            ATSParser::DiscontinuityType type, const sp<AMessage> &extra);
    int32_t getSeqNumberForTime(int64_t timeUs) const;
    void updateDuration();
    int64_t resumeThreshold(const sp<AMessage> &msg);
<<<<<<< HEAD
const int32_t PlaylistFetcher::kNumSkipFrames = 10;
||||||| 40659dea8e
<<<<<<< HEAD
const int32_t PlaylistFetcher::kNumSkipFrames = 10;
||||||| 40659dea8e
const int64_t PlaylistFetcher::kMinBufferedDurationUs = 10000000ll;
=======
const int64_t PlaylistFetcher::kMaxMonitorDelayUs = 3000000ll;
>>>>>>> 675b80da
=======
const int64_t PlaylistFetcher::kMaxMonitorDelayUs = 3000000ll;
>>>>>>> 675b80da
};
}
#endif
