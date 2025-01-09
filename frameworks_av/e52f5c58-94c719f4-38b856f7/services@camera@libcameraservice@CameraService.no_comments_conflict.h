#ifndef ANDROID_SERVERS_CAMERA_CAMERASERVICE_H
#define ANDROID_SERVERS_CAMERA_CAMERASERVICE_H 
#include <android/content/AttributionSourceState.h>
#include <android/hardware/BnCameraService.h>
#include <android/hardware/BnSensorPrivacyListener.h>
#include <android/hardware/ICameraServiceListener.h>
#include <android/hardware/CameraIdRemapping.h>
#include <android/hardware/camera2/BnCameraInjectionSession.h>
#include <android/hardware/camera2/ICameraInjectionCallback.h>
#include <cutils/multiuser.h>
#include <utils/Vector.h>
#include <utils/KeyedVector.h>
#include <binder/ActivityManager.h>
#include <binder/AppOpsManager.h>
#include <binder/BinderService.h>
#include <binder/IServiceManager.h>
#include <binder/IActivityManager.h>
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
#include "utils/IPCTransport.h"
#include "utils/CameraServiceProxyWrapper.h"
#include <set>
#include <string>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace android {
extern volatile int32_t gLogLevel;
class MemoryHeapBase;
class MediaPlayer;
class CameraService :
    public BinderService<CameraService>,
    public virtual ::android::hardware::BnCameraService,
    public virtual IBinder::DeathRecipient,
    public virtual CameraProviderManager::StatusListener,
    public virtual IServiceManager::LocalRegistrationCallback,
    public AttributionAndPermissionUtilsEncapsulator
{
    friend class BinderService<CameraService>;
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
    static const userid_t USER_SYSTEM = 0;
    static void instantiate();
    static char const* getServiceName() { return "media.camera"; }
    virtual void onServiceRegistration(const String16& name, const sp<IBinder>& binder) override;
                        CameraService(std::shared_ptr<CameraServiceProxyWrapper>
                                cameraServiceProxyWrapper = nullptr);
    virtual ~CameraService();
    virtual void onDeviceStatusChanged(const std::string &cameraId,
            CameraDeviceStatus newHalStatus) override;
    virtual void onDeviceStatusChanged(const std::string &cameraId,
            const std::string &physicalCameraId,
            CameraDeviceStatus newHalStatus) override;
    virtual void onTorchStatusChanged(const std::string& cameraId,
            TorchModeStatus newStatus) override;
    virtual void onTorchStatusChanged(const std::string& cameraId,
            TorchModeStatus newStatus,
            SystemCameraKind kind) override;
    virtual void onNewProviderRegistered() override;
    virtual binder::Status getNumberOfCameras(int32_t type, int32_t* numCameras);
    virtual binder::Status getCameraInfo(int cameraId, bool overrideToPortrait,
            hardware::CameraInfo* cameraInfo) override;
    virtual binder::Status getCameraCharacteristics(const std::string& cameraId,
            int targetSdkVersion, bool overrideToPortrait, CameraMetadata* cameraInfo) override;
    virtual binder::Status getCameraVendorTagDescriptor(
            hardware::camera2::params::VendorTagDescriptor* desc);
    virtual binder::Status getCameraVendorTagCache(
            hardware::camera2::params::VendorTagDescriptorCache* cache);
    virtual binder::Status connect(const sp<hardware::ICameraClient>& cameraClient,
            int32_t cameraId, const std::string& clientPackageName,
            int32_t clientUid, int clientPid, int targetSdkVersion,
            bool overrideToPortrait, bool forceSlowJpegMode,
            sp<hardware::ICamera>* device) override;
    virtual binder::Status connectDevice(
            const sp<hardware::camera2::ICameraDeviceCallbacks>& cameraCb,
            const std::string& cameraId,
            const std::string& clientPackageName, const std::optional<std::string>& clientFeatureId,
            int32_t clientUid, int scoreOffset, int targetSdkVersion, bool overrideToPortrait,
            sp<hardware::camera2::ICameraDeviceUser>* device);
    virtual binder::Status addListener(const sp<hardware::ICameraServiceListener>& listener,
            std::vector<hardware::CameraStatus>* cameraStatuses);
    virtual binder::Status removeListener(
            const sp<hardware::ICameraServiceListener>& listener);
    virtual binder::Status getConcurrentCameraIds(
        std::vector<hardware::camera2::utils::ConcurrentCameraIdCombination>* concurrentCameraIds);
    virtual binder::Status isConcurrentSessionConfigurationSupported(
        const std::vector<hardware::camera2::utils::CameraIdAndSessionConfiguration>& sessions,
        int targetSdkVersion, bool* supported);
    virtual binder::Status getLegacyParameters(
            int32_t cameraId,
            std::string* parameters);
    virtual binder::Status setTorchMode(const std::string& cameraId, bool enabled,
            const sp<IBinder>& clientBinder);
    virtual binder::Status turnOnTorchWithStrengthLevel(const std::string& cameraId,
            int32_t torchStrength, const sp<IBinder>& clientBinder);
    virtual binder::Status getTorchStrengthLevel(const std::string& cameraId,
            int32_t* torchStrength);
    virtual binder::Status notifySystemEvent(int32_t eventId,
            const std::vector<int32_t>& args);
    virtual binder::Status notifyDeviceStateChange(int64_t newState);
    virtual binder::Status notifyDisplayConfigurationChange();
    virtual binder::Status supportsCameraApi(
            const std::string& cameraId, int32_t apiVersion,
            bool *isSupported);
    virtual binder::Status isHiddenPhysicalCamera(
            const std::string& cameraId,
            bool *isSupported);
    virtual binder::Status injectCamera(
            const std::string& packageName, const std::string& internalCamId,
            const std::string& externalCamId,
            const sp<hardware::camera2::ICameraInjectionCallback>& callback,
            sp<hardware::camera2::ICameraInjectionSession>* cameraInjectionSession);
    virtual binder::Status reportExtensionSessionStats(
            const hardware::CameraExtensionSessionStats& stats, std::string* sessionKey );
    virtual binder::Status remapCameraIds(const hardware::CameraIdRemapping&
            cameraIdRemapping);
    virtual binder::Status injectSessionParams(
            const std::string& cameraId,
            const hardware::camera2::impl::CameraMetadataNative& sessionParams);
    virtual binder::Status createDefaultRequest(const std::string& cameraId, int templateId,
            hardware::camera2::impl::CameraMetadataNative* request);
    virtual binder::Status isSessionConfigurationWithParametersSupported(
            const std::string& cameraId,
            const SessionConfiguration& sessionConfiguration,
            bool* supported);
    virtual binder::Status getSessionCharacteristics(
            const std::string& cameraId, int targetSdkVersion, bool overrideToPortrait,
            const SessionConfiguration& sessionConfiguration,
                    CameraMetadata* outMetadata);
    virtual status_t onTransact(uint32_t code, const Parcel& data,
                                   Parcel* reply, uint32_t flags);
    virtual status_t dump(int fd, const Vector<String16>& args);
    virtual status_t shellCommand(int in, int out, int err, const Vector<String16>& args);
    binder::Status addListenerHelper(const sp<hardware::ICameraServiceListener>& listener,
            std::vector<hardware::CameraStatus>* cameraStatuses, bool isVendor = false,
            bool isProcessLocalTest = false);
    void notifyMonitoredUids();
    void notifyMonitoredUids(const std::unordered_set<uid_t> &notifyUidSet);
    void cacheDump();
    status_t addOfflineClient(const std::string &cameraId, sp<BasicClient> offlineClient);
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
    std::pair<int, IPCTransport> getDeviceVersion(const std::string& cameraId,
            bool overrideToPortrait, int* portraitRotation,
            int* facing = nullptr, int* orientation = nullptr);
    void clearCachedVariables();
    binder::Status addListenerTest(const sp<hardware::ICameraServiceListener>& listener,
            std::vector<hardware::CameraStatus>* cameraStatuses);
    static binder::Status filterGetInfoErrorCode(status_t err);
<<<<<<< HEAD
    bool isAutomotiveExteriorSystemCamera(const std::string& cameraId) const;
||||||| 38b856f7cf
    bool isAutomotiveDevice() const;
    bool isAutomotivePrivilegedClient(int32_t uid) const;
    bool isAutomotiveExteriorSystemCamera(const std::string& cameraId) const;
=======
>>>>>>> 94c719f4
    class BasicClient :
        public virtual RefBase,
        public AttributionAndPermissionUtilsEncapsulator {
    friend class CameraService;
    public:
        virtual status_t initialize(sp<CameraProviderManager> manager,
                const std::string& monitorTags) = 0;
        virtual binder::Status disconnect();
        virtual sp<IBinder> asBinderWrapper() = 0;
        sp<IBinder> getRemote() {
            return mRemoteBinder;
        }
        bool getOverrideToPortrait() const {
            return mOverrideToPortrait;
        }
        virtual status_t dump(int fd, const Vector<String16>& args);
        virtual status_t dumpClient(int fd, const Vector<String16>& args) = 0;
        virtual status_t startWatchingTags(const std::string &tags, int outFd);
        virtual status_t stopWatchingTags(int outFd);
        virtual status_t dumpWatchedEventsToVector(std::vector<std::string> &out);
        virtual std::string getPackageName() const;
        virtual int getCameraFacing() const;
        virtual int getCameraOrientation() const;
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
        virtual status_t setRotateAndCropOverride(uint8_t rotateAndCrop, bool fromHal = false) = 0;
        virtual status_t setAutoframingOverride(uint8_t autoframingValue) = 0;
        virtual bool supportsCameraMute() = 0;
        virtual status_t setCameraMute(bool enabled) = 0;
        virtual status_t setCameraServiceWatchdog(bool enabled) = 0;
        virtual void setStreamUseCaseOverrides(
                const std::vector<int64_t>& useCaseOverrides) = 0;
        virtual void clearStreamUseCaseOverrides() = 0;
        virtual bool supportsZoomOverride() = 0;
        virtual status_t setZoomOverride(int32_t zoomOverride) = 0;
        virtual status_t injectCamera(const std::string& injectedCamId,
                sp<CameraProviderManager> manager) = 0;
        virtual status_t stopInjection() = 0;
        virtual status_t injectSessionParams(
                const hardware::camera2::impl::CameraMetadataNative& sessionParams) = 0;
    protected:
        BasicClient(const sp<CameraService>& cameraService,
                const sp<IBinder>& remoteCallback,
                const std::string& clientPackageName,
                bool nativeClient,
                const std::optional<std::string>& clientFeatureId,
                const std::string& cameraIdStr,
                int cameraFacing,
                int sensorOrientation,
                int clientPid,
                uid_t clientUid,
                int servicePid,
                bool overrideToPortrait);
        virtual ~BasicClient();
        bool mDestructionStarted;
        static sp<CameraService> sCameraService;
        const std::string mCameraIdStr;
        const int mCameraFacing;
        const int mOrientation;
        std::string mClientPackageName;
        bool mSystemNativeClient;
        std::optional<std::string> mClientFeatureId;
        pid_t mClientPid;
        const uid_t mClientUid;
        const pid_t mServicePid;
        bool mDisconnected;
        bool mUidIsTrusted;
        bool mOverrideToPortrait;
        mutable Mutex mAudioRestrictionLock;
        int32_t mAudioRestriction;
        sp<IBinder> mRemoteBinder;
        virtual status_t startCameraOps();
        virtual status_t startCameraStreamingOps();
        virtual status_t finishCameraStreamingOps();
        virtual status_t finishCameraOps();
        virtual status_t handleAppOpMode(int32_t mode);
        virtual status_t noteAppOp();
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
        bool mOpsStreaming;
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
                const std::string& clientPackageName,
                bool systemNativeClient,
                const std::optional<std::string>& clientFeatureId,
                const std::string& cameraIdStr,
                int api1CameraId,
                int cameraFacing,
                int sensorOrientation,
                int clientPid,
                uid_t clientUid,
                int servicePid,
                bool overrideToPortrait);
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
        void setImageDumpMask(int ) { }
    protected:
        sp<hardware::ICameraClient> mRemoteCallback;
        int mCameraId;
    };
    class ClientEventListener {
    public:
        void onClientAdded(const resource_policy::ClientDescriptor<std::string,
                sp<CameraService::BasicClient>>& descriptor);
        void onClientRemoved(const resource_policy::ClientDescriptor<std::string,
                sp<CameraService::BasicClient>>& descriptor);
    };
    typedef std::shared_ptr<resource_policy::ClientDescriptor<std::string,
            sp<CameraService::BasicClient>>> DescriptorPtr;
    class CameraClientManager : public resource_policy::ClientManager<std::string,
            sp<CameraService::BasicClient>, ClientEventListener> {
    public:
        CameraClientManager();
        virtual ~CameraClientManager();
        sp<CameraService::BasicClient> getCameraClient(const std::string& id) const;
        std::string toString() const;
        static DescriptorPtr makeClientDescriptor(const std::string& key,
                const sp<BasicClient>& value, int32_t cost,
                const std::set<std::string>& conflictingKeys, int32_t score,
                int32_t ownerId, int32_t state, int oomScoreOffset, bool systemNativeClient);
        static DescriptorPtr makeClientDescriptor(const sp<BasicClient>& value,
                const CameraService::DescriptorPtr& partial, int oomScoreOffset,
                bool systemNativeClient);
    };
    int32_t updateAudioRestriction();
    int32_t updateAudioRestrictionLocked();
private:
    bool isAutomotiveExteriorSystemCamera(const std::string& cameraId) const;
    static const sp<IActivityManager>& getActivityManager() {
        static const char* kActivityService = "activity";
        static const auto activityManager = []() -> sp<IActivityManager> {
            const sp<IServiceManager> sm(defaultServiceManager());
            if (sm != nullptr) {
                 return interface_cast<IActivityManager>(sm->checkService(String16(kActivityService)));
            }
            return nullptr;
        }();
        return activityManager;
    }
<<<<<<< HEAD
||||||| 38b856f7cf
    bool hasPermissionsForCamera(int callingPid, int callingUid) const;
    bool hasPermissionsForCamera(const std::string& cameraId, int callingPid, int callingUid) const;
    bool hasPermissionsForSystemCamera(const std::string& cameraId, int callingPid, int callingUid,
            bool checkCameraPermissions = true) const;
    bool hasPermissionsForCameraHeadlessSystemUser(const std::string& cameraId, int callingPid,
            int callingUid) const;
    bool hasPermissionsForCameraPrivacyAllowlist(int callingPid, int callingUid) const;
    bool hasPermissionsForOpenCloseListener(int callingPid, int callingUid) const;
=======
    bool checkPermission(const std::string& cameraId, const std::string& permission,
            const content::AttributionSourceState& attributionSource, const std::string& message,
            int32_t attributedOpCode) const;
    bool hasPermissionsForSystemCamera(const std::string& cameraId, int callingPid, int callingUid)
            const;
    bool hasPermissionsForCameraHeadlessSystemUser(const std::string& cameraId, int callingPid,
            int callingUid) const;
    bool hasCameraPermissions() const;
    bool hasPermissionsForCameraPrivacyAllowlist(int callingPid, int callingUid) const;
>>>>>>> 94c719f4
    enum class StatusInternal : int32_t {
        NOT_PRESENT = static_cast<int32_t>(CameraDeviceStatus::NOT_PRESENT),
        PRESENT = static_cast<int32_t>(CameraDeviceStatus::PRESENT),
        ENUMERATING = static_cast<int32_t>(CameraDeviceStatus::ENUMERATING),
        NOT_AVAILABLE = static_cast<int32_t>(hardware::ICameraServiceListener::STATUS_NOT_AVAILABLE),
        UNKNOWN = static_cast<int32_t>(hardware::ICameraServiceListener::STATUS_UNKNOWN)
    };
    friend int32_t format_as(StatusInternal s);
    class CameraState {
    public:
        CameraState(const std::string& id, int cost, const std::set<std::string>& conflicting,
                SystemCameraKind deviceKind, const std::vector<std::string>& physicalCameras);
        virtual ~CameraState();
        StatusInternal getStatus() const;
        template<class Func>
        void updateStatus(StatusInternal status,
                const std::string& cameraId,
                std::initializer_list<StatusInternal> rejectSourceStates,
                Func onStatusUpdatedLocked);
        CameraParameters getShimParams() const;
        void setShimParams(const CameraParameters& params);
        int getCost() const;
        std::set<std::string> getConflicting() const;
        SystemCameraKind getSystemCameraKind() const;
        bool containsPhysicalCamera(const std::string& physicalCameraId) const;
        bool addUnavailablePhysicalId(const std::string& physicalId);
        bool removeUnavailablePhysicalId(const std::string& physicalId);
        void setClientPackage(const std::string& clientPackage);
        std::string getClientPackage() const;
        std::vector<std::string> getUnavailablePhysicalIds() const;
    private:
        const std::string mId;
        StatusInternal mStatus;
        const int mCost;
        std::set<std::string> mConflicting;
        std::set<std::string> mUnavailablePhysicalIds;
        std::string mClientPackage;
        mutable Mutex mStatusLock;
        CameraParameters mShimParams;
        const SystemCameraKind mSystemCameraKind;
        const std::vector<std::string> mPhysicalCameras;
    };
    class UidPolicy :
        public BnUidObserver,
        public virtual IBinder::DeathRecipient,
        public virtual IServiceManager::LocalRegistrationCallback {
    public:
        explicit UidPolicy(sp<CameraService> service)
                : mRegistered(false), mService(service) {}
        void registerSelf();
        void unregisterSelf();
        bool isUidActive(uid_t uid, const std::string &callingPackage);
        int32_t getProcState(uid_t uid);
        void onUidGone(uid_t uid, bool disabled) override;
        void onUidActive(uid_t uid) override;
        void onUidIdle(uid_t uid, bool disabled) override;
        void onUidStateChanged(uid_t uid, int32_t procState, int64_t procStateSeq,
                int32_t capability) override;
        void onUidProcAdjChanged(uid_t uid, int adj) override;
        void addOverrideUid(uid_t uid, const std::string &callingPackage, bool active);
        void removeOverrideUid(uid_t uid, const std::string &callingPackage);
        void registerMonitorUid(uid_t uid, bool openCamera);
        void unregisterMonitorUid(uid_t uid, bool closeCamera);
        virtual void onServiceRegistration(const String16& name,
                        const sp<IBinder>& binder) override;
        virtual void binderDied(const wp<IBinder> &who);
    private:
        bool isUidActiveLocked(uid_t uid, const std::string &callingPackage);
        int32_t getProcStateLocked(uid_t uid);
        void updateOverrideUid(uid_t uid, const std::string &callingPackage, bool active,
                bool insert);
        void registerWithActivityManager();
        struct MonitoredUid {
            int32_t procState;
            int32_t procAdj;
            bool hasCamera;
            size_t refCount;
        };
        Mutex mUidLock;
        bool mRegistered;
        ActivityManager mAm;
        wp<CameraService> mService;
        std::unordered_set<uid_t> mActiveUids;
        std::unordered_map<uid_t, MonitoredUid> mMonitoredUids;
        std::unordered_map<uid_t, bool> mOverrideUids;
        sp<IBinder> mObserverToken;
    };
    class SensorPrivacyPolicy : public hardware::BnSensorPrivacyListener,
            public virtual IBinder::DeathRecipient,
            public virtual IServiceManager::LocalRegistrationCallback,
            public AttributionAndPermissionUtilsEncapsulator {
        public:
<<<<<<< HEAD
            explicit SensorPrivacyPolicy(wp<CameraService> service,
                    std::shared_ptr<AttributionAndPermissionUtils> attributionAndPermissionUtils)
                    : AttributionAndPermissionUtilsEncapsulator(attributionAndPermissionUtils),
                      mService(service),
                      mSensorPrivacyEnabled(false),
||||||| 38b856f7cf
            explicit SensorPrivacyPolicy(wp<CameraService> service,
                    std::shared_ptr<AttributionAndPermissionUtils> attributionAndPermissionUtils)
                    : mService(service),
                      mAttributionAndPermissionUtils(attributionAndPermissionUtils),
                      mSensorPrivacyEnabled(false),
=======
            explicit SensorPrivacyPolicy(wp<CameraService> service)
                    : mService(service), mSensorPrivacyEnabled(false),
>>>>>>> 94c719f4
                    mCameraPrivacyState(SensorPrivacyManager::DISABLED), mRegistered(false) {}
            void registerSelf();
            void unregisterSelf();
            bool isSensorPrivacyEnabled();
            bool isCameraPrivacyEnabled();
            int getCameraPrivacyState();
            bool isCameraPrivacyEnabled(const String16& packageName);
            binder::Status onSensorPrivacyChanged(int toggleType, int sensor,
                                                  bool enabled);
            binder::Status onSensorPrivacyStateChanged(int toggleType, int sensor, int state);
            virtual void onServiceRegistration(const String16& name,
                                               const sp<IBinder>& binder) override;
            virtual void binderDied(const wp<IBinder> &who);
        private:
            SensorPrivacyManager mSpm;
            wp<CameraService> mService;
            Mutex mSensorPrivacyLock;
            bool mSensorPrivacyEnabled;
            int mCameraPrivacyState;
            bool mRegistered;
            bool hasCameraPrivacyFeature();
            void registerWithSensorPrivacyManager();
    };
    sp<UidPolicy> mUidPolicy;
    sp<SensorPrivacyPolicy> mSensorPrivacyPolicy;
    std::shared_ptr<CameraServiceProxyWrapper> mCameraServiceProxyWrapper;
    virtual void onFirstRef();
    status_t enumerateProviders();
    void addStates(const std::string& id);
    void removeStates(const std::string& id);
    binder::Status validateConnectLocked(const std::string& cameraId, const std::string& clientName,
                     int& clientUid, int& clientPid, int& originalClientPid) const;
    binder::Status validateClientPermissionsLocked(const std::string& cameraId,
            const std::string& clientName, int& clientUid, int& clientPid,
                   int& originalClientPid) const;
    bool isCameraPrivacyEnabled(const String16& packageName,const std::string& cameraId,
           int clientPid, int ClientUid);
    status_t handleEvictionsLocked(const std::string& cameraId, int clientPid,
        apiLevel effectiveApiLevel, const sp<IBinder>& remoteCallback,
        const std::string& packageName, int scoreOffset, bool systemNativeClient,
        sp<BasicClient>* client,
        std::shared_ptr<resource_policy::ClientDescriptor<std::string, sp<BasicClient>>>* partial);
    bool shouldRejectSystemCameraConnection(const std::string& cameraId) const;
    bool shouldSkipStatusUpdates(SystemCameraKind systemCameraKind, bool isVendorListener,
            int clientPid, int clientUid);
    status_t getSystemCameraKind(const std::string& cameraId, SystemCameraKind *kind) const;
    void filterAPI1SystemCameraLocked(const std::vector<std::string> &normalDeviceIds);
    std::string getPackageNameFromUid(int clientUid);
    template<class CALLBACK, class CLIENT>
    binder::Status connectHelper(const sp<CALLBACK>& cameraCb, const std::string& cameraId,
            int api1CameraId, const std::string& clientPackageNameMaybe, bool systemNativeClient,
            const std::optional<std::string>& clientFeatureId, int clientUid, int clientPid,
            apiLevel effectiveApiLevel, bool shimUpdateOnly, int scoreOffset, int targetSdkVersion,
            bool overrideToPortrait, bool forceSlowJpegMode, const std::string& originalCameraId,
                   sp<CLIENT>& device);
    Mutex mServiceLock;
    std::shared_ptr<WaitableMutexWrapper> mServiceLockWrapper;
    status_t checkIfDeviceIsUsable(const std::string& cameraId) const;
    CameraClientManager mActiveClientManager;
    void dumpOpenSessionClientLogs(int fd, const Vector<String16>& args,
            const std::string& cameraId);
    void dumpClosedSessionClientLogs(int fd, const std::string& cameraId);
    std::map<std::string, std::shared_ptr<CameraState>> mCameraStates;
    mutable Mutex mCameraStatesLock;
    typedef std::map<std::string, std::map<std::string, std::string>> TCameraIdRemapping;
    TCameraIdRemapping mCameraIdRemapping{};
    Mutex mCameraIdRemappingLock;
    binder::Status parseCameraIdRemapping(
            const hardware::CameraIdRemapping& cameraIdRemapping,
                      TCameraIdRemapping* cameraIdRemappingMap);
    std::string resolveCameraId(
            const std::string& inputCameraId,
            int clientUid,
            const std::string& packageName = "");
    void remapCameraIds(const TCameraIdRemapping& cameraIdRemapping);
    std::vector<std::string> findOriginalIdsForRemappedCameraId(
        const std::string& inputCameraId, int clientUid);
    RingBuffer<std::string> mEventLog;
    Mutex mLogLock;
    std::set<std::string> mWatchedClientPackages;
    std::map<std::string, std::string> mWatchedClientsDumpCache;
    std::string mMonitorTags;
    std::set<userid_t> mAllowedUsers;
    std::shared_ptr<CameraService::CameraState> getCameraState(const std::string& cameraId) const;
    bool evictClientIdByRemote(const wp<IBinder>& cameraClient);
    void removeByClient(const BasicClient* client);
    void finishConnectLocked(const sp<BasicClient>& client, const DescriptorPtr& desc,
            int oomScoreOffset, bool systemNativeClient);
    std::string cameraIdIntToStr(int cameraIdInt);
    std::string cameraIdIntToStrLocked(int cameraIdInt);
    sp<CameraService::BasicClient> removeClientLocked(const std::string& cameraId);
    void doUserSwitch(const std::vector<int32_t>& newUserIds);
    void logEvent(const std::string &event);
    void logDisconnected(const std::string &cameraId, int clientPid,
            const std::string &clientPackage);
    void logDisconnectedOffline(const std::string &cameraId, int clientPid,
            const std::string &clientPackage);
    void logConnectedOffline(const std::string &cameraId, int clientPid,
            const std::string &clientPackage);
    void logConnected(const std::string &cameraId, int clientPid, const std::string &clientPackage);
    void logRejected(const std::string &cameraId, int clientPid, const std::string &clientPackage,
            const std::string &reason);
    void logTorchEvent(const std::string &cameraId, const std::string &torchState, int clientPid);
    void logUserSwitch(const std::set<userid_t>& oldUserIds,
        const std::set<userid_t>& newUserIds);
    void logDeviceRemoved(const std::string &cameraId, const std::string &reason);
    void logDeviceAdded(const std::string &cameraId, const std::string &reason);
    void logClientDied(int clientPid, const std::string &reason);
    void logServiceError(const std::string &msg, int errorCode);
    void dumpEventLog(int fd);
    void cacheClientTagDumpIfNeeded(const std::string &cameraId, BasicClient *client);
    void updateCameraNumAndIds();
    void filterSPerfClassCharacteristicsLocked();
    int mMemFd;
    int mNumberOfCameras;
    int mNumberOfCamerasWithoutSystemCamera;
    std::vector<std::string> mNormalDeviceIds;
    std::vector<std::string> mNormalDeviceIdsWithoutSystemCamera;
    std::set<std::string> mPerfClassPrimaryCameraIds;
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
            status_t initialize(bool isProcessLocalTest) {
                if (isProcessLocalTest) {
                    return OK;
                }
                return IInterface::asBinder(mListener)->linkToDeath(this);
            }
            template<typename... args_t>
            void handleBinderStatus(const binder::Status &ret, const char *logOnError,
                    args_t... args) {
                if (!ret.isOk() &&
                        (ret.exceptionCode() != binder::Status::Exception::EX_TRANSACTION_FAILED
                        || !mLastTransactFailed)) {
                    ALOGE(logOnError, args...);
                }
                if (ret.exceptionCode() == binder::Status::Exception::EX_TRANSACTION_FAILED) {
                    if (!mLastTransactFailed) {
                        ALOGE("%s: Muting similar errors from listener %d:%d", __FUNCTION__,
                                mListenerUid, mListenerPid);
                    }
                    mLastTransactFailed = true;
                } else {
                    mLastTransactFailed = false;
                }
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
            bool mLastTransactFailed = false;
    };
    std::vector<sp<ServiceListener>> mListenerList;
    Mutex mStatusListenerLock;
    void updateStatus(StatusInternal status,
            const std::string& cameraId,
            std::initializer_list<StatusInternal>
                rejectedSourceStates);
    void updateStatus(StatusInternal status,
            const std::string& cameraId);
    void updateOpenCloseStatus(const std::string& cameraId, bool open,
            const std::string& packageName);
    sp<CameraFlashlight> mFlashlight;
    Mutex mTorchStatusMutex;
    Mutex mTorchClientMapMutex;
    Mutex mTorchUidMapMutex;
    KeyedVector<std::string, TorchModeStatus>
            mTorchStatusMap;
    KeyedVector<std::string, sp<IBinder>> mTorchClientMap;
    std::map<std::string, std::pair<int, int>> mTorchUidMap;
    void handleTorchClientBinderDied(const wp<IBinder> &who);
    void onTorchStatusChangedLocked(const std::string& cameraId,
            TorchModeStatus newStatus,
            SystemCameraKind systemCameraKind);
    status_t getTorchStatusLocked(const std::string &cameraId,
             TorchModeStatus *status) const;
    status_t setTorchStatusLocked(const std::string &cameraId,
            TorchModeStatus status);
    void notifyPhysicalCameraStatusLocked(int32_t status, const std::string& physicalCameraId,
            const std::list<std::string>& logicalCameraIds, SystemCameraKind deviceKind);
    std::list<std::string> getLogicalCameras(const std::string& physicalCameraId);
    virtual void binderDied(const wp<IBinder> &who);
    binder::Status initializeShimMetadata(int cameraId);
    binder::Status getLegacyParametersLazy(int cameraId, CameraParameters* parameters);
    void blockClientsForUid(uid_t uid);
    void blockAllClients();
    void blockPrivacyEnabledClients();
    status_t handleSetUidState(const Vector<String16>& args, int err);
    status_t handleResetUidState(const Vector<String16>& args, int err);
    status_t handleGetUidState(const Vector<String16>& args, int out, int err);
    status_t handleSetRotateAndCrop(const Vector<String16>& args);
    status_t handleGetRotateAndCrop(int out);
    status_t handleSetAutoframing(const Vector<String16>& args);
    status_t handleGetAutoframing(int out);
    status_t handleSetImageDumpMask(const Vector<String16>& args);
    status_t handleGetImageDumpMask(int out);
    status_t handleSetCameraMute(const Vector<String16>& args);
    status_t handleSetStreamUseCaseOverrides(const Vector<String16>& args);
    void handleClearStreamUseCaseOverrides();
    status_t handleSetZoomOverride(const Vector<String16>& args);
    status_t handleCameraIdRemapping(const Vector<String16>& args, int errFd);
    status_t handleWatchCommand(const Vector<String16> &args, int inFd, int outFd);
    status_t handleSetCameraServiceWatchdog(const Vector<String16>& args);
    status_t startWatchingTags(const Vector<String16> &args, int outFd);
    status_t stopWatchingTags(int outFd);
    status_t clearCachedMonitoredTagDumps(int outFd);
    status_t printWatchedTags(int outFd);
    status_t printWatchedTagsUntilInterrupt(const Vector<String16> &args, int inFd, int outFd);
    void parseClientsToWatchLocked(const std::string &clients);
    status_t printHelp(int out);
    bool isClientWatched(const BasicClient *client);
    bool isClientWatchedLocked(const BasicClient *client);
    static std::string getFormattedCurrentTime();
    static binder::Status makeClient(const sp<CameraService>& cameraService,
            const sp<IInterface>& cameraCb, const std::string& packageName,
            bool systemNativeClient, const std::optional<std::string>& featureId,
            const std::string& cameraId, int api1CameraId, int facing, int sensorOrientation,
            int clientPid, uid_t clientUid, int servicePid,
            std::pair<int, IPCTransport> deviceVersionAndIPCTransport, apiLevel effectiveApiLevel,
            bool overrideForPerfClass, bool overrideToPortrait, bool forceSlowJpegMode,
            const std::string& originalCameraId,
                    sp<BasicClient>* client);
    static std::string toString(std::set<userid_t> intSet);
    static int32_t mapToInterface(TorchModeStatus status);
    static StatusInternal mapToInternal(CameraDeviceStatus status);
    static int32_t mapToInterface(StatusInternal status);
    void broadcastTorchModeStatus(const std::string& cameraId,
            TorchModeStatus status, SystemCameraKind systemCameraKind);
    void broadcastTorchStrengthLevel(const std::string& cameraId, int32_t newTorchStrengthLevel);
    void disconnectClient(const std::string& id, sp<BasicClient> clientToDisconnect);
    static const std::string kOfflineDevice;
    static const std::string kWatchAllClientsFlag;
    AppOpsManager mAppOps;
    int32_t mAudioRestriction;
    uint8_t mOverrideRotateAndCropMode = ANDROID_SCALER_ROTATE_AND_CROP_AUTO;
    uint8_t mOverrideAutoframingMode = ANDROID_CONTROL_AUTOFRAMING_AUTO;
    uint8_t mImageDumpMask = 0;
    bool mOverrideCameraMuteMode = false;
    bool mCameraServiceWatchdogEnabled = true;
    std::vector<int64_t> mStreamUseCaseOverrides;
    int32_t mZoomOverrideValue = -1;
    class InjectionStatusListener : public virtual IBinder::DeathRecipient {
        public:
            InjectionStatusListener(sp<CameraService> parent) : mParent(parent) {}
            void addListener(const sp<hardware::camera2::ICameraInjectionCallback>& callback);
            void removeListener();
            void notifyInjectionError(const std::string &injectedCamId, status_t err);
            virtual void binderDied(const wp<IBinder>& who);
        private:
            Mutex mListenerLock;
            wp<CameraService> mParent;
            sp<hardware::camera2::ICameraInjectionCallback> mCameraInjectionCallback;
    };
    sp<InjectionStatusListener> mInjectionStatusListener;
    class CameraInjectionSession : public hardware::camera2::BnCameraInjectionSession {
        public:
            CameraInjectionSession(sp<CameraService> parent) : mParent(parent) {}
            virtual ~CameraInjectionSession() {}
            binder::Status stopInjection() override;
        private:
            Mutex mInjectionSessionLock;
            wp<CameraService> mParent;
    };
    status_t checkIfInjectionCameraIsPresent(const std::string& externalCamId,
            sp<BasicClient> clientSp);
    void clearInjectionParameters();
    std::string mInjectionInternalCamId;
    std::string mInjectionExternalCamId;
    bool mInjectionInitPending = false;
    Mutex mInjectionParametersLock;
    int64_t mDeviceState;
    void updateTorchUidMapLocked(const std::string& cameraId, int uid);
};
}
#endif
