#ifndef _MTP_SERVER_H
#define _MTP_SERVER_H 
#include "MtpRequestPacket.h"
#include "MtpDataPacket.h"
#include "MtpResponsePacket.h"
#include "MtpEventPacket.h"
#include "mtp.h"
#include "MtpUtils.h"
#include <utils/threads.h>
namespace android {
class MtpDatabase;
class MtpStorage;
class MtpServer {
private:
    int mFD;
    MtpDatabase* mDatabase;
    int mFileGroup;
    int mFilePermission;
    int mDirectoryPermission;
    MtpSessionID mSessionID;
    bool mSessionOpen;
    MtpRequestPacket mRequest;
    MtpDataPacket mData;
    MtpResponsePacket mResponse;
    MtpEventPacket mEvent;
    MtpStorageList mStorages;
    MtpObjectHandle mSendObjectHandle;
    MtpObjectFormat mSendObjectFormat;
    MtpString mSendObjectFilePath;
    size_t mSendObjectFileSize;
    Mutex mMutex;
    class ObjectEdit {
        public:
        MtpObjectHandle mHandle;
        MtpString mPath;
        uint64_t mSize;
        MtpObjectFormat mFormat;
        int mFD;
        ObjectEdit(MtpObjectHandle handle, const char* path, uint64_t size,
            MtpObjectFormat format, int fd)
                : mHandle(handle), mPath(path), mSize(size), mFormat(format), mFD(fd) {
            }
        virtual ~ObjectEdit() {
            close(mFD);
        }
    };
    Vector<ObjectEdit*> mObjectEditList;
public:
                        MtpServer(int fd, MtpDatabase* database,
                                    int fileGroup, int filePerm, int directoryPerm);
    virtual ~MtpServer();
    MtpStorage* getStorage(MtpStorageID id);
    inline bool hasStorage() { return mStorages.size() > 0; }
    bool hasStorage(MtpStorageID id);
    void addStorage(MtpStorage* storage);
    void removeStorage(MtpStorage* storage);
    void run();
    void sendObjectAdded(MtpObjectHandle handle);
    void sendObjectRemoved(MtpObjectHandle handle);
private:
    void sendStoreAdded(MtpStorageID id);
    void sendStoreRemoved(MtpStorageID id);
    void sendEvent(MtpEventCode code, uint32_t param1);
    void addEditObject(MtpObjectHandle handle, MtpString& path,
                                uint64_t size, MtpObjectFormat format, int fd);
    ObjectEdit* getEditObject(MtpObjectHandle handle);
    void removeEditObject(MtpObjectHandle handle);
    void commitEdit(ObjectEdit* edit);
    bool handleRequest();
    MtpResponseCode doGetDeviceInfo();
    MtpResponseCode doOpenSession();
    MtpResponseCode doCloseSession();
    MtpResponseCode doGetStorageIDs();
    MtpResponseCode doGetStorageInfo();
    MtpResponseCode doGetObjectPropsSupported();
    MtpResponseCode doGetObjectHandles();
    MtpResponseCode doGetNumObjects();
    MtpResponseCode doGetObjectReferences();
    MtpResponseCode doSetObjectReferences();
    MtpResponseCode doGetObjectPropValue();
    MtpResponseCode doSetObjectPropValue();
    MtpResponseCode doGetDevicePropValue();
    MtpResponseCode doSetDevicePropValue();
    MtpResponseCode doResetDevicePropValue();
    MtpResponseCode doGetObjectPropList();
    MtpResponseCode doGetObjectInfo();
    MtpResponseCode doGetObject();
    MtpResponseCode doGetThumb();
    MtpResponseCode doGetPartialObject(MtpOperationCode operation);
    MtpResponseCode doSendObjectInfo();
    MtpResponseCode doSendObject();
    MtpResponseCode doDeleteObject();
    MtpResponseCode doGetObjectPropDesc();
    MtpResponseCode doGetDevicePropDesc();
    MtpResponseCode doSendPartialObject();
    MtpResponseCode doTruncateObject();
    MtpResponseCode doBeginEditObject();
    MtpResponseCode doEndEditObject();
};
};
#endif
