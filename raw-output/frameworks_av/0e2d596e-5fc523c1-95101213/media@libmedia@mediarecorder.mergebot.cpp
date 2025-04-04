/*
 **
 ** Copyright (c) 2008 The Android Open Source Project
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

#define LOG_TAG "MediaRecorder"
#include <inttypes.h>
#include <android-base/macros.h>
#include <utils/Log.h>
#include <media/mediarecorder.h>
#include <binder/IServiceManager.h>
#include <utils/String8.h>
#include <media/IMediaPlayerService.h>
#include <media/IMediaRecorder.h>
#include <media/mediaplayer.h>  // for MEDIA_ERROR_SERVER_DIED
#include <media/stagefright/PersistentSurface.h>
#include <gui/IGraphicBufferProducer.h>

namespace android {

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

MediaRecorder::~MediaRecorder()
{
    ALOGV("destructor");
    if (mMediaRecorder != NULL) {
        mMediaRecorder.clear();
    }

    if (mSurfaceMediaSource != NULL) {
        mSurfaceMediaSource.clear();
    }
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

MediaRecorder::~MediaRecorder()
{
    ALOGV("destructor");
    if (mMediaRecorder != NULL) {
        mMediaRecorder.clear();
    }

    if (mSurfaceMediaSource != NULL) {
        mSurfaceMediaSource.clear();
    }
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

status_t MediaRecorder::getPortId(audio_port_handle_t *portId) const
{
    ALOGV("getPortId");

    if (mMediaRecorder == NULL) {
        ALOGE("media recorder is not initialized yet");
        return INVALID_OPERATION;
    }
    return mMediaRecorder->getPortId(portId);
}

} // namespace android
