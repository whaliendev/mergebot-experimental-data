/*
**
** Copyright 2010, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/
//#define LOG_NDEBUG 0

#define LOG_TAG "AudioEffect"
#include <stdint.h>
#include <sys/types.h>
#include <limits.h>
#include <private/media/AudioEffectShared.h>
#include <media/AudioEffect.h>
#include <utils/Log.h>
#include <binder/IPCThreadState.h>

namespace android {

// ---------------------------------------------------------------------------

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


// -------------------------------------------------------------------------

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


// -------------------------------------------------------------------------

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


// -------------------------------------------------------------------------

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


// -------------------------------------------------------------------------

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


// -------------------------------------------------------------------------

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


} // namespace android
