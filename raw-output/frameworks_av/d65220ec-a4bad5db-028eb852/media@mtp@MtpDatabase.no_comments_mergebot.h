#ifndef _MTP_DATABASE_H
#define _MTP_DATABASE_H 
#include "MtpTypes.h"
namespace android {
class MtpDataPacket;
class MtpProperty;
class MtpObjectInfo;
class MtpDatabase {
public:
    virtual ~MtpDatabase() {}
    virtual MtpObjectHandle beginSendObject(const char* path,
                                            MtpObjectFormat format,
                                            MtpObjectHandle parent,
                                            MtpStorageID storage,
                                            uint64_t size,
                                            time_t modified) = 0;
    virtual void endSendObject(const char* path,
                                            MtpObjectHandle handle,
                                            MtpObjectFormat format,
                                            bool succeeded) = 0;
    virtual MtpObjectHandleList* getObjectList(MtpStorageID storageID,
                                            MtpObjectFormat format,
                                            MtpObjectHandle parent) = 0;
    virtual int getNumObjects(MtpStorageID storageID,
                                            MtpObjectFormat format,
                                            MtpObjectHandle parent) = 0;
    virtual MtpObjectFormatList* getSupportedPlaybackFormats() = 0;
    virtual MtpObjectFormatList* getSupportedCaptureFormats() = 0;
    virtual MtpObjectPropertyList* getSupportedObjectProperties(MtpObjectFormat format) = 0;
    virtual MtpDevicePropertyList* getSupportedDeviceProperties() = 0;
    virtual MtpResponseCode getObjectPropertyValue(MtpObjectHandle handle,
                                            MtpObjectProperty property,
                                            MtpDataPacket& packet) = 0;
    virtual MtpResponseCode setObjectPropertyValue(MtpObjectHandle handle,
                                            MtpObjectProperty property,
                                            MtpDataPacket& packet) = 0;
    virtual MtpResponseCode getDevicePropertyValue(MtpDeviceProperty property,
                                            MtpDataPacket& packet) = 0;
    virtual MtpResponseCode setDevicePropertyValue(MtpDeviceProperty property,
                                            MtpDataPacket& packet) = 0;
    virtual MtpResponseCode resetDeviceProperty(MtpDeviceProperty property) = 0;
    virtual MtpResponseCode getObjectPropertyList(MtpObjectHandle handle,
                                            uint32_t format, uint32_t property,
                                            int groupCode, int depth,
                                            MtpDataPacket& packet) = 0;
    virtual MtpResponseCode getObjectInfo(MtpObjectHandle handle,
                                            MtpObjectInfo& info) = 0;
    virtual void* getThumbnail(MtpObjectHandle handle, size_t& outThumbSize) = 0;
    virtual MtpResponseCode getObjectFilePath(MtpObjectHandle handle,
                                            MtpString& outFilePath,
                                            int64_t& outFileLength,
                                            MtpObjectFormat& outFormat) = 0;
    virtual MtpResponseCode deleteFile(MtpObjectHandle handle) = 0;
    virtual MtpObjectHandleList* getObjectReferences(MtpObjectHandle handle) = 0;
    virtual MtpResponseCode setObjectReferences(MtpObjectHandle handle,
                                            MtpObjectHandleList* references) = 0;
    virtual MtpProperty* getObjectPropertyDesc(MtpObjectProperty property,
                                            MtpObjectFormat format) = 0;
    virtual MtpProperty* getDevicePropertyDesc(MtpDeviceProperty property) = 0;
    virtual void sessionStarted() = 0;
    virtual void sessionEnded() = 0;
};
}
#endif
