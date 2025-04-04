#define LOG_TAG "MPEG4Writer"
#include <inttypes.h>
#include <utils/Log.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MPEG4Writer.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/Utils.h>
#include <media/mediarecorder.h>
#include <cutils/properties.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "include/ESDS.h"
namespace android {
static const int64_t kMinStreamableFileSizeInBytes = 5 * 1024 * 1024;
static const int64_t kMax32BitFileSize = 0x007fffffffLL;
static const uint8_t kNalUnitTypeSeqParamSet = 0x07;
static const uint8_t kNalUnitTypePicParamSet = 0x08;
static const int64_t kInitialDelayTimeUs = 700000LL;
class MPEG4Writer::Track {
private:
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
    Track &operator=(const Track &);
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    template<class TYPE>
    struct ListTableEntries {
    MPEG4Writer::Track::~Track() {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;
    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
    }
    MPEG4Writer::Track::~Track() {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;
    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
    }
    void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
    }
    void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
    }
    void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
    }
    void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
    }
    void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
    }
    private:
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    };
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
    Track &operator=(const Track &);
};
MPEG4Writer::Track::~Track() {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;
    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}
MPEG4Writer::Track::~Track() {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;
    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}
MPEG4Writer::Track::~Track() {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;
    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
static bool isTestModeEnabled() {
#if (PROPERTY_VALUE_MAX < 5)
#error "PROPERTY_VALUE_MAX must be at least 5"
#endif
    char value[PROPERTY_VALUE_MAX];
    if (property_get("rw.media.record.test", value, NULL) &&
        (!strcasecmp(value, "true") || !strcasecmp(value, "1"))) {
        return true;
    }
    return false;
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
static void StripStartcode(MediaBuffer *buffer) {
    if (buffer->range_length() < 4) {
        return;
    }
    const uint8_t *ptr =
        (const uint8_t *)buffer->data() + buffer->range_offset();
    if (!memcmp(ptr, "\x00\x00\x00\x01", 4)) {
        buffer->set_range(
                buffer->range_offset() + 4, buffer->range_length() - 4);
    }
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
MPEG4Writer::Track::~Track() {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;
    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
MPEG4Writer::Track::~Track() {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries {
    stop();
    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;
    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;
    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
static void getNalUnitType(uint8_t byte, uint8_t* type) {
    ALOGV("getNalUnitType: %d", byte);
    *type = (byte & 0x1F);
}
static const uint8_t *findNextStartCode(
        const uint8_t *data, size_t length) {
    ALOGV("findNextStartCode: %p %d", data, length);
    size_t bytesLeft = length;
    while (bytesLeft > 4 &&
            memcmp("\x00\x00\x00\x01", &data[length - bytesLeft], 4)) {
        --bytesLeft;
    }
    if (bytesLeft <= 4) {
        bytesLeft = 0;
    }
    return &data[length - bytesLeft];
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}
}
