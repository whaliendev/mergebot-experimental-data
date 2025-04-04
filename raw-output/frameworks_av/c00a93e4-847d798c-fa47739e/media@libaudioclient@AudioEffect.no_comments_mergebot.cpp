#define LOG_TAG "AudioEffect"
#include <stdint.h>
#include <sys/types.h>
#include <limits.h>
#include <private/media/AudioEffectShared.h>
#include <media/AudioEffect.h>
#include <utils/Log.h>
#include <binder/IPCThreadState.h>
namespace android {
AudioEffect::~AudioEffect()
{
    ALOGV("Destructor %p", this);
    if (mStatus == NO_ERROR || mStatus == ALREADY_EXISTS) {
        if (!audio_is_global_session(mSessionId)) {
            AudioSystem::releaseAudioSessionId(mSessionId, mClientPid);
        }
        if (mIEffect != NULL) {
            mIEffect->disconnect();
            IInterface::asBinder(mIEffect)->unlinkToDeath(mIEffectClient);
        }
        mIEffect.clear();
        mCblkMemory.clear();
        mIEffectClient.clear();
        IPCThreadState::self()->flushCommands();
    }
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
AudioEffect::~AudioEffect()
{
    ALOGV("Destructor %p", this);
    if (mStatus == NO_ERROR || mStatus == ALREADY_EXISTS) {
        if (!audio_is_global_session(mSessionId)) {
            AudioSystem::releaseAudioSessionId(mSessionId, mClientPid);
        }
        if (mIEffect != NULL) {
            mIEffect->disconnect();
            IInterface::asBinder(mIEffect)->unlinkToDeath(mIEffectClient);
        }
        mIEffect.clear();
        mCblkMemory.clear();
        mIEffectClient.clear();
        IPCThreadState::self()->flushCommands();
    }
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
status_t AudioEffect::guidToString(const effect_uuid_t *guid, char *str, size_t maxLen)
{
    if (guid == NULL || str == NULL) {
        return BAD_VALUE;
    }
    snprintf(str, maxLen, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            guid->timeLow,
            guid->timeMid,
            guid->timeHiAndVersion,
            guid->clockSeq,
            guid->node[0],
            guid->node[1],
            guid->node[2],
            guid->node[3],
            guid->node[4],
            guid->node[5]);
    return NO_ERROR;
}
}
