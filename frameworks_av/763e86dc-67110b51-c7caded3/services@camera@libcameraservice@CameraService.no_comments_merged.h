#ifndef ANDROID_SERVERS_CAMERA_CAMERASERVICE_H
#define ANDROID_SERVERS_CAMERA_CAMERASERVICE_H 
#include <android/hardware/BnCameraService.h>
#include <android/hardware/BnSensorPrivacyListener.h>
#include <android/hardware/ICameraServiceListener.h>
#include <android/hardware/ICameraServiceProxy.h>
#include <cutils/multiuser.h>
#include <utils/Vector.h>
#include <utils/KeyedVector.h>
#include <binder/ActivityManager.h>
#include <binder/AppOpsManager.h>
#include <binder/BinderService.h>
#include <binder/IAppOpsCallback.h>
#include <binder/IUidObserver.h>
#include <hardware/camera.h>
#include <sensorprivacy/SensorPrivacyManager.h>
#include <android/hardware/camera/common/1.0/types.h>
#include <camera/VendorTagDescriptor.h>
#include <camera/CaptureResult.h>
#include <camera/CameraParameters.h>
#include <camera/camera2/ConcurrentCamera.h>
#include "CameraFlashlight.h"
#include "common/CameraProviderManager.h"
#include "media/RingBuffer.h"
#include "utils/AutoConditionLock.h"
#include "utils/ClientManager.h"
#include <set>
#include <string>
#include <map>
#include <memory>
#include <utility>
#include <unordered_map>
#include <unordered_set>
namespace android {
extern volatile int32_t gLogLevel;
class MemoryHeapBase;
class MediaPlayer;
class CameraService :
    public BinderService<CameraService>,
    public virtual ::android::hardware::BnCameraService,
    public virtual IBinder::DeathRecipient,
    public virtual CameraProviderManager::StatusListener
{
    friend class BinderService<CameraService>;
    friend class CameraClient;
    friend class CameraOfflineSessionClient;
public:
    class Client;
    class BasicClient;
    class OfflineClient;
    enum apiLevel {
        API_1 = 1,
        API_2 = 2
    };
    static const nsecs_t DEFAULT_CONNECT_TIMEOUT_NS = 3000000000;
    static const nsecs_t DEFAULT_DISCONNECT_TIMEOUT_NS = 1000000000;
    static const size_t DEFAULT_EVENT_LOG_LENGTH = 100;
    static const int SN_EVENT_LOG_ID = 0x534e4554;
    static char const* getServiceName() { return "media.camera"; }
                        CameraService();
    virtual ~CameraService();
    virtual void onDeviceStatusChanged(const String8 &cameraId,
            hardware::camera::common::V1_0::CameraDeviceStatus newHalStatus) override;
    virtual void onDeviceStatusChanged(const String8 &cameraId,
            const String8 &physicalCameraId,
            hardware::camera::common::V1_0::CameraDeviceStatus newHalStatus) override;
    virtual void onTorchStatusChanged(const String8& cameraId,
            hardware::camera::common::V1_0::TorchModeStatus newStatus) override;
    virtual void onNewProviderRegistered() override;
    virtual binder::Status getNumberOfCameras(int32_t type, int32_t* numCameras);
    virtual binder::Status getCameraInfo(int cameraId,
            hardware::CameraInfo* cameraInfo);
    virtual binder::Status getCameraCharacteristics(const String16& cameraId,
            CameraMetadata* cameraInfo);
    virtual binder::Status getCameraVendorTagDescriptor(
            hardware::camera2::params::VendorTagDescriptor* desc);
    virtual binder::Status getCameraVendorTagCache(
            hardware::camera2::params::VendorTagDescriptorCache* cache);
    virtual binder::Status connect(const sp<hardware::ICameraClient>& cameraClient,
            int32_t cameraId, const String16& clientPackageName,
            int32_t clientUid, int clientPid,
            sp<hardware::ICamera>* device);
    virtual binder::Status connectLegacy(const sp<hardware::ICameraClient>& cameraClient,
            int32_t cameraId, int32_t halVersion,
            const String16& clientPackageName, int32_t clientUid,
            sp<hardware::ICamera>* device);
    virtual binder::Status connectDevice(
            const sp<hardware::camera2::ICameraDeviceCallbacks>& cameraCb, const String16& cameraId,
            const String16& clientPackageName, const std::unique_ptr<String16>& clientFeatureId,
            int32_t clientUid,
            sp<hardware::camera2::ICameraDeviceUser>* device);
    virtual binder::Status addListener(const sp<hardware::ICameraServiceListener>& listener,
            std::vector<hardware::CameraStatus>* cameraStatuses);
    virtual binder::Status removeListener(
            const sp<hardware::ICameraServiceListener>& listener);
    virtual binder::Status getConcurrentStreamingCameraIds(
        std::vector<hardware::camera2::utils::ConcurrentCameraIdCombination>* concurrentCameraIds);
    virtual binder::Status isConcurrentSessionConfigurationSupported(
        const std::vector<hardware::camera2::utils::CameraIdAndSessionConfiguration>& sessions,
               bool* supported);
    virtual binder::Status getLegacyParameters(
            int32_t cameraId,
            String16* parameters);
    virtual binder::Status setTorchMode(const String16& cameraId, bool enabled,
            const sp<IBinder>& clientBinder);
    virtual binder::Status notifySystemEvent(int32_t eventId,
            const std::vector<int32_t>& args);
    virtual binder::Status notifyDeviceStateChange(int64_t newState);
    virtual binder::Status supportsCameraApi(
            const String16& cameraId, int32_t apiVersion,
            bool *isSupported);
    virtual binder::Status isHiddenPhysicalCamera(
            const String16& cameraId,
            bool *isSupported);
    virtual status_t onTransact(uint32_t code, const Parcel& data,
                                   Parcel* reply, uint32_t flags);
    virtual status_t dump(int fd, const Vector<String16>& args);
    virtual status_t shellCommand(int in, int out, int err, const Vector<String16>& args);
    binder::Status addListenerHelper(const sp<hardware::ICameraServiceListener>& listener,
            std::vector<hardware::CameraStatus>* cameraStatuses, bool isVendor = false);
    void notifyMonitoredUids();
    status_t addOfflineClient(String8 cameraId, sp<BasicClient> offlineClient);
    enum sound_kind {
        SOUND_SHUTTER = 0,
        SOUND_RECORDING_START = 1,
        SOUND_RECORDING_STOP = 2,
        NUM_SOUNDS
    };
    void playSound(sound_kind kind);
    void loadSoundLocked(sound_kind kind);
    void decreaseSoundRef();
    void increaseSoundRef();
    static void updateProxyDeviceState(
            int newState,
            const String8& cameraId,
            int facing,
            const String16& clientName,
            int apiLevel);
    int getDeviceVersion(const String8& cameraId, int* facing = NULL);
    static binder::Status filterGetInfoErrorCode(status_t err);
    class BasicClient : public virtual RefBase {
    public:
        virtual status_t initialize(sp<CameraProviderManager> manager,
                const String8& monitorTags) = 0;
        virtual binder::Status disconnect();
        virtual sp<IBinder> asBinderWrapper() = 0;
        sp<IBinder> getRemote() {
            return mRemoteBinder;
        }
        virtual status_t dump(int fd, const Vector<String16>& args);
        virtual status_t dumpClient(int fd, const Vector<String16>& args) = 0;
        virtual String16 getPackageName() const;
        virtual void notifyError(int32_t errorCode,
                const CaptureResultExtras& resultExtras) = 0;
        virtual uid_t getClientUid() const;
        virtual int getClientPid() const;
        virtual bool canCastToApiClient(apiLevel level) const;
        virtual void block();
        virtual status_t setAudioRestriction(int32_t mode);
        virtual int32_t getServiceAudioRestriction() const;
        virtual int32_t getAudioRestriction() const;
        static bool isValidAudioRestriction(int32_t mode);
        virtual status_t setRotateAndCropOverride(uint8_t rotateAndCrop) = 0;
    protected:
        BasicClient(const sp<CameraService>& cameraService,
                const sp<IBinder>& remoteCallback,
                const String16& clientPackageName,
                const std::unique_ptr<String16>& clientFeatureId,
                const String8& cameraIdStr,
                int cameraFacing,
                int clientPid,
                uid_t clientUid,
                int servicePid);
        virtual ~BasicClient();
        bool mDestructionStarted;
        static sp<CameraService> sCameraService;
        const String8 mCameraIdStr;
        const int mCameraFacing;
        String16 mClientPackageName;
        std::unique_ptr<String16> mClientFeatureId;
        pid_t mClientPid;
        const uid_t mClientUid;
        const pid_t mServicePid;
        bool mDisconnected;
        mutable Mutex mAudioRestrictionLock;
        int32_t mAudioRestriction;
        sp<IBinder> mRemoteBinder;
        virtual status_t startCameraOps();
        virtual status_t finishCameraOps();
        std::unique_ptr<AppOpsManager> mAppOpsManager = nullptr;
        class OpsCallback : public BnAppOpsCallback {
        public:
            explicit OpsCallback(wp<BasicClient> client);
            virtual void opChanged(int32_t op, const String16& packageName);
        private:
            wp<BasicClient> mClient;
        };
        sp<OpsCallback> mOpsCallback;
        bool mOpsActive;
        virtual void opChanged(int32_t op, const String16& packageName);
    };
    class Client : public hardware::BnCamera, public BasicClient
    {
    public:
        typedef hardware::ICameraClient TCamCallbacks;
        virtual binder::Status disconnect();
        virtual status_t connect(const sp<hardware::ICameraClient>& client) = 0;
        virtual status_t lock() = 0;
        virtual status_t unlock() = 0;
        virtual status_t setPreviewTarget(const sp<IGraphicBufferProducer>& bufferProducer)=0;
        virtual void setPreviewCallbackFlag(int flag) = 0;
        virtual status_t setPreviewCallbackTarget(
                const sp<IGraphicBufferProducer>& callbackProducer) = 0;
        virtual status_t startPreview() = 0;
        virtual void stopPreview() = 0;
        virtual bool previewEnabled() = 0;
        virtual status_t setVideoBufferMode(int32_t videoBufferMode) = 0;
        virtual status_t startRecording() = 0;
        virtual void stopRecording() = 0;
        virtual bool recordingEnabled() = 0;
        virtual void releaseRecordingFrame(const sp<IMemory>& mem) = 0;
        virtual status_t autoFocus() = 0;
        virtual status_t cancelAutoFocus() = 0;
        virtual status_t takePicture(int msgType) = 0;
        virtual status_t setParameters(const String8& params) = 0;
        virtual String8 getParameters() const = 0;
        virtual status_t sendCommand(int32_t cmd, int32_t arg1, int32_t arg2) = 0;
        virtual status_t setVideoTarget(const sp<IGraphicBufferProducer>& bufferProducer) = 0;
        Client(const sp<CameraService>& cameraService,
                const sp<hardware::ICameraClient>& cameraClient,
                const String16& clientPackageName,
                const std::unique_ptr<String16>& clientFeatureId,
                const String8& cameraIdStr,
                int api1CameraId,
                int cameraFacing,
                int clientPid,
                uid_t clientUid,
                int servicePid);
        ~Client();
        const sp<hardware::ICameraClient>& getRemoteCallback() {
            return mRemoteCallback;
        }
        virtual sp<IBinder> asBinderWrapper() {
            return asBinder(this);
        }
        virtual void notifyError(int32_t errorCode,
                                         const CaptureResultExtras& resultExtras);
        virtual bool canCastToApiClient(apiLevel level) const;
    protected:
        sp<hardware::ICameraClient> mRemoteCallback;
        int mCameraId;
    };
    class ClientEventListener {
    public:
        void onClientAdded(const resource_policy::ClientDescriptor<String8,
                sp<CameraService::BasicClient>>& descriptor);
        void onClientRemoved(const resource_policy::ClientDescriptor<String8,
                sp<CameraService::BasicClient>>& descriptor);
    };
    typedef std::shared_ptr<resource_policy::ClientDescriptor<String8,
            sp<CameraService::BasicClient>>> DescriptorPtr;
    class CameraClientManager : public resource_policy::ClientManager<String8,
            sp<CameraService::BasicClient>, ClientEventListener> {
    public:
        CameraClientManager();
        virtual ~CameraClientManager();
        sp<CameraService::BasicClient> getCameraClient(const String8& id) const;
        String8 toString() const;
        static DescriptorPtr makeClientDescriptor(const String8& key, const sp<BasicClient>& value,
                int32_t cost, const std::set<String8>& conflictingKeys, int32_t score,
                int32_t ownerId, int32_t state);
        static DescriptorPtr makeClientDescriptor(const sp<BasicClient>& value,
                const CameraService::DescriptorPtr& partial);
    };
    int32_t updateAudioRestriction();
    int32_t updateAudioRestrictionLocked();
private:
    typedef hardware::camera::common::V1_0::CameraDeviceStatus CameraDeviceStatus;
    enum class StatusInternal : int32_t {
        NOT_PRESENT = static_cast<int32_t>(CameraDeviceStatus::NOT_PRESENT),
        PRESENT = static_cast<int32_t>(CameraDeviceStatus::PRESENT),
        ENUMERATING = static_cast<int32_t>(CameraDeviceStatus::ENUMERATING),
        NOT_AVAILABLE = static_cast<int32_t>(hardware::ICameraServiceListener::STATUS_NOT_AVAILABLE),
        UNKNOWN = static_cast<int32_t>(hardware::ICameraServiceListener::STATUS_UNKNOWN)
    };
    class CameraState {
    public:
        CameraState(const String8& id, int cost, const std::set<String8>& conflicting,
                SystemCameraKind deviceKind);
        virtual ~CameraState();
        StatusInternal getStatus() const;
        template<class Func>
        void updateStatus(StatusInternal status,
                const String8& cameraId,
                std::initializer_list<StatusInternal> rejectSourceStates,
                Func onStatusUpdatedLocked);
        CameraParameters getShimParams() const;
        void setShimParams(const CameraParameters& params);
        int getCost() const;
        std::set<String8> getConflicting() const;
        String8 getId() const;
        SystemCameraKind getSystemCameraKind() const;
        bool addUnavailablePhysicalId(const String8& physicalId);
        bool removeUnavailablePhysicalId(const String8& physicalId);
        std::vector<String8> getUnavailablePhysicalIds() const;
    private:
        const String8 mId;
        StatusInternal mStatus;
        const int mCost;
        std::set<String8> mConflicting;
        std::set<String8> mUnavailablePhysicalIds;
        mutable Mutex mStatusLock;
        CameraParameters mShimParams;
        const SystemCameraKind mSystemCameraKind;
    };
    class UidPolicy : public BnUidObserver, public virtual IBinder::DeathRecipient {
    public:
        explicit UidPolicy(sp<CameraService> service)
                : mRegistered(false), mService(service) {}
        void registerSelf();
        void unregisterSelf();
        bool isUidActive(uid_t uid, String16 callingPackage);
        int32_t getProcState(uid_t uid);
        void onUidGone(uid_t uid, bool disabled);
        void onUidActive(uid_t uid);
        void onUidIdle(uid_t uid, bool disabled);
        void onUidStateChanged(uid_t uid, int32_t procState, int64_t procStateSeq,
                int32_t capability);
        void addOverrideUid(uid_t uid, String16 callingPackage, bool active);
        void removeOverrideUid(uid_t uid, String16 callingPackage);
        void registerMonitorUid(uid_t uid);
        void unregisterMonitorUid(uid_t uid);
        virtual void binderDied(const wp<IBinder> &who);
    private:
        bool isUidActiveLocked(uid_t uid, String16 callingPackage);
        int32_t getProcStateLocked(uid_t uid);
        void updateOverrideUid(uid_t uid, String16 callingPackage, bool active, bool insert);
        Mutex mUidLock;
        bool mRegistered;
        ActivityManager mAm;
        wp<CameraService> mService;
        std::unordered_set<uid_t> mActiveUids;
        std::unordered_map<uid_t, std::pair<int32_t, size_t>> mMonitoredUids;
        std::unordered_map<uid_t, bool> mOverrideUids;
    };
    class SensorPrivacyPolicy : public hardware::BnSensorPrivacyListener,
            public virtual IBinder::DeathRecipient {
        public:
            explicit SensorPrivacyPolicy(wp<CameraService> service)
                    : mService(service), mSensorPrivacyEnabled(false), mRegistered(false) {}
            void registerSelf();
            void unregisterSelf();
            bool isSensorPrivacyEnabled();
            binder::Status onSensorPrivacyChanged(bool enabled);
            virtual void binderDied(const wp<IBinder> &who);
        private:
            SensorPrivacyManager mSpm;
            wp<CameraService> mService;
            Mutex mSensorPrivacyLock;
            bool mSensorPrivacyEnabled;
            bool mRegistered;
    };
    sp<UidPolicy> mUidPolicy;
    sp<SensorPrivacyPolicy> mSensorPrivacyPolicy;
    virtual void onFirstRef();
    status_t enumerateProviders();
    void addStates(const String8 id);
    void removeStates(const String8 id);
    binder::Status validateConnectLocked(const String8& cameraId, const String8& clientName8,
                     int& clientUid, int& clientPid, int& originalClientPid) const;
    binder::Status validateClientPermissionsLocked(const String8& cameraId, const String8& clientName8,
                     int& clientUid, int& clientPid, int& originalClientPid) const;
    status_t handleEvictionsLocked(const String8& cameraId, int clientPid,
        apiLevel effectiveApiLevel, const sp<IBinder>& remoteCallback, const String8& packageName,
        sp<BasicClient>* client,
        std::shared_ptr<resource_policy::ClientDescriptor<String8, sp<BasicClient>>>* partial);
    bool shouldRejectSystemCameraConnection(const String8 & cameraId) const;
    static bool shouldSkipStatusUpdates(SystemCameraKind systemCameraKind, bool isVendorListener,
            int clientPid, int clientUid);
    status_t getSystemCameraKind(const String8& cameraId, SystemCameraKind *kind) const;
    void filterAPI1SystemCameraLocked(const std::vector<std::string> &normalDeviceIds);
    template<class CALLBACK, class CLIENT>
    binder::Status connectHelper(const sp<CALLBACK>& cameraCb, const String8& cameraId,
            int api1CameraId, int halVersion, const String16& clientPackageName,
            const std::unique_ptr<String16>& clientFeatureId, int clientUid, int clientPid,
            apiLevel effectiveApiLevel, bool shimUpdateOnly, sp<CLIENT>& device);
    Mutex mServiceLock;
    std::shared_ptr<WaitableMutexWrapper> mServiceLockWrapper;
    status_t checkIfDeviceIsUsable(const String8& cameraId) const;
    CameraClientManager mActiveClientManager;
    std::map<String8, std::shared_ptr<CameraState>> mCameraStates;
    mutable Mutex mCameraStatesLock;
    RingBuffer<String8> mEventLog;
    Mutex mLogLock;
    String8 mMonitorTags;
    std::set<userid_t> mAllowedUsers;
    std::shared_ptr<CameraService::CameraState> getCameraState(const String8& cameraId) const;
    bool evictClientIdByRemote(const wp<IBinder>& cameraClient);
    void removeByClient(const BasicClient* client);
    void finishConnectLocked(const sp<BasicClient>& client, const DescriptorPtr& desc);
    String8 cameraIdIntToStr(int cameraIdInt);
    std::string cameraIdIntToStrLocked(int cameraIdInt);
    sp<CameraService::BasicClient> removeClientLocked(const String8& cameraId);
    void doUserSwitch(const std::vector<int32_t>& newUserIds);
    void logEvent(const char* event);
    void logDisconnected(const char* cameraId, int clientPid, const char* clientPackage);
    void logDisconnectedOffline(const char* cameraId, int clientPid, const char* clientPackage);
    void logConnectedOffline(const char* cameraId, int clientPid,
            const char* clientPackage);
    void logConnected(const char* cameraId, int clientPid, const char* clientPackage);
    void logRejected(const char* cameraId, int clientPid, const char* clientPackage,
            const char* reason);
    void logTorchEvent(const char* cameraId, const char *torchState, int clientPid);
    void logUserSwitch(const std::set<userid_t>& oldUserIds,
        const std::set<userid_t>& newUserIds);
    void logDeviceRemoved(const char* cameraId, const char* reason);
    void logDeviceAdded(const char* cameraId, const char* reason);
    void logClientDied(int clientPid, const char* reason);
    void logServiceError(const char* msg, int errorCode);
    void dumpEventLog(int fd);
    void updateCameraNumAndIds();
    int mNumberOfCameras;
    int mNumberOfCamerasWithoutSystemCamera;
    std::vector<std::string> mNormalDeviceIds;
    std::vector<std::string> mNormalDeviceIdsWithoutSystemCamera;
    sp<MediaPlayer> newMediaPlayer(const char *file);
    Mutex mSoundLock;
    sp<MediaPlayer> mSoundPlayer[NUM_SOUNDS];
    int mSoundRef;
    bool mInitialized;
    sp<CameraProviderManager> mCameraProviderManager;
    class ServiceListener : public virtual IBinder::DeathRecipient {
        public:
            ServiceListener(sp<CameraService> parent, sp<hardware::ICameraServiceListener> listener,
                    int uid, int pid, bool isVendorClient, bool openCloseCallbackAllowed)
                    : mParent(parent), mListener(listener), mListenerUid(uid), mListenerPid(pid),
                      mIsVendorListener(isVendorClient),
                      mOpenCloseCallbackAllowed(openCloseCallbackAllowed) { }
            status_t initialize() {
                return IInterface::asBinder(mListener)->linkToDeath(this);
            }
            virtual void binderDied(const wp<IBinder> & ) {
                auto parent = mParent.promote();
                if (parent.get() != nullptr) {
                    parent->removeListener(mListener);
                }
            }
            int getListenerUid() { return mListenerUid; }
            int getListenerPid() { return mListenerPid; }
            sp<hardware::ICameraServiceListener> getListener() { return mListener; }
            bool isVendorListener() { return mIsVendorListener; }
            bool isOpenCloseCallbackAllowed() { return mOpenCloseCallbackAllowed; }
        private:
            wp<CameraService> mParent;
            sp<hardware::ICameraServiceListener> mListener;
            int mListenerUid = -1;
            int mListenerPid = -1;
            bool mIsVendorListener = false;
            bool mOpenCloseCallbackAllowed = false;
    };
    std::vector<sp<ServiceListener>> mListenerList;
    Mutex mStatusListenerLock;
    void updateStatus(StatusInternal status,
            const String8& cameraId,
            std::initializer_list<StatusInternal>
                rejectedSourceStates);
    void updateStatus(StatusInternal status,
            const String8& cameraId);
    void updateOpenCloseStatus(const String8& cameraId, bool open, const String16& packageName);
    sp<CameraFlashlight> mFlashlight;
    Mutex mTorchStatusMutex;
    Mutex mTorchClientMapMutex;
    Mutex mTorchUidMapMutex;
    KeyedVector<String8, hardware::camera::common::V1_0::TorchModeStatus>
            mTorchStatusMap;
    KeyedVector<String8, sp<IBinder>> mTorchClientMap;
    std::map<String8, std::pair<int, int>> mTorchUidMap;
    void handleTorchClientBinderDied(const wp<IBinder> &who);
    void onTorchStatusChangedLocked(const String8& cameraId,
            hardware::camera::common::V1_0::TorchModeStatus newStatus);
    status_t getTorchStatusLocked(const String8 &cameraId,
             hardware::camera::common::V1_0::TorchModeStatus *status) const;
    status_t setTorchStatusLocked(const String8 &cameraId,
            hardware::camera::common::V1_0::TorchModeStatus status);
    void notifyPhysicalCameraStatusLocked(int32_t status, const String8& cameraId);
    virtual void binderDied(const wp<IBinder> &who);
    binder::Status initializeShimMetadata(int cameraId);
    binder::Status getLegacyParametersLazy(int cameraId, CameraParameters* parameters);
    void blockClientsForUid(uid_t uid);
    void blockAllClients();
    status_t handleSetUidState(const Vector<String16>& args, int err);
    status_t handleResetUidState(const Vector<String16>& args, int err);
    status_t handleGetUidState(const Vector<String16>& args, int out, int err);
    status_t handleSetRotateAndCrop(const Vector<String16>& args);
    status_t handleGetRotateAndCrop(int out);
    status_t printHelp(int out);
    static String8 getFormattedCurrentTime();
    static binder::Status makeClient(const sp<CameraService>& cameraService,
            const sp<IInterface>& cameraCb, const String16& packageName,
            const std::unique_ptr<String16>& featureId, const String8& cameraId, int api1CameraId,
            int facing, int clientPid, uid_t clientUid, int servicePid, int halVersion,
            int deviceVersion, apiLevel effectiveApiLevel,
                   sp<BasicClient>* client);
    status_t checkCameraAccess(const String16& opPackageName);
    static String8 toString(std::set<userid_t> intSet);
    static int32_t mapToInterface(hardware::camera::common::V1_0::TorchModeStatus status);
    static StatusInternal mapToInternal(hardware::camera::common::V1_0::CameraDeviceStatus status);
    static int32_t mapToInterface(StatusInternal status);
    static Mutex sProxyMutex;
    static sp<hardware::ICameraServiceProxy> sCameraServiceProxy;
    static sp<hardware::ICameraServiceProxy> getCameraServiceProxy();
    static void pingCameraServiceProxy();
    void broadcastTorchModeStatus(const String8& cameraId,
            hardware::camera::common::V1_0::TorchModeStatus status);
    void disconnectClient(const String8& id, sp<BasicClient> clientToDisconnect);
    static const String8 kOfflineDevice;
    AppOpsManager mAppOps;
    int32_t mAudioRestriction;
    uint8_t mOverrideRotateAndCropMode = ANDROID_SCALER_ROTATE_AND_CROP_AUTO;
};
}
#endif
