#define LOG_TAG "PlaylistFetcher"
#include <utils/Log.h>
#include "PlaylistFetcher.h"
#include "LiveDataSource.h"
#include "LiveSession.h"
#include "M3UParser.h"
#include "include/avc_utils.h"
#include "include/HTTPBase.h"
#include "include/ID3.h"
#include "mpeg2ts/AnotherPacketSource.h"
#include <media/IStreamSource.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/FileSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include <ctype.h>
#include <openssl/aes.h>
#include <openssl/md5.h>
namespace android {
const int64_t PlaylistFetcher::kMinBufferedDurationUs = 10000000ll;
<<<<<<< HEAD
const int64_t PlaylistFetcher::kMaxMonitorDelayUs = 3000000ll;
const int32_t PlaylistFetcher::kNumSkipFrames = 10;
||||||| 40659dea8e
=======
const int64_t PlaylistFetcher::kMaxMonitorDelayUs = 3000000ll;
>>>>>>> 675b80da
PlaylistFetcher::PlaylistFetcher(
        const sp<AMessage> &notify,
        const sp<LiveSession> &session,
        const char *uri)
    : mNotify(notify),
      mStartTimeUsNotify(notify->dup()),
      mSession(session),
      mURI(uri),
      mStreamTypeMask(0),
      mStartTimeUs(-1ll),
      mMinStartTimeUs(0ll),
      mStopParams(NULL),
      mLastPlaylistFetchTimeUs(-1ll),
      mSeqNumber(-1),
      mNumRetries(0),
      mStartup(true),
      mPrepared(false),
      mNextPTSTimeUs(-1ll),
      mMonitorQueueGeneration(0),
      mRefreshState(INITIAL_MINIMUM_RELOAD_DELAY),
      mFirstPTSValid(false),
      mAbsoluteTimeAnchorUs(0ll) {
    memset(mPlaylistHash, 0, sizeof(mPlaylistHash));
    mStartTimeUsNotify->setInt32("what", kWhatStartedAt);
    mStartTimeUsNotify->setInt32("streamMask", 0);
}
PlaylistFetcher::~PlaylistFetcher() {
}
int64_t PlaylistFetcher::getSegmentStartTimeUs(int32_t seqNumber) const {
    CHECK(mPlaylist != NULL);
    int32_t firstSeqNumberInPlaylist;
    if (mPlaylist->meta() == NULL || !mPlaylist->meta()->findInt32(
                "media-sequence", &firstSeqNumberInPlaylist)) {
        firstSeqNumberInPlaylist = 0;
    }
    int32_t lastSeqNumberInPlaylist =
        firstSeqNumberInPlaylist + (int32_t)mPlaylist->size() - 1;
    CHECK_GE(seqNumber, firstSeqNumberInPlaylist);
    CHECK_LE(seqNumber, lastSeqNumberInPlaylist);
    int64_t segmentStartUs = 0ll;
    for (int32_t index = 0;
            index < seqNumber - firstSeqNumberInPlaylist; ++index) {
        sp<AMessage> itemMeta;
        CHECK(mPlaylist->itemAt(
                    index, NULL , &itemMeta));
        int64_t itemDurationUs;
        CHECK(itemMeta->findInt64("durationUs", &itemDurationUs));
        segmentStartUs += itemDurationUs;
    }
    return segmentStartUs;
}
int64_t PlaylistFetcher::delayUsToRefreshPlaylist() const {
    int64_t nowUs = ALooper::GetNowUs();
    if (mPlaylist == NULL || mLastPlaylistFetchTimeUs < 0ll) {
        CHECK_EQ((int)mRefreshState, (int)INITIAL_MINIMUM_RELOAD_DELAY);
        return 0ll;
    }
    if (mPlaylist->isComplete()) {
        return (~0llu >> 1);
    }
    int32_t targetDurationSecs;
    CHECK(mPlaylist->meta()->findInt32("target-duration", &targetDurationSecs));
    int64_t targetDurationUs = targetDurationSecs * 1000000ll;
    int64_t minPlaylistAgeUs;
    switch (mRefreshState) {
        case INITIAL_MINIMUM_RELOAD_DELAY:
        {
            size_t n = mPlaylist->size();
            if (n > 0) {
                sp<AMessage> itemMeta;
                CHECK(mPlaylist->itemAt(n - 1, NULL , &itemMeta));
                int64_t itemDurationUs;
                CHECK(itemMeta->findInt64("durationUs", &itemDurationUs));
                minPlaylistAgeUs = itemDurationUs;
                break;
            }
        }
        case FIRST_UNCHANGED_RELOAD_ATTEMPT:
        {
            minPlaylistAgeUs = targetDurationUs / 2;
            break;
        }
        case SECOND_UNCHANGED_RELOAD_ATTEMPT:
        {
            minPlaylistAgeUs = (targetDurationUs * 3) / 2;
            break;
        }
        case THIRD_UNCHANGED_RELOAD_ATTEMPT:
        {
            minPlaylistAgeUs = targetDurationUs * 3;
            break;
        }
        default:
            TRESPASS();
            break;
    }
    int64_t delayUs = mLastPlaylistFetchTimeUs + minPlaylistAgeUs - nowUs;
    return delayUs > 0ll ? delayUs : 0ll;
}
status_t PlaylistFetcher::decryptBuffer(
        size_t playlistIndex, const sp<ABuffer> &buffer,
        bool first) {
    sp<AMessage> itemMeta;
    bool found = false;
    AString method;
    for (ssize_t i = playlistIndex; i >= 0; --i) {
        AString uri;
        CHECK(mPlaylist->itemAt(i, &uri, &itemMeta));
        if (itemMeta->findString("cipher-method", &method)) {
            found = true;
            break;
        }
    }
    if (!found) {
        method = "NONE";
    }
    buffer->meta()->setString("cipher-method", method.c_str());
    if (method == "NONE") {
        return OK;
    } else if (!(method == "AES-128")) {
        ALOGE("Unsupported cipher method '%s'", method.c_str());
        return ERROR_UNSUPPORTED;
    }
    AString keyURI;
    if (!itemMeta->findString("cipher-uri", &keyURI)) {
        ALOGE("Missing key uri");
        return ERROR_MALFORMED;
    }
    ssize_t index = mAESKeyForURI.indexOfKey(keyURI);
    sp<ABuffer> key;
    if (index >= 0) {
        key = mAESKeyForURI.valueAt(index);
    } else {
        status_t err = mSession->fetchFile(keyURI.c_str(), &key);
        if (err != OK) {
            ALOGE("failed to fetch cipher key from '%s'.", keyURI.c_str());
            return ERROR_IO;
        } else if (key->size() != 16) {
            ALOGE("key file '%s' wasn't 16 bytes in size.", keyURI.c_str());
            return ERROR_MALFORMED;
        }
        mAESKeyForURI.add(keyURI, key);
    }
    AES_KEY aes_key;
    if (AES_set_decrypt_key(key->data(), 128, &aes_key) != 0) {
        ALOGE("failed to set AES decryption key.");
        return UNKNOWN_ERROR;
    }
    size_t n = buffer->size();
    if (!n) {
        return OK;
    }
    CHECK(n % 16 == 0);
    if (first) {
        AString iv;
        if (itemMeta->findString("cipher-iv", &iv)) {
            if ((!iv.startsWith("0x") && !iv.startsWith("0X"))
                    || iv.size() != 16 * 2 + 2) {
                ALOGE("malformed cipher IV '%s'.", iv.c_str());
                return ERROR_MALFORMED;
            }
            memset(mAESInitVec, 0, sizeof(mAESInitVec));
            for (size_t i = 0; i < 16; ++i) {
                char c1 = tolower(iv.c_str()[2 + 2 * i]);
                char c2 = tolower(iv.c_str()[3 + 2 * i]);
                if (!isxdigit(c1) || !isxdigit(c2)) {
                    ALOGE("malformed cipher IV '%s'.", iv.c_str());
                    return ERROR_MALFORMED;
                }
                uint8_t nibble1 = isdigit(c1) ? c1 - '0' : c1 - 'a' + 10;
                uint8_t nibble2 = isdigit(c2) ? c2 - '0' : c2 - 'a' + 10;
                mAESInitVec[i] = nibble1 << 4 | nibble2;
            }
        } else {
            memset(mAESInitVec, 0, sizeof(mAESInitVec));
            mAESInitVec[15] = mSeqNumber & 0xff;
            mAESInitVec[14] = (mSeqNumber >> 8) & 0xff;
            mAESInitVec[13] = (mSeqNumber >> 16) & 0xff;
            mAESInitVec[12] = (mSeqNumber >> 24) & 0xff;
        }
    }
    AES_cbc_encrypt(
            buffer->data(), buffer->data(), buffer->size(),
            &aes_key, mAESInitVec, AES_DECRYPT);
    return OK;
}
<<<<<<< HEAD
status_t PlaylistFetcher::checkDecryptPadding(const sp<ABuffer> &buffer) {
    status_t err;
    AString method;
    CHECK(buffer->meta()->findString("cipher-method", &method));
    if (method == "NONE") {
        return OK;
    }
    uint8_t padding = 0;
    if (buffer->size() > 0) {
        padding = buffer->data()[buffer->size() - 1];
    }
    if (padding > 16) {
        return ERROR_MALFORMED;
    }
    for (size_t i = buffer->size() - padding; i < padding; i++) {
        if (buffer->data()[i] != padding) {
            return ERROR_MALFORMED;
        }
    }
    buffer->setRange(buffer->offset(), buffer->size() - padding);
    return OK;
}
void PlaylistFetcher::postMonitorQueue(int64_t delayUs, int64_t minDelayUs) {
    int64_t maxDelayUs = delayUsToRefreshPlaylist();
    if (maxDelayUs < minDelayUs) {
        maxDelayUs = minDelayUs;
    }
    if (delayUs > maxDelayUs) {
        ALOGV("Need to refresh playlist in %lld", maxDelayUs);
        delayUs = maxDelayUs;
    }
||||||| 40659dea8e
void PlaylistFetcher::postMonitorQueue(int64_t delayUs) {
=======
void PlaylistFetcher::postMonitorQueue(int64_t delayUs, int64_t minDelayUs) {
    int64_t maxDelayUs = delayUsToRefreshPlaylist();
    if (maxDelayUs < minDelayUs) {
        maxDelayUs = minDelayUs;
    }
    if (delayUs > maxDelayUs) {
        ALOGV("Need to refresh playlist in %lld", maxDelayUs);
        delayUs = maxDelayUs;
    }
>>>>>>> 675b80da
    sp<AMessage> msg = new AMessage(kWhatMonitorQueue, id());
    msg->setInt32("generation", mMonitorQueueGeneration);
    msg->post(delayUs);
}
void PlaylistFetcher::cancelMonitorQueue() {
    ++mMonitorQueueGeneration;
}
void PlaylistFetcher::startAsync(
        const sp<AnotherPacketSource> &audioSource,
        const sp<AnotherPacketSource> &videoSource,
        const sp<AnotherPacketSource> &subtitleSource,
        int64_t startTimeUs,
        int64_t minStartTimeUs,
        int32_t startSeqNumberHint) {
    sp<AMessage> msg = new AMessage(kWhatStart, id());
    uint32_t streamTypeMask = 0ul;
    if (audioSource != NULL) {
        msg->setPointer("audioSource", audioSource.get());
        streamTypeMask |= LiveSession::STREAMTYPE_AUDIO;
    }
    if (videoSource != NULL) {
        msg->setPointer("videoSource", videoSource.get());
        streamTypeMask |= LiveSession::STREAMTYPE_VIDEO;
    }
    if (subtitleSource != NULL) {
        msg->setPointer("subtitleSource", subtitleSource.get());
        streamTypeMask |= LiveSession::STREAMTYPE_SUBTITLES;
    }
    msg->setInt32("streamTypeMask", streamTypeMask);
    msg->setInt64("startTimeUs", startTimeUs);
    msg->setInt64("minStartTimeUs", minStartTimeUs);
    msg->setInt32("startSeqNumberHint", startSeqNumberHint);
    msg->post();
}
void PlaylistFetcher::pauseAsync() {
    (new AMessage(kWhatPause, id()))->post();
}
void PlaylistFetcher::stopAsync() {
    (new AMessage(kWhatStop, id()))->post();
}
void PlaylistFetcher::resumeUntilAsync(const sp<AMessage> &params) {
    AMessage* msg = new AMessage(kWhatResumeUntil, id());
    msg->setMessage("params", params);
    msg->post();
}
void PlaylistFetcher::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatStart:
        {
            status_t err = onStart(msg);
            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatStarted);
            notify->setInt32("err", err);
            notify->post();
            break;
        }
        case kWhatPause:
        {
            onPause();
            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatPaused);
            notify->post();
            break;
        }
        case kWhatStop:
        {
            onStop();
            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatStopped);
            notify->post();
            break;
        }
        case kWhatMonitorQueue:
        case kWhatDownloadNext:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));
            if (generation != mMonitorQueueGeneration) {
                break;
            }
            if (msg->what() == kWhatMonitorQueue) {
                onMonitorQueue();
            } else {
                onDownloadNext();
            }
            break;
        }
        case kWhatResumeUntil:
        {
            onResumeUntil(msg);
            break;
        }
        default:
            TRESPASS();
    }
}
status_t PlaylistFetcher::onStart(const sp<AMessage> &msg) {
    mPacketSources.clear();
    uint32_t streamTypeMask;
    CHECK(msg->findInt32("streamTypeMask", (int32_t *)&streamTypeMask));
    int64_t startTimeUs;
    int32_t startSeqNumberHint;
    CHECK(msg->findInt64("startTimeUs", &startTimeUs));
    CHECK(msg->findInt64("minStartTimeUs", (int64_t *) &mMinStartTimeUs));
    CHECK(msg->findInt32("startSeqNumberHint", &startSeqNumberHint));
    if (streamTypeMask & LiveSession::STREAMTYPE_AUDIO) {
        void *ptr;
        CHECK(msg->findPointer("audioSource", &ptr));
        mPacketSources.add(
                LiveSession::STREAMTYPE_AUDIO,
                static_cast<AnotherPacketSource *>(ptr));
    }
    if (streamTypeMask & LiveSession::STREAMTYPE_VIDEO) {
        void *ptr;
        CHECK(msg->findPointer("videoSource", &ptr));
        mPacketSources.add(
                LiveSession::STREAMTYPE_VIDEO,
                static_cast<AnotherPacketSource *>(ptr));
    }
    if (streamTypeMask & LiveSession::STREAMTYPE_SUBTITLES) {
        void *ptr;
        CHECK(msg->findPointer("subtitleSource", &ptr));
        mPacketSources.add(
                LiveSession::STREAMTYPE_SUBTITLES,
                static_cast<AnotherPacketSource *>(ptr));
    }
    mStreamTypeMask = streamTypeMask;
    mStartTimeUs = startTimeUs;
    if (mStartTimeUs >= 0ll) {
        mSeqNumber = -1;
        mStartup = true;
<<<<<<< HEAD
        mPrepared = false;
    }
    if (startSeqNumberHint >= 0) {
        mSeqNumber = startSeqNumberHint;
||||||| 40659dea8e
=======
        mPrepared = false;
>>>>>>> 675b80da
    }
    postMonitorQueue();
    return OK;
}
void PlaylistFetcher::onPause() {
    cancelMonitorQueue();
}
void PlaylistFetcher::onStop() {
    cancelMonitorQueue();
    mPacketSources.clear();
    mStreamTypeMask = 0;
}
status_t PlaylistFetcher::onResumeUntil(const sp<AMessage> &msg) {
    sp<AMessage> params;
    CHECK(msg->findMessage("params", &params));
    bool stop = false;
    for (size_t i = 0; i < mPacketSources.size(); i++) {
        sp<AnotherPacketSource> packetSource = mPacketSources.valueAt(i);
        const char *stopKey;
        int streamType = mPacketSources.keyAt(i);
        switch (streamType) {
        case LiveSession::STREAMTYPE_VIDEO:
            stopKey = "timeUsVideo";
            break;
        case LiveSession::STREAMTYPE_AUDIO:
            stopKey = "timeUsAudio";
            break;
        case LiveSession::STREAMTYPE_SUBTITLES:
            stopKey = "timeUsSubtitle";
            break;
        default:
            TRESPASS();
        }
        int64_t latestTimeUs = 0, stopTimeUs = 0;
        sp<AMessage> latestMeta = packetSource->getLatestMeta();
        if (latestMeta != NULL
                && (latestMeta->findInt64("timeUs", &latestTimeUs)
                && params->findInt64(stopKey, &stopTimeUs))) {
            int64_t diffUs = stopTimeUs - latestTimeUs;
            if (diffUs < resumeThreshold(latestMeta)) {
                stop = true;
            }
        }
    }
    if (stop) {
        for (size_t i = 0; i < mPacketSources.size(); i++) {
            mPacketSources.valueAt(i)->queueAccessUnit(mSession->createFormatChangeBuffer());
        }
        stopAsync();
        return OK;
    }
    mStopParams = params;
    postMonitorQueue();
    return OK;
}
void PlaylistFetcher::notifyError(status_t err) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatError);
    notify->setInt32("err", err);
    notify->post();
}
void PlaylistFetcher::queueDiscontinuity(
        ATSParser::DiscontinuityType type, const sp<AMessage> &extra) {
    for (size_t i = 0; i < mPacketSources.size(); ++i) {
        mPacketSources.valueAt(i)->queueDiscontinuity(type, extra);
    }
}
void PlaylistFetcher::onMonitorQueue() {
    bool downloadMore = false;
    refreshPlaylist();
    int32_t targetDurationSecs;
    int64_t targetDurationUs = kMinBufferedDurationUs;
    if (mPlaylist != NULL) {
        CHECK(mPlaylist->meta()->findInt32("target-duration", &targetDurationSecs));
        targetDurationUs = targetDurationSecs * 1000000ll;
    }
    int64_t durationToBufferUs = targetDurationUs * 3;
    if (durationToBufferUs > kMinBufferedDurationUs) {
        durationToBufferUs = kMinBufferedDurationUs;
    }
    int64_t bufferedDurationUs = 0ll;
    status_t finalResult = NOT_ENOUGH_DATA;
    if (mStreamTypeMask == LiveSession::STREAMTYPE_SUBTITLES) {
        sp<AnotherPacketSource> packetSource =
            mPacketSources.valueFor(LiveSession::STREAMTYPE_SUBTITLES);
        bufferedDurationUs =
                packetSource->getBufferedDurationUs(&finalResult);
        finalResult = OK;
    } else {
<<<<<<< HEAD
||||||| 40659dea8e
        bool first = true;
        int64_t minBufferedDurationUs = 0ll;
=======
        bool first = true;
>>>>>>> 675b80da
        for (size_t i = 0; i < mPacketSources.size(); ++i) {
            if ((mStreamTypeMask & mPacketSources.keyAt(i)) == 0) {
                continue;
            }
            int64_t bufferedStreamDurationUs =
                mPacketSources.valueAt(i)->getBufferedDurationUs(&finalResult);
<<<<<<< HEAD
            ALOGV("buffered %lld for stream %d",
                    bufferedStreamDurationUs, mPacketSources.keyAt(i));
            if (bufferedStreamDurationUs > bufferedDurationUs) {
                bufferedDurationUs = bufferedStreamDurationUs;
||||||| 40659dea8e
            if (first || bufferedDurationUs < minBufferedDurationUs) {
                minBufferedDurationUs = bufferedDurationUs;
                first = false;
=======
            if (first || bufferedStreamDurationUs < bufferedDurationUs) {
                bufferedDurationUs = bufferedStreamDurationUs;
                first = false;
>>>>>>> 675b80da
            }
        }
    }
    downloadMore = (bufferedDurationUs < durationToBufferUs);
    if (!mPrepared && bufferedDurationUs > targetDurationUs && downloadMore) {
        mPrepared = true;
        ALOGV("prepared, buffered=%lld > %lld",
                bufferedDurationUs, targetDurationUs);
        sp<AMessage> msg = mNotify->dup();
        msg->setInt32("what", kWhatTemporarilyDoneFetching);
        msg->post();
    }
    if (finalResult == OK && downloadMore) {
<<<<<<< HEAD
        ALOGV("monitoring, buffered=%lld < %lld",
                bufferedDurationUs, durationToBufferUs);
        sp<AMessage> msg = new AMessage(kWhatDownloadNext, id());
        msg->setInt32("generation", mMonitorQueueGeneration);
        msg->post(1000l);
||||||| 40659dea8e
        onDownloadNext();
=======
        ALOGV("monitoring, buffered=%lld < %lld",
                bufferedDurationUs, durationToBufferUs);
        onDownloadNext();
>>>>>>> 675b80da
    } else {
        sp<AMessage> msg = mNotify->dup();
        msg->setInt32("what", kWhatTemporarilyDoneFetching);
        msg->post();
        int64_t delayUs = mPrepared ? kMaxMonitorDelayUs : targetDurationUs / 2;
        ALOGV("pausing for %lld, buffered=%lld > %lld",
                delayUs, bufferedDurationUs, durationToBufferUs);
        postMonitorQueue(delayUs, mPrepared ? targetDurationUs * 2 : 0);
    }
}
status_t PlaylistFetcher::refreshPlaylist() {
    if (delayUsToRefreshPlaylist() <= 0) {
        bool unchanged;
        sp<M3UParser> playlist = mSession->fetchPlaylist(
                mURI.c_str(), mPlaylistHash, &unchanged);
        if (playlist == NULL) {
            if (unchanged) {
                if (mRefreshState != THIRD_UNCHANGED_RELOAD_ATTEMPT) {
                    mRefreshState = (RefreshState)(mRefreshState + 1);
                }
            } else {
                ALOGE("failed to load playlist at url '%s'", mURI.c_str());
                notifyError(ERROR_IO);
                return ERROR_IO;
            }
        } else {
            mRefreshState = INITIAL_MINIMUM_RELOAD_DELAY;
            mPlaylist = playlist;
            if (mPlaylist->isComplete() || mPlaylist->isEvent()) {
                updateDuration();
            }
        }
        mLastPlaylistFetchTimeUs = ALooper::GetNowUs();
    }
    return OK;
}
void PlaylistFetcher::onDownloadNext() {
    if (refreshPlaylist() != OK) {
        return;
    }
    int32_t firstSeqNumberInPlaylist;
    if (mPlaylist->meta() == NULL || !mPlaylist->meta()->findInt32(
                "media-sequence", &firstSeqNumberInPlaylist)) {
        firstSeqNumberInPlaylist = 0;
    }
    bool seekDiscontinuity = false;
    bool explicitDiscontinuity = false;
    const int32_t lastSeqNumberInPlaylist =
        firstSeqNumberInPlaylist + (int32_t)mPlaylist->size() - 1;
    if (mStartup && mSeqNumber >= 0
            && (mSeqNumber < firstSeqNumberInPlaylist || mSeqNumber > lastSeqNumberInPlaylist)) {
        mSeqNumber = lastSeqNumberInPlaylist;
    }
    if (mSeqNumber < 0) {
        CHECK_GE(mStartTimeUs, 0ll);
        if (mPlaylist->isComplete() || mPlaylist->isEvent()) {
            mSeqNumber = getSeqNumberForTime(mStartTimeUs);
<<<<<<< HEAD
            ALOGV("Initial sequence number for time %lld is %d from (%d .. %d)",
                    mStartTimeUs, mSeqNumber, firstSeqNumberInPlaylist,
                    lastSeqNumberInPlaylist);
||||||| 40659dea8e
=======
            ALOGV("Initial sequence number for time %lld is %ld from (%ld .. %ld)",
                    mStartTimeUs, mSeqNumber, firstSeqNumberInPlaylist,
                    lastSeqNumberInPlaylist);
>>>>>>> 675b80da
        } else {
            mSeqNumber = lastSeqNumberInPlaylist - 3;
            if (mSeqNumber < firstSeqNumberInPlaylist) {
                mSeqNumber = firstSeqNumberInPlaylist;
            }
<<<<<<< HEAD
            ALOGV("Initial sequence number for live event %d from (%d .. %d)",
                    mSeqNumber, firstSeqNumberInPlaylist,
                    lastSeqNumberInPlaylist);
||||||| 40659dea8e
=======
            ALOGV("Initial sequence number for live event %ld from (%ld .. %ld)",
                    mSeqNumber, firstSeqNumberInPlaylist,
                    lastSeqNumberInPlaylist);
>>>>>>> 675b80da
        }
        mStartTimeUs = -1ll;
    }
    if (mSeqNumber < firstSeqNumberInPlaylist
            || mSeqNumber > lastSeqNumberInPlaylist) {
        if (!mPlaylist->isComplete() && mNumRetries < kMaxNumRetries) {
            ++mNumRetries;
            if (mSeqNumber > lastSeqNumberInPlaylist) {
<<<<<<< HEAD
                int32_t targetDurationSecs;
                CHECK(mPlaylist->meta()->findInt32(
                        "target-duration", &targetDurationSecs));
                int64_t delayUs = mPlaylist->size() * targetDurationSecs *
                        1000000ll / (1 + mNumRetries);
                if (delayUs > kMaxMonitorDelayUs) {
                    delayUs = kMaxMonitorDelayUs;
                }
                ALOGV("sequence number high: %d from (%d .. %d), "
                      "monitor in %lld (retry=%d)",
                        mSeqNumber, firstSeqNumberInPlaylist,
                        lastSeqNumberInPlaylist, delayUs, mNumRetries);
                postMonitorQueue(delayUs);
||||||| 40659dea8e
                mLastPlaylistFetchTimeUs = -1;
                postMonitorQueue(3000000ll);
=======
                int32_t targetDurationSecs;
                CHECK(mPlaylist->meta()->findInt32(
                        "target-duration", &targetDurationSecs));
                int64_t delayUs = mPlaylist->size() * targetDurationSecs *
                        1000000ll / (1 + mNumRetries);
                if (delayUs > kMaxMonitorDelayUs) {
                    delayUs = kMaxMonitorDelayUs;
                }
                ALOGV("sequence number high: %ld from (%ld .. %ld), monitor in %lld (retry=%d)",
                        mSeqNumber, firstSeqNumberInPlaylist,
                        lastSeqNumberInPlaylist, delayUs, mNumRetries);
                postMonitorQueue(delayUs);
>>>>>>> 675b80da
                return;
            }
            ALOGI("We've missed the boat, restarting playback."
                  "  mStartup=%d, was  looking for %d in %d-%d",
                    mStartup, mSeqNumber, firstSeqNumberInPlaylist,
                    lastSeqNumberInPlaylist);
            mSeqNumber = lastSeqNumberInPlaylist - 3;
            if (mSeqNumber < firstSeqNumberInPlaylist) {
                mSeqNumber = firstSeqNumberInPlaylist;
            }
            explicitDiscontinuity = true;
        } else {
            ALOGE("Cannot find sequence number %d in playlist "
                 "(contains %d - %d)",
                 mSeqNumber, firstSeqNumberInPlaylist,
                 firstSeqNumberInPlaylist + mPlaylist->size() - 1);
            notifyError(ERROR_END_OF_STREAM);
            return;
        }
    }
    mNumRetries = 0;
    AString uri;
    sp<AMessage> itemMeta;
    CHECK(mPlaylist->itemAt(
                mSeqNumber - firstSeqNumberInPlaylist,
                &uri,
                &itemMeta));
    int32_t val;
    if (itemMeta->findInt32("discontinuity", &val) && val != 0) {
        explicitDiscontinuity = true;
    }
    int64_t range_offset, range_length;
    if (!itemMeta->findInt64("range-offset", &range_offset)
            || !itemMeta->findInt64("range-length", &range_length)) {
        range_offset = 0;
        range_length = -1;
    }
    ALOGV("fetching segment %d from (%d .. %d)",
          mSeqNumber, firstSeqNumberInPlaylist, lastSeqNumberInPlaylist);
    ALOGV("fetching '%s'", uri.c_str());
    sp<ABuffer> buffer;
    status_t err = mSession->fetchFile(
            uri.c_str(), &buffer, range_offset, range_length);
    if (err != OK) {
        ALOGE("failed to fetch .ts segment at url '%s'", uri.c_str());
        notifyError(err);
        return;
    }
    CHECK(buffer != NULL);
    err = decryptBuffer(mSeqNumber - firstSeqNumberInPlaylist, buffer);
    if (err == OK) {
        err = checkDecryptPadding(buffer);
    }
    if (err != OK) {
        ALOGE("decryptBuffer failed w/ error %d", err);
        notifyError(err);
        return;
    }
    if (mStartup || seekDiscontinuity || explicitDiscontinuity) {
        if (mPlaylist->isComplete() || mPlaylist->isEvent()) {
            mNextPTSTimeUs = getSegmentStartTimeUs(mSeqNumber);
        }
        if (seekDiscontinuity || explicitDiscontinuity) {
            ALOGI("queueing discontinuity (seek=%d, explicit=%d)",
                 seekDiscontinuity, explicitDiscontinuity);
            queueDiscontinuity(
                    explicitDiscontinuity
                        ? ATSParser::DISCONTINUITY_FORMATCHANGE
                        : ATSParser::DISCONTINUITY_SEEK,
                    NULL );
        }
    }
    err = extractAndQueueAccessUnits(buffer, itemMeta);
    if (err == -EAGAIN) {
        postMonitorQueue();
        return;
    }
    if (err == ERROR_OUT_OF_RANGE) {
        stopAsync();
        return;
    }
    if (err != OK) {
        notifyError(err);
        return;
    }
    ++mSeqNumber;
    postMonitorQueue();
    mStartup = false;
}
int32_t PlaylistFetcher::getSeqNumberForTime(int64_t timeUs) const {
    int32_t firstSeqNumberInPlaylist;
    if (mPlaylist->meta() == NULL || !mPlaylist->meta()->findInt32(
                "media-sequence", &firstSeqNumberInPlaylist)) {
        firstSeqNumberInPlaylist = 0;
    }
    size_t index = 0;
    int64_t segmentStartUs = 0;
    while (index < mPlaylist->size()) {
        sp<AMessage> itemMeta;
        CHECK(mPlaylist->itemAt(
                    index, NULL , &itemMeta));
        int64_t itemDurationUs;
        CHECK(itemMeta->findInt64("durationUs", &itemDurationUs));
        if (timeUs < segmentStartUs + itemDurationUs) {
            break;
        }
        segmentStartUs += itemDurationUs;
        ++index;
    }
    if (index >= mPlaylist->size()) {
        index = mPlaylist->size() - 1;
    }
    return firstSeqNumberInPlaylist + index;
}
status_t PlaylistFetcher::extractAndQueueAccessUnits(
        const sp<ABuffer> &buffer, const sp<AMessage> &itemMeta) {
    if (buffer->size() > 0 && buffer->data()[0] == 0x47) {
        if ((buffer->size() % 188) != 0) {
            ALOGE("MPEG2 transport stream is not an even multiple of 188 "
                  "bytes in length.");
            return ERROR_MALFORMED;
        }
        if (mTSParser == NULL) {
            mTSParser = new ATSParser(ATSParser::TS_TIMESTAMPS_ARE_ABSOLUTE);
        }
        if (mNextPTSTimeUs >= 0ll) {
            sp<AMessage> extra = new AMessage;
            extra->setInt64(IStreamListener::kKeyMediaTimeUs, 0);
            mTSParser->signalDiscontinuity(
                    ATSParser::DISCONTINUITY_SEEK, extra);
            mNextPTSTimeUs = -1ll;
        }
        size_t offset = 0;
        while (offset < buffer->size()) {
            status_t err = mTSParser->feedTSPacket(buffer->data() + offset, 188);
            if (err != OK) {
                return err;
            }
            offset += 188;
        }
        status_t err = OK;
        for (size_t i = mPacketSources.size(); i-- > 0;) {
            sp<AnotherPacketSource> packetSource = mPacketSources.valueAt(i);
            const char *key;
            ATSParser::SourceType type;
            const LiveSession::StreamType stream = mPacketSources.keyAt(i);
            switch (stream) {
                case LiveSession::STREAMTYPE_VIDEO:
                    type = ATSParser::VIDEO;
                    key = "timeUsVideo";
                    break;
                case LiveSession::STREAMTYPE_AUDIO:
                    type = ATSParser::AUDIO;
                    key = "timeUsAudio";
                    break;
                case LiveSession::STREAMTYPE_SUBTITLES:
                {
                    ALOGE("MPEG2 Transport streams do not contain subtitles.");
                    return ERROR_MALFORMED;
                    break;
                }
                default:
                    TRESPASS();
            }
            sp<AnotherPacketSource> source =
                static_cast<AnotherPacketSource *>(
                        mTSParser->getSource(type).get());
            if (source == NULL) {
                ALOGW("MPEG2 Transport stream does not contain %s data.",
                      type == ATSParser::VIDEO ? "video" : "audio");
                mStreamTypeMask &= ~mPacketSources.keyAt(i);
                mPacketSources.removeItemsAt(i);
                continue;
            }
            int64_t timeUs;
            sp<ABuffer> accessUnit;
            status_t finalResult;
            while (source->hasBufferAvailable(&finalResult)
                    && source->dequeueAccessUnit(&accessUnit) == OK) {
                CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));
                if (mMinStartTimeUs > 0) {
                    if (timeUs < mMinStartTimeUs) {
                        int32_t targetDuration;
                        mPlaylist->meta()->findInt32("target-duration", &targetDuration);
                        int32_t incr = (mMinStartTimeUs - timeUs) / 1000000 / targetDuration;
                        if (incr == 0) {
                            incr = 1;
                        }
                        mSeqNumber += incr;
                        err = -EAGAIN;
                        break;
                    } else {
                        int64_t startTimeUs;
                        if (mStartTimeUsNotify != NULL
                                && !mStartTimeUsNotify->findInt64(key, &startTimeUs)) {
                            mStartTimeUsNotify->setInt64(key, timeUs);
                            uint32_t streamMask = 0;
                            mStartTimeUsNotify->findInt32("streamMask", (int32_t *) &streamMask);
                            streamMask |= mPacketSources.keyAt(i);
                            mStartTimeUsNotify->setInt32("streamMask", streamMask);
                            if (streamMask == mStreamTypeMask) {
                                mStartTimeUsNotify->post();
                                mStartTimeUsNotify.clear();
                            }
                        }
                    }
                }
                if (mStopParams != NULL) {
                    int64_t stopTimeUs;
                    if (!mStopParams->findInt64(key, &stopTimeUs) || timeUs >= stopTimeUs) {
                        packetSource->queueAccessUnit(mSession->createFormatChangeBuffer());
                        mStreamTypeMask &= ~stream;
                        mPacketSources.removeItemsAt(i);
                        break;
                    }
                }
                sp<MetaData> format = source->getFormat();
                if (format != NULL) {
                    accessUnit->meta()->setObject("format", format);
                }
                accessUnit->meta()->setInt32("seq", mSeqNumber);
                packetSource->queueAccessUnit(accessUnit);
            }
            if (err != OK) {
                break;
            }
        }
        if (err != OK) {
            for (size_t i = mPacketSources.size(); i-- > 0;) {
                sp<AnotherPacketSource> packetSource = mPacketSources.valueAt(i);
                packetSource->clear();
            }
            return err;
        }
        if (!mStreamTypeMask) {
            ALOGV("ERROR OUT OF RANGE");
            return ERROR_OUT_OF_RANGE;
        }
        return OK;
    } else if (buffer->size() >= 7 && !memcmp("WEBVTT\n", buffer->data(), 7)) {
        if (mStreamTypeMask != LiveSession::STREAMTYPE_SUBTITLES) {
            ALOGE("This stream only contains subtitles.");
            return ERROR_MALFORMED;
        }
        const sp<AnotherPacketSource> packetSource =
            mPacketSources.valueFor(LiveSession::STREAMTYPE_SUBTITLES);
        int64_t durationUs;
        CHECK(itemMeta->findInt64("durationUs", &durationUs));
        buffer->meta()->setInt64("timeUs", getSegmentStartTimeUs(mSeqNumber));
        buffer->meta()->setInt64("durationUs", durationUs);
        buffer->meta()->setInt32("seq", mSeqNumber);
        packetSource->queueAccessUnit(buffer);
        return OK;
    }
    if (mNextPTSTimeUs >= 0ll) {
        mFirstPTSValid = false;
        mAbsoluteTimeAnchorUs = mNextPTSTimeUs;
        mNextPTSTimeUs = -1ll;
    }
    bool firstID3Tag = true;
    uint64_t PTS = 0;
    for (;;) {
        ID3 id3(buffer->data(), buffer->size(), true );
        if (!id3.isValid()) {
            if (firstID3Tag) {
                ALOGE("Unable to parse ID3 tag.");
                return ERROR_MALFORMED;
            } else {
                break;
            }
        }
        if (firstID3Tag) {
            bool found = false;
            ID3::Iterator it(id3, "PRIV");
            while (!it.done()) {
                size_t length;
                const uint8_t *data = it.getData(&length);
                static const char *kMatchName =
                    "com.apple.streaming.transportStreamTimestamp";
                static const size_t kMatchNameLen = strlen(kMatchName);
                if (length == kMatchNameLen + 1 + 8
                        && !strncmp((const char *)data, kMatchName, kMatchNameLen)) {
                    found = true;
                    PTS = U64_AT(&data[kMatchNameLen + 1]);
                }
                it.next();
            }
            if (!found) {
                ALOGE("Unable to extract transportStreamTimestamp from ID3 tag.");
                return ERROR_MALFORMED;
            }
        }
        buffer->setRange(
                buffer->offset() + id3.rawSize(), buffer->size() - id3.rawSize());
        firstID3Tag = false;
    }
    if (!mFirstPTSValid) {
        mFirstPTSValid = true;
        mFirstPTS = PTS;
    }
    PTS -= mFirstPTS;
    int64_t timeUs = (PTS * 100ll) / 9ll + mAbsoluteTimeAnchorUs;
    if (mStreamTypeMask != LiveSession::STREAMTYPE_AUDIO) {
        ALOGW("This stream only contains audio data!");
        mStreamTypeMask &= LiveSession::STREAMTYPE_AUDIO;
        if (mStreamTypeMask == 0) {
            return OK;
        }
    }
    sp<AnotherPacketSource> packetSource =
        mPacketSources.valueFor(LiveSession::STREAMTYPE_AUDIO);
    if (packetSource->getFormat() == NULL && buffer->size() >= 7) {
        ABitReader bits(buffer->data(), buffer->size());
        CHECK_EQ(bits.getBits(12), 0xfffu);
        bits.skipBits(3);
        bool protection_absent = bits.getBits(1) != 0;
        unsigned profile = bits.getBits(2);
        CHECK_NE(profile, 3u);
        unsigned sampling_freq_index = bits.getBits(4);
        bits.getBits(1);
        unsigned channel_configuration = bits.getBits(3);
        CHECK_NE(channel_configuration, 0u);
        bits.skipBits(2);
        sp<MetaData> meta = MakeAACCodecSpecificData(
                profile, sampling_freq_index, channel_configuration);
        meta->setInt32(kKeyIsADTS, true);
        packetSource->setFormat(meta);
    }
    int64_t numSamples = 0ll;
    int32_t sampleRate;
    CHECK(packetSource->getFormat()->findInt32(kKeySampleRate, &sampleRate));
    size_t offset = 0;
    while (offset < buffer->size()) {
        const uint8_t *adtsHeader = buffer->data() + offset;
        CHECK_LT(offset + 5, buffer->size());
        unsigned aac_frame_length =
            ((adtsHeader[3] & 3) << 11)
            | (adtsHeader[4] << 3)
            | (adtsHeader[5] >> 5);
        if (aac_frame_length == 0) {
            const uint8_t *id3Header = adtsHeader;
            if (!memcmp(id3Header, "ID3", 3)) {
                ID3 id3(id3Header, buffer->size() - offset, true);
                if (id3.isValid()) {
                    offset += id3.rawSize();
                    continue;
                };
            }
            return ERROR_MALFORMED;
        }
        CHECK_LE(offset + aac_frame_length, buffer->size());
        sp<ABuffer> unit = new ABuffer(aac_frame_length);
        memcpy(unit->data(), adtsHeader, aac_frame_length);
        int64_t unitTimeUs = timeUs + numSamples * 1000000ll / sampleRate;
        unit->meta()->setInt64("timeUs", unitTimeUs);
        numSamples += 1024;
        unit->meta()->setInt32("seq", mSeqNumber);
        packetSource->queueAccessUnit(unit);
        offset += aac_frame_length;
    }
    return OK;
}
void PlaylistFetcher::updateDuration() {
    int64_t durationUs = 0ll;
    for (size_t index = 0; index < mPlaylist->size(); ++index) {
        sp<AMessage> itemMeta;
        CHECK(mPlaylist->itemAt(
                    index, NULL , &itemMeta));
        int64_t itemDurationUs;
        CHECK(itemMeta->findInt64("durationUs", &itemDurationUs));
        durationUs += itemDurationUs;
    }
    sp<AMessage> msg = mNotify->dup();
    msg->setInt32("what", kWhatDurationUpdate);
    msg->setInt64("durationUs", durationUs);
    msg->post();
}
int64_t PlaylistFetcher::resumeThreshold(const sp<AMessage> &msg) {
    int64_t durationUs, threshold;
    if (msg->findInt64("durationUs", &durationUs)) {
        return kNumSkipFrames * durationUs;
    }
    sp<RefBase> obj;
    msg->findObject("format", &obj);
    MetaData *format = static_cast<MetaData *>(obj.get());
    const char *mime;
    CHECK(format->findCString(kKeyMIMEType, &mime));
    bool audio = !strncasecmp(mime, "audio/", 6);
    if (audio) {
        int32_t sampleRate;
        CHECK(format->findInt32(kKeySampleRate, &sampleRate));
        return kNumSkipFrames * 1000
                * (1000000 / sampleRate) ;
    } else {
        int32_t frameRate;
        if (format->findInt32(kKeyFrameRate, &frameRate) && frameRate > 0) {
            return kNumSkipFrames * (1000000 / frameRate);
        }
    }
    return 500000ll;
}
}
