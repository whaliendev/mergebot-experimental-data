#ifndef A_AVC_ASSEMBLER_H_
#define A_AVC_ASSEMBLER_H_ 
#include "ARTPAssembler.h"
#include <utils/List.h>
#include <utils/RefBase.h>
namespace android {
struct ABuffer;
struct AMessage;
struct AAVCAssembler : public ARTPAssembler {
    explicit AAVCAssembler(const sp<AMessage> &notify);
    typedef List<sp<ABuffer> > Queue;
protected:
    virtual ~AAVCAssembler();
    virtual AssemblyStatus assembleMore(const sp<ARTPSource> &source);
    virtual void onByeReceived();
    virtual void packetLost();
private:
    sp<AMessage> mNotifyMsg;
    uint32_t mAccessUnitRTPTime;
    bool mNextExpectedSeqNoValid;
    uint32_t mNextExpectedSeqNo;
    bool mAccessUnitDamaged;
<<<<<<< HEAD
    bool mFirstIFrameProvided;
    uint64_t mLastIFrameProvidedAtMs;
    int32_t mWidth;
    int32_t mHeight;
||||||| de616ef923
=======
    bool mFirstIFrameProvided;
    uint64_t mLastIFrameProvidedAtMs;
>>>>>>> b9f2ba2d
    List<sp<ABuffer> > mNALUnits;
<<<<<<< HEAD
    int32_t addNack(const sp<ARTPSource> &source);
    void checkSpsUpdated(const sp<ABuffer> &buffer);
    void checkIFrameProvided(const sp<ABuffer> &buffer);
    bool dropFramesUntilIframe(const sp<ABuffer> &buffer);
||||||| de616ef923
=======
    int32_t addNack(const sp<ARTPSource> &source);
    void checkIFrameProvided(const sp<ABuffer> &buffer);
>>>>>>> b9f2ba2d
    AssemblyStatus addNALUnit(const sp<ARTPSource> &source);
    void addSingleNALUnit(const sp<ABuffer> &buffer);
    AssemblyStatus addFragmentedNALUnit(List<sp<ABuffer> > *queue);
    bool addSingleTimeAggregationPacket(const sp<ABuffer> &buffer);
    void submitAccessUnit();
    int32_t pickProperSeq(const Queue *q, uint32_t jit, int64_t play);
    bool recycleUnit(uint32_t start, uint32_t end, uint32_t connected,
            size_t avail, float goodRatio);
    int32_t deleteUnitUnderSeq(Queue *q, uint32_t seq);
    void printNowTimeUs(int64_t start, int64_t now, int64_t play);
    void printRTPTime(uint32_t rtp, int64_t play, uint32_t exp, bool isExp);
    DISALLOW_EVIL_CONSTRUCTORS(AAVCAssembler);
};
}
#endif
