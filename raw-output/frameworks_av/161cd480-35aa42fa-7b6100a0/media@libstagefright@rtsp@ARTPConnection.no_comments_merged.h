#ifndef A_RTP_CONNECTION_H_
#define A_RTP_CONNECTION_H_ 
#include <media/stagefright/foundation/AHandler.h>
#include <utils/List.h>
namespace android {
struct ABuffer;
struct ARTPSource;
struct ASessionDescription;
struct ARTPConnection : public AHandler {
    enum Flags {
        kRegularlyRequestFIR = 2,
        kViLTEConnection = 4,
    };
    explicit ARTPConnection(uint32_t flags = 0);
    void addStream(
            int rtpSocket, int rtcpSocket,
            const sp<ASessionDescription> &sessionDesc, size_t index,
            const sp<AMessage> &notify,
            bool injected);
    void removeStream(int rtpSocket, int rtcpSocket);
    void injectPacket(int index, const sp<ABuffer> &buffer);
<<<<<<< HEAD
    void setSelfID(const uint32_t selfID);
    void setJbTime(const uint32_t jbTimeMs);
    void setTargetBitrate(int32_t targetBitrate);
||||||| 7b6100a0cd
=======
    void setSelfID(const uint32_t selfID);
>>>>>>> 35aa42fa
    static void MakePortPair(
            int *rtpSocket, int *rtcpSocket, unsigned *rtpPort);
    static void MakeRTPSocketPair(
            int *rtpSocket, int *rtcpSocket,
            const char *localIp, const char *remoteIp,
            unsigned localPort, unsigned remotePort, int64_t socketNetwork = 0);
protected:
    virtual ~ARTPConnection();
    virtual void onMessageReceived(const sp<AMessage> &msg);
private:
    enum {
        kWhatAddStream,
        kWhatRemoveStream,
        kWhatPollStreams,
        kWhatInjectPacket,
    };
    static const int64_t kSelectTimeoutUs;
    uint32_t mFlags;
    struct StreamInfo;
    List<StreamInfo> mStreams;
    bool mPollEventPending;
    int64_t mLastReceiverReportTimeUs;
    int64_t mLastBitrateReportTimeUs;
    int32_t mSelfID;
    int32_t mTargetBitrate;
    uint32_t mJbTimeMs;
    int32_t mCumulativeBytes;
    int32_t mSelfID;
    void onAddStream(const sp<AMessage> &msg);
    void onRemoveStream(const sp<AMessage> &msg);
    void onPollStreams();
    void onInjectPacket(const sp<AMessage> &msg);
    void onSendReceiverReports();
    void checkRxBitrate(int64_t nowUs);
    status_t receive(StreamInfo *info, bool receiveRTP);
    ssize_t send(const StreamInfo *info, const sp<ABuffer> buffer);
    status_t parseRTP(StreamInfo *info, const sp<ABuffer> &buffer);
    status_t parseRTPExt(StreamInfo *s, const uint8_t *extData, size_t extLen, int32_t *cvoDegrees);
    status_t parseRTCP(StreamInfo *info, const sp<ABuffer> &buffer);
    status_t parseSR(StreamInfo *info, const uint8_t *data, size_t size);
    status_t parseTSFB(StreamInfo *info, const uint8_t *data, size_t size);
    status_t parsePSFB(StreamInfo *info, const uint8_t *data, size_t size);
    status_t parseBYE(StreamInfo *info, const uint8_t *data, size_t size);
    sp<ARTPSource> findSource(StreamInfo *info, uint32_t id);
    void postPollEvent();
    DISALLOW_EVIL_CONSTRUCTORS(ARTPConnection);
};
}
#endif
