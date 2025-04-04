#define LOG_TAG "RTPSource"
#include <utils/Log.h>
#include "RTPSource.h"
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <string.h>
namespace android {
const int64_t kNearEOSTimeoutUs = 2000000ll;
static int32_t kMaxAllowedStaleAccessUnits = 20;
NuPlayer::RTPSource::RTPSource(
        const sp<AMessage> &notify,
        const String8& rtpParams)
    : Source(notify),
      mRTPParams(rtpParams),
      mFlags(0),
      mState(DISCONNECTED),
      mFinalResult(OK),
      mBuffering(false),
      mInPreparationPhase(true),
      mRTPConn(new ARTPConnection(ARTPConnection::kViLTEConnection)),
      mEOSTimeoutAudio(0),
      mEOSTimeoutVideo(0),
      mLastCVOUpdated(-1) {
      ALOGD("RTPSource initialized with rtpParams=%s", rtpParams.string());
}
NuPlayer::RTPSource::~RTPSource() {
    if (mLooper != NULL) {
        mLooper->unregisterHandler(id());
        mLooper->unregisterHandler(mRTPConn->id());
        mLooper->stop();
    }
}
status_t NuPlayer::RTPSource::setBufferingSettings(const BufferingSettings& buffering) {
    Mutex::Autolock _l(mBufferingSettingsLock);
    mBufferingSettings = buffering;
    return OK;
}
void NuPlayer::RTPSource::prepareAsync() {
    if (mLooper == NULL) {
        mLooper = new ALooper;
        mLooper->setName("rtp");
        mLooper->start();
        mLooper->registerHandler(this);
        mLooper->registerHandler(mRTPConn);
    }
    CHECK_EQ(mState, (int)DISCONNECTED);
    mState = CONNECTING;
    setParameters(mRTPParams);
    TrackInfo *info = NULL;
    unsigned i;
    for (i = 0; i < mTracks.size(); i++) {
        info = &mTracks.editItemAt(i);
        if (info == NULL)
            break;
        AString sdp;
        ASessionDescription::SDPStringFactory(sdp, info->mLocalIp,
                info->mIsAudio, info->mLocalPort, info->mPayloadType, info->mAS, info->mCodecName,
                NULL, info->mWidth, info->mHeight, info->mCVOExtMap);
        ALOGD("RTPSource SDP =>\n%s", sdp.c_str());
        sp<ASessionDescription> desc = new ASessionDescription;
        bool isValidSdp = desc->setTo(sdp.c_str(), sdp.size());
        ALOGV("RTPSource isValidSdp => %d", isValidSdp);
        int sockRtp, sockRtcp;
        ARTPConnection::MakeRTPSocketPair(&sockRtp, &sockRtcp, info->mLocalIp, info->mRemoteIp,
                info->mLocalPort, info->mRemotePort, info->mSocketNetwork);
        sp<AMessage> notify = new AMessage('accu', this);
        ALOGV("RTPSource addStream. track-index=%d", i);
        notify->setSize("trackIndex", i);
        mRTPConn->addStream(sockRtp, sockRtcp, desc, i + 1, notify, false);
<<<<<<< HEAD
        mRTPConn->setSelfID(info->mSelfID);
        mRTPConn->setJbTime(
                (info->mJbTimeMs <= 3000 && info->mJbTimeMs >= 40) ? info->mJbTimeMs : 300);
||||||| 7b6100a0cd
=======
        mRTPConn->setSelfID(info->mSelfID);
>>>>>>> 35aa42fa
        info->mRTPSocket = sockRtp;
        info->mRTCPSocket = sockRtcp;
        info->mFirstSeqNumInSegment = 0;
        info->mNewSegment = true;
        info->mAllowedStaleAccessUnits = kMaxAllowedStaleAccessUnits;
        info->mRTPAnchor = 0;
        info->mNTPAnchorUs = -1;
        info->mNormalPlayTimeRTP = 0;
        info->mNormalPlayTimeUs = 0ll;
        info->mPacketSource = new APacketSource(desc, i + 1);
        int32_t timeScale;
        sp<MetaData> format = getTrackFormat(i, &timeScale);
        sp<AnotherPacketSource> source = new AnotherPacketSource(format);
        if (info->mIsAudio) {
            mAudioTrack = source;
            info->mTimeScale = 16000;
        } else {
            mVideoTrack = source;
            info->mTimeScale = 90000;
        }
        info->mSource = source;
        info->mRTPTime = 0;
        info->mNormalPlaytimeUs = 0;
        info->mNPTMappingValid = false;
    }
    if (mInPreparationPhase) {
        mInPreparationPhase = false;
        notifyPrepared();
    }
}
void NuPlayer::RTPSource::pause() {
    mState = PAUSED;
}
void NuPlayer::RTPSource::resume() {
    mState = CONNECTING;
}
sp<AnotherPacketSource> NuPlayer::RTPSource::getSource(bool audio) {
    return audio ? mAudioTrack : mVideoTrack;
}
void NuPlayer::RTPSource::setEOSTimeout(bool audio, int64_t timeout) {
    if (audio) {
        mEOSTimeoutAudio = timeout;
    } else {
        mEOSTimeoutVideo = timeout;
    }
}
status_t NuPlayer::RTPSource::getDuration(int64_t *durationUs) {
    *durationUs = 0ll;
    int64_t audioDurationUs;
    if (mAudioTrack != NULL
            && mAudioTrack->getFormat()->findInt64(
                kKeyDuration, &audioDurationUs)
            && audioDurationUs > *durationUs) {
        *durationUs = audioDurationUs;
    }
    int64_t videoDurationUs;
    if (mVideoTrack != NULL
            && mVideoTrack->getFormat()->findInt64(
                kKeyDuration, &videoDurationUs)
            && videoDurationUs > *durationUs) {
        *durationUs = videoDurationUs;
    }
    return OK;
}
void NuPlayer::RTPSource::schedulePollBuffering() {
    sp<AMessage> msg = new AMessage(kWhatPollBuffering, this);
    msg->post(kBufferingPollIntervalUs);
}
void NuPlayer::RTPSource::onPollBuffering() {
    schedulePollBuffering();
}
bool NuPlayer::RTPSource::addMediaTimestamp(
        int32_t trackIndex, const TrackInfo *track,
        const sp<ABuffer> &accessUnit) {
    uint32_t rtpTime;
    CHECK(accessUnit->meta()->findInt32(
                "rtp-time", (int32_t *)&rtpTime));
    int64_t relRtpTimeUs =
        (((int64_t)rtpTime - (int64_t)track->mRTPAnchor) * 1000000ll)
        / track->mTimeScale;
    int64_t ntpTimeUs = track->mNTPAnchorUs + relRtpTimeUs;
    int64_t mediaTimeUs = mMediaAnchorUs + ntpTimeUs - mNTPAnchorUs;
    if (mediaTimeUs > mLastMediaTimeUs) {
        mLastMediaTimeUs = mediaTimeUs;
    }
    if (mediaTimeUs < 0) {
        ALOGV("dropping early accessUnit.");
        return false;
    }
    ALOGV("track %d rtpTime=%u mediaTimeUs = %lld us (%.2f secs)",
            trackIndex, rtpTime, (long long)mediaTimeUs, mediaTimeUs / 1E6);
    accessUnit->meta()->setInt64("timeUs", mediaTimeUs);
    return true;
}
void NuPlayer::RTPSource::postQueueAccessUnit(
        size_t trackIndex, const sp<ABuffer> &accessUnit) {
    sp<AMessage> msg = new AMessage(kWhatAccessUnit, this);
    msg->setInt32("what", kWhatAccessUnit);
    msg->setSize("trackIndex", trackIndex);
    msg->setBuffer("accessUnit", accessUnit);
    msg->post();
}
void NuPlayer::RTPSource::postQueueEOS(size_t trackIndex, status_t finalResult) {
    sp<AMessage> msg = new AMessage(kWhatEOS, this);
    msg->setInt32("what", kWhatEOS);
    msg->setSize("trackIndex", trackIndex);
    msg->setInt32("finalResult", finalResult);
    msg->post();
}
sp<MetaData> NuPlayer::RTPSource::getTrackFormat(size_t index, int32_t *timeScale) {
    CHECK_GE(index, 0u);
    CHECK_LT(index, mTracks.size());
    const TrackInfo &info = mTracks.itemAt(index);
    *timeScale = info.mTimeScale;
    return info.mPacketSource->getFormat();
}
status_t NuPlayer::RTPSource::setParameters(const String8 &params) {
    ALOGV("setParameters: %s", params.string());
    const char *cparams = params.string();
    const char *key_start = cparams;
    for (;;) {
        const char *equal_pos = strchr(key_start, '=');
        if (equal_pos == NULL) {
            ALOGE("Parameters %s miss a value", cparams);
            return BAD_VALUE;
        }
        String8 key(key_start, equal_pos - key_start);
        TrimString(&key);
        if (key.length() == 0) {
            ALOGE("Parameters %s contains an empty key", cparams);
            return BAD_VALUE;
        }
        const char *value_start = equal_pos + 1;
        const char *semicolon_pos = strchr(value_start, ';');
        String8 value;
        if (semicolon_pos == NULL) {
            value.setTo(value_start);
        } else {
            value.setTo(value_start, semicolon_pos - value_start);
        }
        if (setParameter(key, value) != OK) {
            return BAD_VALUE;
        }
        if (semicolon_pos == NULL) {
            break;
        }
        key_start = semicolon_pos + 1;
    }
    return OK;
}
status_t NuPlayer::RTPSource::getBufferingSettings(BufferingSettings* buffering ) {
    Mutex::Autolock _l(mBufferingSettingsLock);
    *buffering = mBufferingSettings;
    return OK;
}
void NuPlayer::RTPSource::start() {
}
void NuPlayer::RTPSource::stop() {
    if (mLooper == NULL) {
        return;
    }
    sp<AMessage> msg = new AMessage(kWhatDisconnect, this);
    sp<AMessage> dummy;
    msg->postAndAwaitResponse(&dummy);
}
status_t NuPlayer::RTPSource::feedMoreTSData() {
    Mutex::Autolock _l(mBufferingLock);
    return mFinalResult;
}
sp<MetaData> NuPlayer::RTPSource::getFormatMeta(bool audio) {
    sp<AnotherPacketSource> source = getSource(audio);
    if (source == NULL) {
        return NULL;
    }
    return source->getFormat();
}
bool NuPlayer::RTPSource::haveSufficientDataOnAllTracks() {
    static const int64_t kMinDurationUs = 2000000ll;
    int64_t mediaDurationUs = 0;
    getDuration(&mediaDurationUs);
    if ((mAudioTrack != NULL && mAudioTrack->isFinished(mediaDurationUs))
            || (mVideoTrack != NULL && mVideoTrack->isFinished(mediaDurationUs))) {
        return true;
    }
    status_t err;
    int64_t durationUs;
    if (mAudioTrack != NULL
            && (durationUs = mAudioTrack->getBufferedDurationUs(&err))
                    < kMinDurationUs
            && err == OK) {
        ALOGV("audio track doesn't have enough data yet. (%.2f secs buffered)",
              durationUs / 1E6);
        return false;
    }
    if (mVideoTrack != NULL
            && (durationUs = mVideoTrack->getBufferedDurationUs(&err))
                    < kMinDurationUs
            && err == OK) {
        ALOGV("video track doesn't have enough data yet. (%.2f secs buffered)",
              durationUs / 1E6);
        return false;
    }
    return true;
}
status_t NuPlayer::RTPSource::dequeueAccessUnit(bool audio, sp<ABuffer> *accessUnit) {
    sp<AnotherPacketSource> source = getSource(audio);
    if (mState == PAUSED) {
        ALOGV("-EWOULDBLOCK");
        return -EWOULDBLOCK;
    }
    status_t finalResult;
    if (!source->hasBufferAvailable(&finalResult)) {
        if (finalResult == OK) {
            int64_t mediaDurationUs = 0;
            getDuration(&mediaDurationUs);
            sp<AnotherPacketSource> otherSource = getSource(!audio);
            status_t otherFinalResult;
            if (otherSource != NULL &&
                    !otherSource->hasBufferAvailable(&otherFinalResult) &&
                    otherFinalResult == ERROR_END_OF_STREAM) {
                source->signalEOS(ERROR_END_OF_STREAM);
                return ERROR_END_OF_STREAM;
            }
            if (source->isFinished(mediaDurationUs)) {
                int64_t eosTimeout = audio ? mEOSTimeoutAudio : mEOSTimeoutVideo;
                if (eosTimeout == 0) {
                    setEOSTimeout(audio, ALooper::GetNowUs());
                } else if ((ALooper::GetNowUs() - eosTimeout) > kNearEOSTimeoutUs) {
                    setEOSTimeout(audio, 0);
                    source->signalEOS(ERROR_END_OF_STREAM);
                    return ERROR_END_OF_STREAM;
                }
                return -EWOULDBLOCK;
            }
            if (!(otherSource != NULL && otherSource->isFinished(mediaDurationUs))) {
            }
            return -EWOULDBLOCK;
        }
        return finalResult;
    }
    setEOSTimeout(audio, 0);
    finalResult = source->dequeueAccessUnit(accessUnit);
    if (finalResult != OK) {
        return finalResult;
    }
    int32_t cvo;
    if ((*accessUnit) != NULL && (*accessUnit)->meta()->findInt32("cvo", &cvo) &&
            cvo != mLastCVOUpdated) {
        sp<AMessage> msg = new AMessage();
        msg->setInt32("payload-type", NuPlayer::RTPSource::RTP_CVO);
        msg->setInt32("cvo", cvo);
        sp<AMessage> notify = dupNotify();
        notify->setInt32("what", kWhatIMSRxNotice);
        notify->setMessage("message", msg);
        notify->post();
        ALOGV("notify cvo updated (%d)->(%d) to upper layer", mLastCVOUpdated, cvo);
        mLastCVOUpdated = cvo;
    }
    return finalResult;
}
status_t NuPlayer::RTPSource::seekTo(int64_t seekTimeUs, MediaPlayerSeekMode mode) {
    ALOGV("RTPSource::seekTo=%d, mode=%d", (int)seekTimeUs, mode);
    return OK;
}
bool NuPlayer::RTPSource::isRealTime() const {
    ALOGD("RTPSource::isRealTime=%d", true);
    return true;
}
void NuPlayer::RTPSource::onMessageReceived(const sp<AMessage> &msg) {
    ALOGV("onMessageReceived =%d", msg->what());
    switch (msg->what()) {
        case kWhatAccessUnitComplete:
        {
            if (mState == CONNECTING) {
                mState = CONNECTED;
            }
            int32_t timeUpdate;
            if (msg->findInt32("time-update", &timeUpdate) && timeUpdate) {
                size_t trackIndex;
                CHECK(msg->findSize("trackIndex", &trackIndex));
                uint32_t rtpTime;
                uint64_t ntpTime;
                CHECK(msg->findInt32("rtp-time", (int32_t *)&rtpTime));
                CHECK(msg->findInt64("ntp-time", (int64_t *)&ntpTime));
                onTimeUpdate(trackIndex, rtpTime, ntpTime);
                break;
            }
            int32_t firstRTCP;
            if (msg->findInt32("first-rtcp", &firstRTCP)) {
                ALOGV("first-rtcp");
                break;
            }
            int32_t IMSRxNotice;
            if (msg->findInt32("rtcp-event", &IMSRxNotice)) {
                int32_t payloadType, feedbackType;
                CHECK(msg->findInt32("payload-type", &payloadType));
                CHECK(msg->findInt32("feedback-type", &feedbackType));
                sp<AMessage> notify = dupNotify();
                notify->setInt32("what", kWhatIMSRxNotice);
                notify->setMessage("message", msg);
                notify->post();
                ALOGV("IMSRxNotice \t\t payload : %d feedback : %d",
                      payloadType, feedbackType);
                break;
            }
            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));
            sp<ABuffer> accessUnit;
            if (msg->findBuffer("access-unit", &accessUnit) == false) {
                break;
            }
            int32_t damaged;
            if (accessUnit->meta()->findInt32("damaged", &damaged)
                    && damaged) {
                ALOGD("dropping damaged access unit.");
                break;
            }
            TrackInfo *info = &mTracks.editItemAt(trackIndex);
            sp<AnotherPacketSource> source = info->mSource;
            if (source != NULL) {
                uint32_t rtpTime;
                CHECK(accessUnit->meta()->findInt32("rtp-time", (int32_t *)&rtpTime));
                accessUnit->meta()->setInt64("timeUs", ALooper::GetNowUs());
                source->queueAccessUnit(accessUnit);
            }
            break;
        }
        case kWhatDisconnect:
        {
            sp<AReplyToken> replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));
            for (size_t i = 0; i < mTracks.size(); ++i) {
                TrackInfo *info = &mTracks.editItemAt(i);
                if (info->mIsAudio) {
                    mAudioTrack->signalEOS(ERROR_END_OF_STREAM);
                    mAudioTrack = NULL;
                    ALOGV("mAudioTrack disconnected");
                } else {
                    mVideoTrack->signalEOS(ERROR_END_OF_STREAM);
                    mVideoTrack = NULL;
                    ALOGV("mVideoTrack disconnected");
                }
                mRTPConn->removeStream(info->mRTPSocket, info->mRTCPSocket);
                close(info->mRTPSocket);
                close(info->mRTCPSocket);
            }
            mTracks.clear();
            mFirstAccessUnit = true;
            mAllTracksHaveTime = false;
            mNTPAnchorUs = -1;
            mMediaAnchorUs = -1;
            mLastMediaTimeUs = -1;
            mNumAccessUnitsReceived = 0;
            mReceivedFirstRTCPPacket = false;
            mReceivedFirstRTPPacket = false;
            mPausing = false;
            mPauseGeneration = 0;
            (new AMessage)->postReply(replyID);
            break;
        }
        case kWhatPollBuffering:
            break;
        default:
            TRESPASS();
    }
}
void NuPlayer::RTPSource::setTargetBitrate(int32_t bitrate) {
    mRTPConn->setTargetBitrate(bitrate);
}
void NuPlayer::RTPSource::onTimeUpdate(int32_t trackIndex, uint32_t rtpTime, uint64_t ntpTime) {
    ALOGV("onTimeUpdate track %d, rtpTime = 0x%08x, ntpTime = %#016llx",
         trackIndex, rtpTime, (long long)ntpTime);
    int64_t ntpTimeUs = (int64_t)(ntpTime * 1E6 / (1ll << 32));
    TrackInfo *track = &mTracks.editItemAt(trackIndex);
    track->mRTPAnchor = rtpTime;
    track->mNTPAnchorUs = ntpTimeUs;
    if (mNTPAnchorUs < 0) {
        mNTPAnchorUs = ntpTimeUs;
        mMediaAnchorUs = mLastMediaTimeUs;
    }
    if (!mAllTracksHaveTime) {
        bool allTracksHaveTime = (mTracks.size() > 0);
        for (size_t i = 0; i < mTracks.size(); ++i) {
            TrackInfo *track = &mTracks.editItemAt(i);
            if (track->mNTPAnchorUs < 0) {
                allTracksHaveTime = false;
                break;
            }
        }
        if (allTracksHaveTime) {
            mAllTracksHaveTime = true;
            ALOGI("Time now established for all tracks.");
        }
    }
    if (mAllTracksHaveTime && dataReceivedOnAllChannels()) {
        for (size_t i = 0; i < mTracks.size(); ++i) {
            TrackInfo *trackInfo = &mTracks.editItemAt(i);
            while (!trackInfo->mPackets.empty()) {
                sp<ABuffer> accessUnit = *trackInfo->mPackets.begin();
                trackInfo->mPackets.erase(trackInfo->mPackets.begin());
                if (addMediaTimestamp(i, trackInfo, accessUnit)) {
                    postQueueAccessUnit(i, accessUnit);
                }
            }
        }
    }
}
bool NuPlayer::RTPSource::dataReceivedOnAllChannels() {
    TrackInfo *track;
    for (size_t i = 0; i < mTracks.size(); ++i) {
        track = &mTracks.editItemAt(i);
        if (track->mPackets.empty()) {
            return false;
        }
    }
    return true;
}
void NuPlayer::RTPSource::onConnected() {
    ALOGV("onConnected");
    mState = CONNECTED;
}
void NuPlayer::RTPSource::onDisconnected(const sp<AMessage> &msg) {
    if (mState == DISCONNECTED) {
        return;
    }
    status_t err;
    CHECK(msg->findInt32("result", &err));
    CHECK_NE(err, (status_t)OK);
    if (mState == CONNECTING) {
        notifyPrepared(err);
    }
    mState = DISCONNECTED;
}
status_t NuPlayer::RTPSource::setParameter(const String8 &key, const String8 &value) {
    ALOGV("setParameter: key (%s) => value (%s)", key.string(), value.string());
    bool isAudioKey = key.contains("audio");
    TrackInfo *info = NULL;
    for (unsigned i = 0; i < mTracks.size(); ++i) {
        info = &mTracks.editItemAt(i);
        if (info != NULL && info->mIsAudio == isAudioKey) {
            ALOGV("setParameter: %s track (%d) found", isAudioKey ? "audio" : "video" , i);
            break;
        }
    }
    if (info == NULL) {
        TrackInfo newTrackInfo;
        newTrackInfo.mIsAudio = isAudioKey;
        mTracks.push(newTrackInfo);
        info = &mTracks.editTop();
        info->mJbTimeMs = 300;
    }
    if (key == "rtp-param-mime-type") {
        info->mMimeType = value;
        const char *mime = value.string();
        const char *delimiter = strchr(mime, '/');
        info->mCodecName = delimiter ? (delimiter + 1) : "<none>";
        ALOGV("rtp-param-mime-type: mMimeType (%s) => mCodecName (%s)",
                info->mMimeType.string(), info->mCodecName.string());
    } else if (key == "video-param-decoder-profile") {
        info->mCodecProfile = atoi(value);
    } else if (key == "video-param-decoder-level") {
        info->mCodecLevel = atoi(value);
    } else if (key == "video-param-width") {
        info->mWidth = atoi(value);
    } else if (key == "video-param-height") {
        info->mHeight = atoi(value);
    } else if (key == "rtp-param-local-ip") {
        info->mLocalIp = value;
    } else if (key == "rtp-param-local-port") {
        info->mLocalPort = atoi(value);
    } else if (key == "rtp-param-remote-ip") {
        info->mRemoteIp = value;
    } else if (key == "rtp-param-remote-port") {
        info->mRemotePort = atoi(value);
    } else if (key == "rtp-param-payload-type") {
        info->mPayloadType = atoi(value);
    } else if (key == "rtp-param-as") {
        info->mAS = atoi(value);
    } else if (key == "rtp-param-rtp-timeout") {
    } else if (key == "rtp-param-rtcp-timeout") {
    } else if (key == "rtp-param-time-scale") {
    } else if (key == "rtp-param-self-id") {
        info->mSelfID = atoi(value);
    } else if (key == "rtp-param-ext-cvo-extmap") {
        info->mCVOExtMap = atoi(value);
    } else if (key == "rtp-param-set-socket-network") {
        int64_t networkHandle = atoll(value);
        setSocketNetwork(networkHandle);
    } else if (key == "rtp-param-jitter-buffer-time") {
        info->mJbTimeMs = atoi(value);
    }
    return OK;
}
void NuPlayer::RTPSource::setSocketNetwork(int64_t networkHandle) {
    ALOGV("setSocketNetwork: %llu", (unsigned long long)networkHandle);
    TrackInfo *info = NULL;
    for (size_t i = 0; i < mTracks.size(); ++i) {
        info = &mTracks.editItemAt(i);
        if (info == NULL)
            break;
        info->mSocketNetwork = networkHandle;
    }
}
void NuPlayer::RTPSource::TrimString(String8 *s) {
    size_t num_bytes = s->bytes();
    const char *data = s->string();
    size_t leading_space = 0;
    while (leading_space < num_bytes && isspace(data[leading_space])) {
        ++leading_space;
    }
    size_t i = num_bytes;
    while (i > leading_space && isspace(data[i - 1])) {
        --i;
    }
    s->setTo(String8(&data[leading_space], i - leading_space));
}
}
