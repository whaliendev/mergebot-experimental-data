/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "SessionMap.h"
#include "FwdLockEngine.h"
#include <utils/Log.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include "drm_framework_common.h"
#include <fcntl.h>
#include <limits.h>
#include <DrmRights.h>
#include <DrmConstraints.h>
#include <DrmMetadata.h>
#include <DrmInfo.h>
#include <DrmInfoStatus.h>
#include <DrmInfoRequest.h>
#include <DrmSupportInfo.h>
#include <DrmConvertedStatus.h>
#include <utils/String8.h>
#include "FwdLockConv.h"
#include "FwdLockFile.h"
#include "FwdLockGlue.h"
#include "FwdLockEngineConst.h"
#include "MimeTypeUtil.h"

#undef LOG_TAG
#define LOG_TAG "FwdLockEngine"

#ifdef DRM_OMA_FL_ENGINE_DEBUG
#define LOG_NDEBUG 0
#define LOG_VERBOSE(...) LOGV(__VA_ARGS__)
#else
#define LOG_VERBOSE(...)
#endif

using namespace android;
// This extern "C" is mandatory to be managed by TPlugInManager
extern "C" IDrmEngine* create() {
    return new FwdLockEngine();
}

// This extern "C" is mandatory to be managed by TPlugInManager
extern "C" void destroy(IDrmEngine* plugIn) {
    delete plugIn;
}

FwdLockEngine::FwdLockEngine() {
<<<<<<< ours
    LOGV("FwdLockEngine Construction");
||||||| base
    LOGD("FwdLockEngine Construction");
=======
    LOG_VERBOSE("FwdLockEngine Construction");
>>>>>>> theirs
}

FwdLockEngine::~FwdLockEngine() {
<<<<<<< ours
    LOGV("FwdLockEngine Destruction");
||||||| base
    LOGD("FwdLockEngine Destruction");
=======
    LOG_VERBOSE("FwdLockEngine Destruction");

    int size = decodeSessionMap.getSize();
>>>>>>> theirs

    for (int i = 0; i < size; i++) {
        DecodeSession *session = (DecodeSession*) decodeSessionMap.getValueAt(i);
        FwdLockFile_detach(session->fileDesc);
        ::close(session->fileDesc);
    }

    size = convertSessionMap.getSize();
    for (int i = 0; i < size; i++) {
        ConvertSession *convSession = (ConvertSession*) convertSessionMap.getValueAt(i);
        FwdLockConv_CloseSession(convSession->uniqueId, &(convSession->output));
    }
}

int FwdLockEngine::getConvertedStatus(FwdLockConv_Status_t status) {
    int retStatus = DrmConvertedStatus::STATUS_ERROR;

    switch(status) {
        case FwdLockConv_Status_OK:
            retStatus = DrmConvertedStatus::STATUS_OK;
            break;
        case FwdLockConv_Status_SyntaxError:
        case FwdLockConv_Status_InvalidArgument:
        case FwdLockConv_Status_UnsupportedFileFormat:
        case FwdLockConv_Status_UnsupportedContentTransferEncoding:
            LOGE("FwdLockEngine getConvertedStatus: file conversion Error %d. "
                  "Returning STATUS_INPUTDATA_ERROR", status);
            retStatus = DrmConvertedStatus::STATUS_INPUTDATA_ERROR;
            break;
        default:
            LOGE("FwdLockEngine getConvertedStatus: file conversion Error %d. "
                  "Returning STATUS_ERROR", status);
            retStatus = DrmConvertedStatus::STATUS_ERROR;
            break;
    }

    return retStatus;
}

DrmConstraints* FwdLockEngine::onGetConstraints(int uniqueId, const String8* path, int action) {
    DrmConstraints* drmConstraints = NULL;

<<<<<<< ours
    LOGV("FwdLockEngine::onGetConstraints");
||||||| base
    LOGD("FwdLockEngine::onGetConstraints");
=======
    LOG_VERBOSE("FwdLockEngine::onGetConstraints");
>>>>>>> theirs

    if (NULL != path &&
        (RightsStatus::RIGHTS_VALID == onCheckRightsStatus(uniqueId, *path, action))) {
        // Return the empty constraints to show no error condition.
        drmConstraints = new DrmConstraints();
    }

    return drmConstraints;
}

DrmMetadata* FwdLockEngine::onGetMetadata(int uniqueId, const String8* path) {
    DrmMetadata* drmMetadata = NULL;

<<<<<<< ours
    LOGV("FwdLockEngine::onGetMetadata");
||||||| base
    LOGD("FwdLockEngine::onGetMetadata");
=======
    LOG_VERBOSE("FwdLockEngine::onGetMetadata");
>>>>>>> theirs

    if (NULL != path) {
        // Returns empty metadata to show no error condition.
        drmMetadata = new DrmMetadata();
    }

    return drmMetadata;
}

android::status_t FwdLockEngine::onInitialize(int uniqueId) {
<<<<<<< ours
    LOGV("FwdLockEngine::onInitialize");

||||||| base
    LOGD("FwdLockEngine::onInitialize");

=======
    LOG_VERBOSE("FwdLockEngine::onInitialize");
>>>>>>> theirs

    if (FwdLockGlue_InitializeKeyEncryption()) {
<<<<<<< ours
        LOGV("FwdLockEngine::onInitialize -- FwdLockGlue_InitializeKeyEncryption succeeded");
||||||| base
        LOGD("FwdLockEngine::onInitialize -- FwdLockGlue_InitializeKeyEncryption succeeded");
=======
        LOG_VERBOSE("FwdLockEngine::onInitialize -- FwdLockGlue_InitializeKeyEncryption succeeded");
>>>>>>> theirs
    } else {
        LOGE("FwdLockEngine::onInitialize -- FwdLockGlue_InitializeKeyEncryption failed:"
             "errno = %d", errno);
    }

    return DRM_NO_ERROR;
}

android::status_t
FwdLockEngine::onSetOnInfoListener(int uniqueId, const IDrmEngine::OnInfoListener* infoListener) {
    // Not used
<<<<<<< ours
    LOGV("FwdLockEngine::onSetOnInfoListener");
||||||| base
    LOGD("FwdLockEngine::onSetOnInfoListener");
=======
    LOG_VERBOSE("FwdLockEngine::onSetOnInfoListener");
>>>>>>> theirs

    return DRM_NO_ERROR;
}

android::status_t FwdLockEngine::onTerminate(int uniqueId) {
<<<<<<< ours
    LOGV("FwdLockEngine::onTerminate");
||||||| base
    LOGD("FwdLockEngine::onTerminate");
=======
    LOG_VERBOSE("FwdLockEngine::onTerminate");
>>>>>>> theirs

    return DRM_NO_ERROR;
}

DrmSupportInfo* FwdLockEngine::onGetSupportInfo(int uniqueId) {
    DrmSupportInfo* pSupportInfo = new DrmSupportInfo();

<<<<<<< ours
    LOGV("FwdLockEngine::onGetSupportInfo");
||||||| base
    LOGD("FwdLockEngine::onGetSupportInfo");
=======
    LOG_VERBOSE("FwdLockEngine::onGetSupportInfo");
>>>>>>> theirs

    // fill all Forward Lock mimetypes and extensions
    if (NULL != pSupportInfo) {
        pSupportInfo->addMimeType(String8(FWDLOCK_MIMETYPE_FL));
        pSupportInfo->addFileSuffix(String8(FWDLOCK_DOTEXTENSION_FL));
        pSupportInfo->addMimeType(String8(FWDLOCK_MIMETYPE_DM));
        pSupportInfo->addFileSuffix(String8(FWDLOCK_DOTEXTENSION_DM));

        pSupportInfo->setDescription(String8(FWDLOCK_DESCRIPTION));
    }

    return pSupportInfo;
}

bool FwdLockEngine::onCanHandle(int uniqueId, const String8& path) {
    bool result = false;

    String8 extString = path.getPathExtension();

    extString.toLower();

    if ((extString == String8(FWDLOCK_DOTEXTENSION_FL)) ||
        (extString == String8(FWDLOCK_DOTEXTENSION_DM))) {
        result = true;
    }
    return result;
}

DrmInfoStatus* FwdLockEngine::onProcessDrmInfo(int uniqueId, const DrmInfo* drmInfo) {
    DrmInfoStatus *drmInfoStatus = NULL;

    // Nothing to process

    drmInfoStatus = new DrmInfoStatus((int)DrmInfoStatus::STATUS_OK, 0, NULL, String8(""));

<<<<<<< ours
    LOGV("FwdLockEngine::onProcessDrmInfo");
||||||| base
    LOGD("FwdLockEngine::onProcessDrmInfo");
=======
    LOG_VERBOSE("FwdLockEngine::onProcessDrmInfo");
>>>>>>> theirs

    return drmInfoStatus;
}

status_t FwdLockEngine::onSaveRights(
            int uniqueId,
            const DrmRights& drmRights,
            const String8& rightsPath,
            const String8& contentPath) {
    // No rights to save. Return
<<<<<<< ours
    LOGV("FwdLockEngine::onSaveRights");
||||||| base
    LOGD("FwdLockEngine::onSaveRights");
=======
    LOG_VERBOSE("FwdLockEngine::onSaveRights");
>>>>>>> theirs
    return DRM_ERROR_UNKNOWN;
}

DrmInfo* FwdLockEngine::onAcquireDrmInfo(int uniqueId, const DrmInfoRequest* drmInfoRequest) {
    DrmInfo* drmInfo = NULL;

    // Nothing to be done for Forward Lock file
<<<<<<< ours
    LOGV("FwdLockEngine::onAcquireDrmInfo");
||||||| base
    LOGD("FwdLockEngine::onAcquireDrmInfo");
=======
    LOG_VERBOSE("FwdLockEngine::onAcquireDrmInfo");
>>>>>>> theirs

    return drmInfo;
}

int FwdLockEngine::onCheckRightsStatus(int uniqueId,
                                       const String8& path,
                                       int action) {
    int result = RightsStatus::RIGHTS_INVALID;

<<<<<<< ours
    LOGV("FwdLockEngine::onCheckRightsStatus");
||||||| base
    LOGD("FwdLockEngine::onCheckRightsStatus");
=======
    LOG_VERBOSE("FwdLockEngine::onCheckRightsStatus");
>>>>>>> theirs

    // Only Transfer action is not allowed for forward Lock files.
    if (onCanHandle(uniqueId, path)) {
        switch(action) {
            case Action::DEFAULT:
            case Action::PLAY:
            case Action::RINGTONE:
            case Action::OUTPUT:
            case Action::PREVIEW:
            case Action::EXECUTE:
            case Action::DISPLAY:
                result = RightsStatus::RIGHTS_VALID;
                break;

            case Action::TRANSFER:
            default:
                result = RightsStatus::RIGHTS_INVALID;
                break;
        }
    }

    return result;
}

status_t FwdLockEngine::onConsumeRights(int uniqueId,
                                        DecryptHandle* decryptHandle,
                                        int action,
                                        bool reserve) {
    // No rights consumption
<<<<<<< ours
    LOGV("FwdLockEngine::onConsumeRights");
||||||| base
    LOGD("FwdLockEngine::onConsumeRights");
=======
    LOG_VERBOSE("FwdLockEngine::onConsumeRights");
>>>>>>> theirs
    return DRM_NO_ERROR;
}

bool FwdLockEngine::onValidateAction(int uniqueId,
                                     const String8& path,
                                     int action,
                                     const ActionDescription& description) {
<<<<<<< ours
    LOGV("FwdLockEngine::onValidateAction");
||||||| base
    LOGD("FwdLockEngine::onValidateAction");
=======
    LOG_VERBOSE("FwdLockEngine::onValidateAction");
>>>>>>> theirs

    // For the forwardlock engine checkRights and ValidateAction are the same.
    return (onCheckRightsStatus(uniqueId, path, action) == RightsStatus::RIGHTS_VALID);
}

String8 FwdLockEngine::onGetOriginalMimeType(int uniqueId, const String8& path) {
<<<<<<< ours
    LOGV("FwdLockEngine::onGetOriginalMimeType");
||||||| base
    LOGD("FwdLockEngine::onGetOriginalMimeType");
=======
    LOG_VERBOSE("FwdLockEngine::onGetOriginalMimeType");
>>>>>>> theirs
    String8 mimeString = String8("");
    int fileDesc = FwdLockFile_open(path.string());

    if (-1 < fileDesc) {
        const char* pMimeType = FwdLockFile_GetContentType(fileDesc);

        if (NULL != pMimeType) {
            String8 contentType = String8(pMimeType);
            contentType.toLower();
            mimeString = MimeTypeUtil::convertMimeType(contentType);
        }

        FwdLockFile_close(fileDesc);
    }

    return mimeString;
}

int FwdLockEngine::onGetDrmObjectType(int uniqueId,
                                      const String8& path,
                                      const String8& mimeType) {
    String8 mimeStr = String8(mimeType);

<<<<<<< ours
    LOGV("FwdLockEngine::onGetDrmObjectType");
||||||| base
    LOGD("FwdLockEngine::onGetDrmObjectType");
=======
    LOG_VERBOSE("FwdLockEngine::onGetDrmObjectType");
>>>>>>> theirs

    mimeStr.toLower();

    /* Checks whether
    * 1. path and mime type both are not empty strings (meaning unavailable) else content is unknown
    * 2. if one of them is empty string and if other is known then its a DRM Content Object.
    * 3. if both of them are available, then both may be of known type
    *    (regardless of the relation between them to make it compatible with other DRM Engines)
    */
    if (((0 == path.length()) || onCanHandle(uniqueId, path)) &&
        ((0 == mimeType.length()) || ((mimeStr == String8(FWDLOCK_MIMETYPE_FL)) ||
        (mimeStr == String8(FWDLOCK_MIMETYPE_DM)))) && (mimeType != path) ) {
            return DrmObjectType::CONTENT;
    }

    return DrmObjectType::UNKNOWN;
}

status_t FwdLockEngine::onRemoveRights(int uniqueId, const String8& path) {
    // No Rights to remove
<<<<<<< ours
    LOGV("FwdLockEngine::onRemoveRights");
||||||| base
    LOGD("FwdLockEngine::onRemoveRights");
=======
    LOG_VERBOSE("FwdLockEngine::onRemoveRights");
>>>>>>> theirs
    return DRM_NO_ERROR;
}

status_t FwdLockEngine::onRemoveAllRights(int uniqueId) {
    // No rights to remove
<<<<<<< ours
    LOGV("FwdLockEngine::onRemoveAllRights");
||||||| base
    LOGD("FwdLockEngine::onRemoveAllRights");
=======
    LOG_VERBOSE("FwdLockEngine::onRemoveAllRights");
>>>>>>> theirs
    return DRM_NO_ERROR;
}

#ifdef USE_64BIT_DRM_API
status_t FwdLockEngine::onSetPlaybackStatus(int uniqueId, DecryptHandle* decryptHandle,
                                            int playbackStatus, int64_t position) {
#else
status_t FwdLockEngine::onSetPlaybackStatus(int uniqueId, DecryptHandle* decryptHandle,
                                            int playbackStatus, int position) {
#endif
    // Not used
<<<<<<< ours
    LOGV("FwdLockEngine::onSetPlaybackStatus");
||||||| base
    LOGD("FwdLockEngine::onSetPlaybackStatus");
=======
    LOG_VERBOSE("FwdLockEngine::onSetPlaybackStatus");
>>>>>>> theirs
    return DRM_NO_ERROR;
}

status_t FwdLockEngine::onOpenConvertSession(int uniqueId,
                                         int convertId) {
    status_t result = DRM_ERROR_UNKNOWN;
<<<<<<< ours
    LOGV("FwdLockEngine::onOpenConvertSession");
||||||| base
    LOGD("FwdLockEngine::onOpenConvertSession");
=======
    LOG_VERBOSE("FwdLockEngine::onOpenConvertSession");
>>>>>>> theirs
    if (!convertSessionMap.isCreated(convertId)) {
        ConvertSession *newSession = new ConvertSession();
        if (FwdLockConv_Status_OK ==
            FwdLockConv_OpenSession(&(newSession->uniqueId), &(newSession->output))) {
            convertSessionMap.addValue(convertId, newSession);
            result = DRM_NO_ERROR;
        } else {
            LOGE("FwdLockEngine::onOpenConvertSession -- FwdLockConv_OpenSession failed.");
            delete newSession;
        }
    }
    return result;
}

DrmConvertedStatus* FwdLockEngine::onConvertData(int uniqueId,
                                                 int convertId,
                                                 const DrmBuffer* inputData) {
    FwdLockConv_Status_t retStatus = FwdLockConv_Status_InvalidArgument;
    DrmBuffer *convResult = new DrmBuffer(NULL, 0);
    int offset = -1;

    if (NULL != inputData && convertSessionMap.isCreated(convertId)) {
        ConvertSession *convSession = convertSessionMap.getValue(convertId);

        if (NULL != convSession) {
            retStatus = FwdLockConv_ConvertData(convSession->uniqueId,
                                                inputData->data,
                                                inputData->length,
                                                &(convSession->output));

            if (FwdLockConv_Status_OK == retStatus) {
                // return bytes from conversion if available
                if (convSession->output.fromConvertData.numBytes > 0) {
                    convResult->data = new char[convSession->output.fromConvertData.numBytes];

                    if (NULL != convResult->data) {
                        convResult->length = convSession->output.fromConvertData.numBytes;
                        memcpy(convResult->data,
                               (char *)convSession->output.fromConvertData.pBuffer,
                               convResult->length);
                    }
                }
            } else {
                offset = convSession->output.fromConvertData.errorPos;
            }
        }
    }
    return new DrmConvertedStatus(getConvertedStatus(retStatus), convResult, offset);
}

DrmConvertedStatus* FwdLockEngine::onCloseConvertSession(int uniqueId,
                                                         int convertId) {
    FwdLockConv_Status_t retStatus = FwdLockConv_Status_InvalidArgument;
    DrmBuffer *convResult = new DrmBuffer(NULL, 0);
    int offset = -1;

<<<<<<< ours
    LOGV("FwdLockEngine::onCloseConvertSession");
||||||| base
    LOGD("FwdLockEngine::onCloseConvertSession");
=======
    LOG_VERBOSE("FwdLockEngine::onCloseConvertSession");
>>>>>>> theirs

    if (convertSessionMap.isCreated(convertId)) {
        ConvertSession *convSession = convertSessionMap.getValue(convertId);

        if (NULL != convSession) {
            retStatus = FwdLockConv_CloseSession(convSession->uniqueId, &(convSession->output));

            if (FwdLockConv_Status_OK == retStatus) {
                offset = convSession->output.fromCloseSession.fileOffset;
                convResult->data = new char[FWD_LOCK_SIGNATURES_SIZE];

                if (NULL != convResult->data) {
                      convResult->length = FWD_LOCK_SIGNATURES_SIZE;
                      memcpy(convResult->data,
                             (char *)convSession->output.fromCloseSession.signatures,
                             convResult->length);
                }
            }
        }
        convertSessionMap.removeValue(convertId);
    }
    return new DrmConvertedStatus(getConvertedStatus(retStatus), convResult, offset);
}

#ifdef USE_64BIT_DRM_API
status_t FwdLockEngine::onOpenDecryptSession(int uniqueId,
                                             DecryptHandle* decryptHandle,
                                             int fd,
                                             off64_t offset,
                                             off64_t length) {
#else
status_t FwdLockEngine::onOpenDecryptSession(int uniqueId,
                                             DecryptHandle* decryptHandle,
                                             int fd,
                                             int offset,
                                             int length) {
#endif
    status_t result = DRM_ERROR_CANNOT_HANDLE;
    int fileDesc = -1;

<<<<<<< ours
    LOGV("FwdLockEngine::onOpenDecryptSession");
||||||| base
    LOGD("FwdLockEngine::onOpenDecryptSession");
=======
    LOG_VERBOSE("FwdLockEngine::onOpenDecryptSession");
>>>>>>> theirs

    if ((-1 < fd) &&
        (NULL != decryptHandle) &&
        (!decodeSessionMap.isCreated(decryptHandle->decryptId))) {
        fileDesc = dup(fd);
    } else {
        LOGE("FwdLockEngine::onOpenDecryptSession parameter error");
        return result;
    }

    if (-1 < fileDesc &&
        -1 < ::lseek(fileDesc, offset, SEEK_SET) &&
        -1 < FwdLockFile_attach(fileDesc)) {
        // check for file integrity. This must be done to protect the content mangling.
        int retVal = FwdLockFile_CheckHeaderIntegrity(fileDesc);
        DecodeSession* decodeSession = new DecodeSession(fileDesc);

        if (retVal && NULL != decodeSession) {
            decodeSessionMap.addValue(decryptHandle->decryptId, decodeSession);
            const char *pmime= FwdLockFile_GetContentType(fileDesc);
            String8 contentType = String8(pmime == NULL ? "" : pmime);
            contentType.toLower();
            decryptHandle->mimeType = MimeTypeUtil::convertMimeType(contentType);
            decryptHandle->decryptApiType = DecryptApiType::CONTAINER_BASED;
            decryptHandle->status = RightsStatus::RIGHTS_VALID;
            decryptHandle->decryptInfo = NULL;
            result = DRM_NO_ERROR;
        } else {
            LOG_VERBOSE("FwdLockEngine::onOpenDecryptSession Integrity Check failed for the fd");
            FwdLockFile_detach(fileDesc);
            delete decodeSession;
        }
    }

<<<<<<< ours
    if (DRM_NO_ERROR != result && -1 < fileDesc) {
        ::close(fileDesc);
    }

    LOGV("FwdLockEngine::onOpenDecryptSession Exit. result = %d", result);
||||||| base
    LOGD("FwdLockEngine::onOpenDecryptSession Exit. result = %d", result);
=======
    LOG_VERBOSE("FwdLockEngine::onOpenDecryptSession Exit. result = %d", result);
>>>>>>> theirs

    return result;
}

status_t FwdLockEngine::onOpenDecryptSession(int uniqueId,
                                             DecryptHandle* decryptHandle,
                                             const char* uri) {
    status_t result = DRM_ERROR_CANNOT_HANDLE;
    const char fileTag [] = "file://";

    if (NULL != decryptHandle && NULL != uri && strlen(uri) > sizeof(fileTag)) {
        String8 uriTag = String8(uri);
        uriTag.toLower();

        if (0 == strncmp(uriTag.string(), fileTag, sizeof(fileTag) - 1)) {
            const char *filePath = strchr(uri + sizeof(fileTag) - 1, '/');
            if (NULL != filePath && onCanHandle(uniqueId, String8(filePath))) {
                int fd = open(filePath, O_RDONLY);

                if (-1 < fd) {
                    // offset is always 0 and length is not used. so any positive size.
                    result = onOpenDecryptSession(uniqueId, decryptHandle, fd, 0, 1);

                    // fd is duplicated already if success. closing the file
                    close(fd);
                }
            }
        }
    }

    return result;
}

status_t FwdLockEngine::onCloseDecryptSession(int uniqueId,
                                              DecryptHandle* decryptHandle) {
    status_t result = DRM_ERROR_UNKNOWN;
<<<<<<< ours
    LOGV("FwdLockEngine::onCloseDecryptSession");
||||||| base
    LOGD("FwdLockEngine::onCloseDecryptSession");
=======
    LOG_VERBOSE("FwdLockEngine::onCloseDecryptSession");
>>>>>>> theirs

    if (NULL != decryptHandle && decodeSessionMap.isCreated(decryptHandle->decryptId)) {
        DecodeSession* session = decodeSessionMap.getValue(decryptHandle->decryptId);
        if (NULL != session && session->fileDesc > -1) {
            FwdLockFile_detach(session->fileDesc);
            ::close(session->fileDesc);
            decodeSessionMap.removeValue(decryptHandle->decryptId);
            result = DRM_NO_ERROR;
        }
    }

<<<<<<< ours
    if (NULL != decryptHandle) {
        if (NULL != decryptHandle->decryptInfo) {
            delete decryptHandle->decryptInfo;
            decryptHandle->decryptInfo = NULL;
        }

        decryptHandle->copyControlVector.clear();
        decryptHandle->extendedData.clear();

        delete decryptHandle;
        decryptHandle = NULL;
    }

    LOGV("FwdLockEngine::onCloseDecryptSession Exit");
||||||| base
    LOGD("FwdLockEngine::onCloseDecryptSession Exit");
=======
    LOG_VERBOSE("FwdLockEngine::onCloseDecryptSession Exit");
>>>>>>> theirs
    return result;
}

status_t FwdLockEngine::onInitializeDecryptUnit(int uniqueId,
                                                DecryptHandle* decryptHandle,
                                                int decryptUnitId,
                                                const DrmBuffer* headerInfo) {
<<<<<<< ours
    LOGV("FwdLockEngine::onInitializeDecryptUnit");
||||||| base
    LOGD("FwdLockEngine::onInitializeDecryptUnit");
=======
    LOGE("FwdLockEngine::onInitializeDecryptUnit is not supported for this DRM scheme");
>>>>>>> theirs
    return DRM_ERROR_UNKNOWN;
}

status_t FwdLockEngine::onDecrypt(int uniqueId, DecryptHandle* decryptHandle, int decryptUnitId,
            const DrmBuffer* encBuffer, DrmBuffer** decBuffer, DrmBuffer* IV) {
<<<<<<< ours
    LOGV("FwdLockEngine::onDecrypt");
||||||| base
    LOGD("FwdLockEngine::onDecrypt");
=======
    LOGE("FwdLockEngine::onDecrypt is not supported for this DRM scheme");
>>>>>>> theirs
    return DRM_ERROR_UNKNOWN;
}

status_t FwdLockEngine::onDecrypt(int uniqueId,
                                  DecryptHandle* decryptHandle,
                                  int decryptUnitId,
                                  const DrmBuffer* encBuffer,
                                  DrmBuffer** decBuffer) {
<<<<<<< ours
    LOGV("FwdLockEngine::onDecrypt");
||||||| base
    LOGD("FwdLockEngine::onDecrypt");
=======
    LOGE("FwdLockEngine::onDecrypt is not supported for this DRM scheme");
>>>>>>> theirs
    return DRM_ERROR_UNKNOWN;
}

status_t FwdLockEngine::onFinalizeDecryptUnit(int uniqueId,
                                              DecryptHandle* decryptHandle,
                                              int decryptUnitId) {
<<<<<<< ours
    LOGV("FwdLockEngine::onFinalizeDecryptUnit");
||||||| base
    LOGD("FwdLockEngine::onFinalizeDecryptUnit");
=======
    LOGE("FwdLockEngine::onFinalizeDecryptUnit is not supported for this DRM scheme");
>>>>>>> theirs
    return DRM_ERROR_UNKNOWN;
}

ssize_t FwdLockEngine::onRead(int uniqueId,
                              DecryptHandle* decryptHandle,
                              void* buffer,
                              int numBytes) {
    ssize_t size = -1;

    if (NULL != decryptHandle &&
       decodeSessionMap.isCreated(decryptHandle->decryptId) &&
        NULL != buffer &&
        numBytes > -1) {
        DecodeSession* session = decodeSessionMap.getValue(decryptHandle->decryptId);
        if (NULL != session && session->fileDesc > -1) {
            size = FwdLockFile_read(session->fileDesc, buffer, numBytes);

            if (0 > size) {
                session->offset = ((off_t)-1);
            } else {
                session->offset += size;
            }
        }
    }

    return size;
}

#ifdef USE_64BIT_DRM_API
off64_t FwdLockEngine::onLseek(int uniqueId, DecryptHandle* decryptHandle,
                               off64_t offset, int whence) {
#else
off_t FwdLockEngine::onLseek(int uniqueId, DecryptHandle* decryptHandle,
                             off_t offset, int whence) {
#endif
    off_t offval = -1;

    if (NULL != decryptHandle && decodeSessionMap.isCreated(decryptHandle->decryptId)) {
        DecodeSession* session = decodeSessionMap.getValue(decryptHandle->decryptId);
        if (NULL != session && session->fileDesc > -1) {
            offval = FwdLockFile_lseek(session->fileDesc, offset, whence);
            session->offset = offval;
        }
    }

    return offval;
}

#ifdef USE_64BIT_DRM_API
ssize_t FwdLockEngine::onPread(int uniqueId,
                               DecryptHandle* decryptHandle,
                               void* buffer,
                               ssize_t numBytes,
                               off64_t offset) {
#else
ssize_t FwdLockEngine::onPread(int uniqueId,
                               DecryptHandle* decryptHandle,
                               void* buffer,
                               ssize_t numBytes,
                               off_t offset) {
#endif
    ssize_t bytesRead = -1;

    DecodeSession* decoderSession = NULL;

    if ((NULL != decryptHandle) &&
        (NULL != (decoderSession = decodeSessionMap.getValue(decryptHandle->decryptId))) &&
        (NULL != buffer) &&
        (numBytes > -1) &&
        (offset > -1)) {
        if (offset != decoderSession->offset) {
            decoderSession->offset = onLseek(uniqueId, decryptHandle, offset, SEEK_SET);
        }

        if (((off_t)-1) != decoderSession->offset) {
            bytesRead = onRead(uniqueId, decryptHandle, buffer, numBytes);
            if (bytesRead < 0) {
                LOGE("FwdLockEngine::onPread error reading");
            }
        }
    } else {
        LOGE("FwdLockEngine::onPread decryptId not found");
    }

    return bytesRead;
}
