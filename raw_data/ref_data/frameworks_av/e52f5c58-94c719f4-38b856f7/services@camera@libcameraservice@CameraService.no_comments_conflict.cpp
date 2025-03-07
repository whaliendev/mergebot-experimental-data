#define LOG_TAG "CameraService"
#define ATRACE_TAG ATRACE_TAG_CAMERA
#include <algorithm>
#include <climits>
#include <stdio.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <inttypes.h>
#include <pthread.h>
#include <poll.h>
#include <android/hardware/ICamera.h>
#include <android/hardware/ICameraClient.h>
#include <aidl/AidlCameraService.h>
#include <android-base/macros.h>
#include <android-base/parseint.h>
#include <android/permission/PermissionChecker.h>
#include <binder/ActivityManager.h>
#include <binder/AppOpsManager.h>
#include <binder/IPCThreadState.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <binder/PermissionController.h>
#include <binder/IResultReceiver.h>
#include <binderthreadstate/CallerUtils.h>
#include <com_android_internal_camera_flags.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>
#include <cutils/misc.h>
#include <gui/Surface.h>
#include <hardware/hardware.h>
#include "hidl/HidlCameraService.h"
#include <hidl/HidlTransportSupport.h>
#include <hwbinder/IPCThreadState.h>
#include <memunreachable/memunreachable.h>
#include <media/AudioSystem.h>
#include <media/IMediaHTTPService.h>
#include <media/mediaplayer.h>
#include <mediautils/BatteryNotifier.h>
#include <processinfo/ProcessInfoService.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/String16.h>
#include <utils/SystemClock.h>
#include <utils/Trace.h>
#include <utils/CallStack.h>
#include <private/android_filesystem_config.h>
#include <system/camera_vendor_tags.h>
#include <system/camera_metadata.h>
#include <binder/IServiceManager.h>
#include <binder/IActivityManager.h>
#include <camera/StringUtils.h>
#include <system/camera.h>
#include "CameraService.h"
#include "api1/Camera2Client.h"
#include "api2/CameraDeviceClient.h"
#include "utils/CameraTraces.h"
#include "utils/TagMonitor.h"
#include "utils/CameraThreadState.h"
#include "utils/CameraServiceProxyWrapper.h"
#include "utils/SessionConfigurationUtils.h"
namespace {
    const char* kPermissionServiceName = "permission";
    const char* kActivityServiceName = "activity";
    const char* kSensorPrivacyServiceName = "sensor_privacy";
    const char* kAppopsServiceName = "appops";
    const char* kProcessInfoServiceName = "processinfo";
};
namespace android {
using namespace camera3;
using namespace camera3::SessionConfigurationUtils;
using binder::Status;
using frameworks::cameraservice::service::V2_0::implementation::HidlCameraService;
using frameworks::cameraservice::service::implementation::AidlCameraService;
using hardware::ICamera;
using hardware::ICameraClient;
using hardware::ICameraServiceListener;
using hardware::camera2::ICameraInjectionCallback;
using hardware::camera2::ICameraInjectionSession;
using hardware::camera2::utils::CameraIdAndSessionConfiguration;
using hardware::camera2::utils::ConcurrentCameraIdCombination;
namespace flags = com::android::internal::camera::flags;
volatile int32_t gLogLevel = 0;
#define LOG1(...) ALOGD_IF(gLogLevel >= 1, __VA_ARGS__);
#define LOG2(...) ALOGD_IF(gLogLevel >= 2, __VA_ARGS__);
static void setLogLevel(int level) {
    android_atomic_write(level, &gLogLevel);
}
int32_t format_as(CameraService::StatusInternal s) {
  return fmt::underlying(s);
}
static const std::string sDumpPermission("android.permission.DUMP");
static const std::string sManageCameraPermission("android.permission.MANAGE_CAMERA");
static const std::string sCameraPermission("android.permission.CAMERA");
static const std::string sSystemCameraPermission("android.permission.SYSTEM_CAMERA");
static const std::string sCameraHeadlessSystemUserPermission(
        "android.permission.CAMERA_HEADLESS_SYSTEM_USER");
static const std::string sCameraPrivacyAllowlistPermission(
        "android.permission.CAMERA_PRIVACY_ALLOWLIST");
static const std::string
        sCameraSendSystemEventsPermission("android.permission.CAMERA_SEND_SYSTEM_EVENTS");
static const std::string sCameraOpenCloseListenerPermission(
        "android.permission.CAMERA_OPEN_CLOSE_LISTENER");
static const std::string
        sCameraInjectExternalCameraPermission("android.permission.CAMERA_INJECT_EXTERNAL_CAMERA");
static const int LOG_FGS_CAMERA_API = 1;
const char *sFileName = "lastOpenSessionDumpFile";
static constexpr int32_t kSystemNativeClientScore = resource_policy::PERCEPTIBLE_APP_ADJ;
static constexpr int32_t kSystemNativeClientState =
        ActivityManager::PROCESS_STATE_PERSISTENT_UI;
static const std::string kServiceName("cameraserver");
const std::string CameraService::kOfflineDevice("offline-");
const std::string CameraService::kWatchAllClientsFlag("all");
static std::set<std::string> sServiceErrorEventSet;
CameraService::CameraService(
<<<<<<< HEAD
        std::shared_ptr<CameraServiceProxyWrapper> cameraServiceProxyWrapper,
        std::shared_ptr<AttributionAndPermissionUtils> attributionAndPermissionUtils) :
        AttributionAndPermissionUtilsEncapsulator(attributionAndPermissionUtils == nullptr ?
                std::make_shared<AttributionAndPermissionUtils>()\
                : attributionAndPermissionUtils),
||||||| 38b856f7cf
        std::shared_ptr<CameraServiceProxyWrapper> cameraServiceProxyWrapper,
        std::shared_ptr<AttributionAndPermissionUtils> attributionAndPermissionUtils) :
=======
        std::shared_ptr<CameraServiceProxyWrapper> cameraServiceProxyWrapper) :
>>>>>>> 94c719f4
        mCameraServiceProxyWrapper(cameraServiceProxyWrapper == nullptr ?
                std::make_shared<CameraServiceProxyWrapper>() : cameraServiceProxyWrapper),
        mEventLog(DEFAULT_EVENT_LOG_LENGTH),
        mNumberOfCameras(0),
        mNumberOfCamerasWithoutSystemCamera(0),
        mSoundRef(0), mInitialized(false),
        mAudioRestriction(hardware::camera2::ICameraDeviceUser::AUDIO_RESTRICTION_NONE) {
    ALOGI("CameraService started (pid=%d)", getpid());
    mAttributionAndPermissionUtils->setCameraService(this);
    mServiceLockWrapper = std::make_shared<WaitableMutexWrapper>(&mServiceLock);
    mMemFd = memfd_create(sFileName, MFD_ALLOW_SEALING);
    if (mMemFd == -1) {
        ALOGE("%s: Error while creating the file: %s", __FUNCTION__, sFileName);
    }
}
static bool doesClientHaveSystemUid() {
    return (CameraThreadState::getCallingUid() < AID_APP_START);
}
void CameraService::instantiate() {
    CameraService::publish(true);
}
void CameraService::onServiceRegistration(const String16& name, const sp<IBinder>&) {
    if (name != toString16(kAppopsServiceName)) {
        return;
    }
    ALOGV("appops service registered. setting camera audio restriction");
    mAppOps.setCameraAudioRestriction(mAudioRestriction);
}
void CameraService::onFirstRef()
{
    ALOGI("CameraService process starting");
    BnCameraService::onFirstRef();
    BatteryNotifier& notifier(BatteryNotifier::getInstance());
    notifier.noteResetCamera();
    notifier.noteResetFlashlight();
    status_t res = INVALID_OPERATION;
    res = enumerateProviders();
    if (res == OK) {
        mInitialized = true;
    }
    mUidPolicy = new UidPolicy(this);
    mUidPolicy->registerSelf();
    mSensorPrivacyPolicy = new SensorPrivacyPolicy(this);
    mSensorPrivacyPolicy->registerSelf();
    mInjectionStatusListener = new InjectionStatusListener(this);
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->checkService(toString16(kAppopsServiceName));
    if (!binder) {
        sm->registerForNotifications(toString16(kAppopsServiceName), this);
    } else {
        mAppOps.setCameraAudioRestriction(mAudioRestriction);
    }
    sp<HidlCameraService> hcs = HidlCameraService::getInstance(this);
    if (hcs->registerAsService() != android::OK) {
        ALOGW("%s: Did not register default android.frameworks.cameraservice.service@2.2",
              __FUNCTION__);
    }
    if (!AidlCameraService::registerService(this)) {
        ALOGE("%s: Failed to register default AIDL VNDK CameraService", __FUNCTION__);
    }
    mCameraServiceProxyWrapper->pingCameraServiceProxy();
    ALOGI("CameraService pinged cameraservice proxy");
}
status_t CameraService::enumerateProviders() {
    status_t res;
    std::vector<std::string> deviceIds;
    std::unordered_map<std::string, std::set<std::string>> unavailPhysicalIds;
    {
        Mutex::Autolock l(mServiceLock);
        if (nullptr == mCameraProviderManager.get()) {
            mCameraProviderManager = new CameraProviderManager();
            res = mCameraProviderManager->initialize(this);
            if (res != OK) {
                ALOGE("%s: Unable to initialize camera provider manager: %s (%d)",
                        __FUNCTION__, strerror(-res), res);
                logServiceError("Unable to initialize camera provider manager",
                        ERROR_DISCONNECTED);
                return res;
            }
        }
        mCameraProviderManager->setUpVendorTags();
        if (nullptr == mFlashlight.get()) {
            mFlashlight = new CameraFlashlight(mCameraProviderManager, this);
        }
        res = mFlashlight->findFlashUnits();
        if (res != OK) {
            ALOGE("Failed to enumerate flash units: %s (%d)", strerror(-res), res);
        }
        deviceIds = mCameraProviderManager->getCameraDeviceIds(&unavailPhysicalIds);
    }
    for (auto& cameraId : deviceIds) {
        if (getCameraState(cameraId) == nullptr) {
            onDeviceStatusChanged(cameraId, CameraDeviceStatus::PRESENT);
        }
        if (unavailPhysicalIds.count(cameraId) > 0) {
            for (const auto& physicalId : unavailPhysicalIds[cameraId]) {
                onDeviceStatusChanged(cameraId, physicalId, CameraDeviceStatus::NOT_PRESENT);
            }
        }
    }
    if (SessionConfigurationUtils::IS_PERF_CLASS) {
        Mutex::Autolock l(mServiceLock);
        filterSPerfClassCharacteristicsLocked();
    }
    return OK;
}
void CameraService::broadcastTorchModeStatus(const std::string& cameraId, TorchModeStatus status,
        SystemCameraKind systemCameraKind) {
    Mutex::Autolock lock(mStatusListenerLock);
    for (auto& i : mListenerList) {
        if (shouldSkipStatusUpdates(systemCameraKind, i->isVendorListener(), i->getListenerPid(),
                i->getListenerUid())) {
            ALOGV("%s: Skipping torch callback for system-only camera device %s",
                    __FUNCTION__, cameraId.c_str());
            continue;
        }
        auto ret = i->getListener()->onTorchStatusChanged(mapToInterface(status),
                cameraId);
        i->handleBinderStatus(ret, "%s: Failed to trigger onTorchStatusChanged for %d:%d: %d",
                __FUNCTION__, i->getListenerUid(), i->getListenerPid(), ret.exceptionCode());
        std::vector<std::string> remappedCameraIds =
                findOriginalIdsForRemappedCameraId(cameraId, i->getListenerUid());
        for (auto& remappedCameraId : remappedCameraIds) {
            ret = i->getListener()->onTorchStatusChanged(mapToInterface(status), remappedCameraId);
            i->handleBinderStatus(ret, "%s: Failed to trigger onTorchStatusChanged for %d:%d: %d",
                    __FUNCTION__, i->getListenerUid(), i->getListenerPid(), ret.exceptionCode());
        }
    }
}
CameraService::~CameraService() {
    VendorTagDescriptor::clearGlobalVendorTagDescriptor();
    mUidPolicy->unregisterSelf();
    mSensorPrivacyPolicy->unregisterSelf();
    mInjectionStatusListener->removeListener();
}
void CameraService::onNewProviderRegistered() {
    enumerateProviders();
}
void CameraService::filterAPI1SystemCameraLocked(
        const std::vector<std::string> &normalDeviceIds) {
    mNormalDeviceIdsWithoutSystemCamera.clear();
    for (auto &deviceId : normalDeviceIds) {
        SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
        if (getSystemCameraKind(deviceId, &deviceKind) != OK) {
            ALOGE("%s: Invalid camera id %s, skipping", __FUNCTION__, deviceId.c_str());
            continue;
        }
        if (deviceKind == SystemCameraKind::SYSTEM_ONLY_CAMERA) {
            break;
        }
        mNormalDeviceIdsWithoutSystemCamera.push_back(deviceId);
    }
    ALOGV("%s: number of API1 compatible public cameras is %zu", __FUNCTION__,
              mNormalDeviceIdsWithoutSystemCamera.size());
}
status_t CameraService::getSystemCameraKind(const std::string& cameraId,
        SystemCameraKind *kind) const {
    auto state = getCameraState(cameraId);
    if (state != nullptr) {
        *kind = state->getSystemCameraKind();
        return OK;
    }
    return mCameraProviderManager->getSystemCameraKind(cameraId, kind);
}
void CameraService::updateCameraNumAndIds() {
    Mutex::Autolock l(mServiceLock);
    std::pair<int, int> systemAndNonSystemCameras = mCameraProviderManager->getCameraCount();
    mNumberOfCameras =
            systemAndNonSystemCameras.first + systemAndNonSystemCameras.second;
    mNumberOfCamerasWithoutSystemCamera = systemAndNonSystemCameras.second;
    mNormalDeviceIds =
            mCameraProviderManager->getAPI1CompatibleCameraDeviceIds();
    filterAPI1SystemCameraLocked(mNormalDeviceIds);
}
void CameraService::filterSPerfClassCharacteristicsLocked() {
    bool firstRearCameraSeen = false, firstFrontCameraSeen = false;
    for (const auto& cameraId : mNormalDeviceIdsWithoutSystemCamera) {
        int facing = -1;
        int orientation = 0;
        int portraitRotation;
        getDeviceVersion(cameraId, false, &portraitRotation,
                       &facing, &orientation);
        if (facing == -1) {
            ALOGE("%s: Unable to get camera device \"%s\" facing", __FUNCTION__, cameraId.c_str());
            return;
        }
        if ((facing == hardware::CAMERA_FACING_BACK && !firstRearCameraSeen) ||
                (facing == hardware::CAMERA_FACING_FRONT && !firstFrontCameraSeen)) {
            status_t res = mCameraProviderManager->filterSmallJpegSizes(cameraId);
            if (res == OK) {
                mPerfClassPrimaryCameraIds.insert(cameraId);
            } else {
                ALOGE("%s: Failed to filter small JPEG sizes for performance class primary "
                        "camera %s: %s(%d)", __FUNCTION__, cameraId.c_str(), strerror(-res), res);
                break;
            }
            if (facing == hardware::CAMERA_FACING_BACK) {
                firstRearCameraSeen = true;
            }
            if (facing == hardware::CAMERA_FACING_FRONT) {
                firstFrontCameraSeen = true;
            }
        }
        if (firstRearCameraSeen && firstFrontCameraSeen) {
            break;
        }
    }
}
void CameraService::addStates(const std::string& cameraId) {
    CameraResourceCost cost;
    status_t res = mCameraProviderManager->getResourceCost(cameraId, &cost);
    if (res != OK) {
        ALOGE("Failed to query device resource cost: %s (%d)", strerror(-res), res);
        return;
    }
    SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
    res = mCameraProviderManager->getSystemCameraKind(cameraId, &deviceKind);
    if (res != OK) {
        ALOGE("Failed to query device kind: %s (%d)", strerror(-res), res);
        return;
    }
    std::vector<std::string> physicalCameraIds;
    mCameraProviderManager->isLogicalCamera(cameraId, &physicalCameraIds);
    std::set<std::string> conflicting;
    for (size_t i = 0; i < cost.conflictingDevices.size(); i++) {
        conflicting.emplace(cost.conflictingDevices[i]);
    }
    {
        Mutex::Autolock lock(mCameraStatesLock);
        mCameraStates.emplace(cameraId, std::make_shared<CameraState>(cameraId, cost.resourceCost,
                conflicting, deviceKind, physicalCameraIds));
    }
    if (mFlashlight->hasFlashUnit(cameraId)) {
        Mutex::Autolock al(mTorchStatusMutex);
        mTorchStatusMap.add(cameraId, TorchModeStatus::AVAILABLE_OFF);
        broadcastTorchModeStatus(cameraId, TorchModeStatus::AVAILABLE_OFF, deviceKind);
    }
    updateCameraNumAndIds();
    logDeviceAdded(cameraId, "Device added");
}
void CameraService::removeStates(const std::string& cameraId) {
    updateCameraNumAndIds();
    if (mFlashlight->hasFlashUnit(cameraId)) {
        Mutex::Autolock al(mTorchStatusMutex);
        mTorchStatusMap.removeItem(cameraId);
    }
    {
        Mutex::Autolock lock(mCameraStatesLock);
        mCameraStates.erase(cameraId);
    }
}
void CameraService::onDeviceStatusChanged(const std::string& cameraId,
        CameraDeviceStatus newHalStatus) {
    ALOGI("%s: Status changed for cameraId=%s, newStatus=%d", __FUNCTION__,
            cameraId.c_str(), newHalStatus);
    StatusInternal newStatus = mapToInternal(newHalStatus);
    std::shared_ptr<CameraState> state = getCameraState(cameraId);
    if (state == nullptr) {
        if (newStatus == StatusInternal::PRESENT) {
            ALOGI("%s: Unknown camera ID %s, a new camera is added",
                    __FUNCTION__, cameraId.c_str());
            addStates(cameraId);
            updateStatus(newStatus, cameraId);
        } else {
            ALOGE("%s: Bad camera ID %s", __FUNCTION__, cameraId.c_str());
        }
        return;
    }
    StatusInternal oldStatus = state->getStatus();
    if (oldStatus == newStatus) {
        ALOGE("%s: State transition to the same status %#x not allowed", __FUNCTION__, newStatus);
        return;
    }
    if (newStatus == StatusInternal::NOT_PRESENT) {
        logDeviceRemoved(cameraId, fmt::format("Device status changed from {} to {}",
                oldStatus, newStatus));
        updateStatus(StatusInternal::NOT_PRESENT, cameraId);
        sp<BasicClient> clientToDisconnectOnline, clientToDisconnectOffline;
        {
            Mutex::Autolock lock(mServiceLock);
            state->setShimParams(CameraParameters());
            clientToDisconnectOnline = removeClientLocked(cameraId);
            clientToDisconnectOffline = removeClientLocked(kOfflineDevice + cameraId);
        }
        disconnectClient(cameraId, clientToDisconnectOnline);
        disconnectClient(kOfflineDevice + cameraId, clientToDisconnectOffline);
        removeStates(cameraId);
    } else {
        if (oldStatus == StatusInternal::NOT_PRESENT) {
            logDeviceAdded(cameraId, fmt::format("Device status changed from {} to {}",
                    oldStatus, newStatus));
        }
        updateStatus(newStatus, cameraId);
    }
}
void CameraService::onDeviceStatusChanged(const std::string& id,
        const std::string& physicalId,
        CameraDeviceStatus newHalStatus) {
    ALOGI("%s: Status changed for cameraId=%s, physicalCameraId=%s, newStatus=%d",
            __FUNCTION__, id.c_str(), physicalId.c_str(), newHalStatus);
    StatusInternal newStatus = mapToInternal(newHalStatus);
    std::shared_ptr<CameraState> state = getCameraState(id);
    if (state == nullptr) {
        ALOGE("%s: Physical camera id %s status change on a non-present ID %s",
                __FUNCTION__, physicalId.c_str(), id.c_str());
        return;
    }
    StatusInternal logicalCameraStatus = state->getStatus();
    if (logicalCameraStatus != StatusInternal::PRESENT &&
            logicalCameraStatus != StatusInternal::NOT_AVAILABLE) {
        ALOGE("%s: Physical camera id %s status %d change for an invalid logical camera state %d",
                __FUNCTION__, physicalId.c_str(), newHalStatus, logicalCameraStatus);
        return;
    }
    bool updated = false;
    if (newStatus == StatusInternal::PRESENT) {
        updated = state->removeUnavailablePhysicalId(physicalId);
    } else {
        updated = state->addUnavailablePhysicalId(physicalId);
    }
    if (updated) {
        std::string idCombo = id + " : " + physicalId;
        if (newStatus == StatusInternal::PRESENT) {
            logDeviceAdded(idCombo, fmt::format("Device status changed to {}", newStatus));
        } else {
            logDeviceRemoved(idCombo, fmt::format("Device status changed to {}", newStatus));
        }
        SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
        if (getSystemCameraKind(id, &deviceKind) != OK) {
            ALOGE("%s: Invalid camera id %s, skipping", __FUNCTION__, id.c_str());
            return;
        }
        Mutex::Autolock lock(mStatusListenerLock);
        for (auto& listener : mListenerList) {
            if (shouldSkipStatusUpdates(deviceKind, listener->isVendorListener(),
                    listener->getListenerPid(), listener->getListenerUid())) {
                ALOGV("Skipping discovery callback for system-only camera device %s",
                        id.c_str());
                continue;
            }
            auto ret = listener->getListener()->onPhysicalCameraStatusChanged(
                    mapToInterface(newStatus), id, physicalId);
            listener->handleBinderStatus(ret,
                    "%s: Failed to trigger onPhysicalCameraStatusChanged for %d:%d: %d",
                    __FUNCTION__, listener->getListenerUid(), listener->getListenerPid(),
                    ret.exceptionCode());
        }
    }
}
void CameraService::disconnectClient(const std::string& id, sp<BasicClient> clientToDisconnect) {
    if (clientToDisconnect.get() != nullptr) {
        ALOGI("%s: Client for camera ID %s evicted due to device status change from HAL",
                __FUNCTION__, id.c_str());
        clientToDisconnect->notifyError(
                hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISCONNECTED,
                CaptureResultExtras{});
        clientToDisconnect->disconnect();
    }
}
void CameraService::onTorchStatusChanged(const std::string& cameraId,
        TorchModeStatus newStatus) {
    SystemCameraKind systemCameraKind = SystemCameraKind::PUBLIC;
    status_t res = getSystemCameraKind(cameraId, &systemCameraKind);
    if (res != OK) {
        ALOGE("%s: Could not get system camera kind for camera id %s", __FUNCTION__,
                cameraId.c_str());
        return;
    }
    Mutex::Autolock al(mTorchStatusMutex);
    onTorchStatusChangedLocked(cameraId, newStatus, systemCameraKind);
}
void CameraService::onTorchStatusChanged(const std::string& cameraId,
        TorchModeStatus newStatus, SystemCameraKind systemCameraKind) {
    Mutex::Autolock al(mTorchStatusMutex);
    onTorchStatusChangedLocked(cameraId, newStatus, systemCameraKind);
}
void CameraService::broadcastTorchStrengthLevel(const std::string& cameraId,
        int32_t newStrengthLevel) {
    Mutex::Autolock lock(mStatusListenerLock);
    for (auto& i : mListenerList) {
        auto ret = i->getListener()->onTorchStrengthLevelChanged(cameraId, newStrengthLevel);
        i->handleBinderStatus(ret,
                "%s: Failed to trigger onTorchStrengthLevelChanged for %d:%d: %d", __FUNCTION__,
                i->getListenerUid(), i->getListenerPid(), ret.exceptionCode());
    }
}
void CameraService::onTorchStatusChangedLocked(const std::string& cameraId,
        TorchModeStatus newStatus, SystemCameraKind systemCameraKind) {
    ALOGI("%s: Torch status changed for cameraId=%s, newStatus=%d",
            __FUNCTION__, cameraId.c_str(), newStatus);
    TorchModeStatus status;
    status_t res = getTorchStatusLocked(cameraId, &status);
    if (res) {
        ALOGE("%s: cannot get torch status of camera %s: %s (%d)",
                __FUNCTION__, cameraId.c_str(), strerror(-res), res);
        return;
    }
    if (status == newStatus) {
        return;
    }
    res = setTorchStatusLocked(cameraId, newStatus);
    if (res) {
        ALOGE("%s: Failed to set the torch status to %d: %s (%d)", __FUNCTION__,
                (uint32_t)newStatus, strerror(-res), res);
        return;
    }
    {
        Mutex::Autolock al(mTorchUidMapMutex);
        auto iter = mTorchUidMap.find(cameraId);
        if (iter != mTorchUidMap.end()) {
            int oldUid = iter->second.second;
            int newUid = iter->second.first;
            BatteryNotifier& notifier(BatteryNotifier::getInstance());
            if (oldUid != newUid) {
                if (status == TorchModeStatus::AVAILABLE_ON) {
                    notifier.noteFlashlightOff(toString8(cameraId), oldUid);
                }
                if (newStatus == TorchModeStatus::AVAILABLE_ON) {
                    notifier.noteFlashlightOn(toString8(cameraId), newUid);
                }
                iter->second.second = newUid;
            } else {
                if (newStatus == TorchModeStatus::AVAILABLE_ON) {
                    notifier.noteFlashlightOn(toString8(cameraId), oldUid);
                } else {
                    notifier.noteFlashlightOff(toString8(cameraId), oldUid);
                }
            }
        }
    }
    broadcastTorchModeStatus(cameraId, newStatus, systemCameraKind);
}
<<<<<<< HEAD
bool CameraService::isAutomotiveExteriorSystemCamera(const std::string& cam_id) const {
||||||| 38b856f7cf
bool CameraService::isAutomotiveDevice() const {
    return mAttributionAndPermissionUtils->isAutomotiveDevice();
}
bool CameraService::isAutomotivePrivilegedClient(int32_t uid) const {
    return mAttributionAndPermissionUtils->isAutomotivePrivilegedClient(uid);
}
bool CameraService::isAutomotiveExteriorSystemCamera(const std::string& cam_id) const {
=======
static bool isAutomotiveDevice() {
    char value[PROPERTY_VALUE_MAX] = {0};
    property_get("ro.hardware.type", value, "");
    return strncmp(value, "automotive", PROPERTY_VALUE_MAX) == 0;
}
static bool isHeadlessSystemUserMode() {
    char value[PROPERTY_VALUE_MAX] = {0};
    property_get("ro.fw.mu.headless_system_user", value, "");
    return strncmp(value, "true", PROPERTY_VALUE_MAX) == 0;
}
static bool isAutomotivePrivilegedClient(int32_t uid) {
    if (!isAutomotiveDevice())
        return false;
    return uid == AID_AUTOMOTIVE_EVS;
}
bool CameraService::isAutomotiveExteriorSystemCamera(const std::string& cam_id) const{
>>>>>>> 94c719f4
    if (!isAutomotiveDevice())
        return false;
    if (cam_id.empty())
        return false;
    SystemCameraKind systemCameraKind = SystemCameraKind::PUBLIC;
    if (getSystemCameraKind(cam_id, &systemCameraKind) != OK) {
        ALOGE("%s: Unknown camera id %s, ", __FUNCTION__, cam_id.c_str());
        return false;
    }
    if (systemCameraKind != SystemCameraKind::SYSTEM_ONLY_CAMERA) {
        ALOGE("%s: camera id %s is not a system camera", __FUNCTION__, cam_id.c_str());
        return false;
    }
    CameraMetadata cameraInfo;
    status_t res = mCameraProviderManager->getCameraCharacteristics(
            cam_id, false, &cameraInfo, false);
    if (res != OK){
        ALOGE("%s: Not able to get camera characteristics for camera id %s",__FUNCTION__,
                cam_id.c_str());
        return false;
    }
    camera_metadata_entry auto_location = cameraInfo.find(ANDROID_AUTOMOTIVE_LOCATION);
    if (auto_location.count != 1)
        return false;
    uint8_t location = auto_location.data.u8[0];
    if ((location != ANDROID_AUTOMOTIVE_LOCATION_EXTERIOR_FRONT) &&
            (location != ANDROID_AUTOMOTIVE_LOCATION_EXTERIOR_REAR) &&
            (location != ANDROID_AUTOMOTIVE_LOCATION_EXTERIOR_LEFT) &&
            (location != ANDROID_AUTOMOTIVE_LOCATION_EXTERIOR_RIGHT)) {
        return false;
    }
    return true;
}
<<<<<<< HEAD
||||||| 38b856f7cf
static AttributionSourceState attributionSourceFromPidAndUid(int callingPid, int callingUid) {
    AttributionSourceState attributionSource{};
    attributionSource.pid = callingPid;
    attributionSource.uid = callingUid;
    return attributionSource;
}
bool CameraService::hasPermissionsForCamera(int callingPid, int callingUid) const {
    return hasPermissionsForCamera(std::string(), callingPid, callingUid);
}
bool CameraService::hasPermissionsForCamera(const std::string& cameraId, int callingPid,
        int callingUid) const {
    auto attributionSource = attributionSourceFromPidAndUid(callingPid, callingUid);
    return mAttributionAndPermissionUtils->hasPermissionsForCamera(cameraId, attributionSource);
}
bool CameraService::hasPermissionsForSystemCamera(const std::string& cameraId, int callingPid,
        int callingUid, bool checkCameraPermissions) const {
    auto attributionSource = attributionSourceFromPidAndUid(callingPid, callingUid);
    return mAttributionAndPermissionUtils->hasPermissionsForSystemCamera(
                cameraId, attributionSource, checkCameraPermissions);
}
bool CameraService::hasPermissionsForCameraHeadlessSystemUser(const std::string& cameraId,
        int callingPid, int callingUid) const {
    auto attributionSource = attributionSourceFromPidAndUid(callingPid, callingUid);
    return mAttributionAndPermissionUtils->hasPermissionsForCameraHeadlessSystemUser(
                cameraId, attributionSource);
}
bool CameraService::hasPermissionsForCameraPrivacyAllowlist(int callingPid, int callingUid) const {
    auto attributionSource = attributionSourceFromPidAndUid(callingPid, callingUid);
    return mAttributionAndPermissionUtils->hasPermissionsForCameraPrivacyAllowlist(
            attributionSource);
}
bool CameraService::hasPermissionsForOpenCloseListener(int callingPid, int callingUid) const {
    auto attributionSource = attributionSourceFromPidAndUid(callingPid, callingUid);
    return mAttributionAndPermissionUtils->hasPermissionsForOpenCloseListener(
            attributionSource);
}
=======
bool CameraService::checkPermission(const std::string& cameraId, const std::string& permission,
        const AttributionSourceState& attributionSource, const std::string& message,
        int32_t attributedOpCode) const{
    if (isAutomotivePrivilegedClient(attributionSource.uid)) {
        if (cameraId.empty())
            return true;
        return isAutomotiveExteriorSystemCamera(cameraId);
    }
    permission::PermissionChecker permissionChecker;
    return permissionChecker.checkPermissionForPreflight(toString16(permission), attributionSource,
            toString16(message), attributedOpCode)
            != permission::PermissionChecker::PERMISSION_HARD_DENIED;
}
bool CameraService::hasPermissionsForSystemCamera(const std::string& cameraId, int callingPid,
        int callingUid) const{
    AttributionSourceState attributionSource{};
    attributionSource.pid = callingPid;
    attributionSource.uid = callingUid;
    bool checkPermissionForSystemCamera = checkPermission(cameraId,
            sSystemCameraPermission, attributionSource, std::string(), AppOpsManager::OP_NONE);
    bool checkPermissionForCamera = checkPermission(cameraId,
            sCameraPermission, attributionSource, std::string(), AppOpsManager::OP_NONE);
    return checkPermissionForSystemCamera && checkPermissionForCamera;
}
bool CameraService::hasPermissionsForCameraHeadlessSystemUser(const std::string& cameraId,
        int callingPid, int callingUid) const{
    AttributionSourceState attributionSource{};
    attributionSource.pid = callingPid;
    attributionSource.uid = callingUid;
    return checkPermission(cameraId, sCameraHeadlessSystemUserPermission, attributionSource,
            std::string(), AppOpsManager::OP_NONE);
}
bool CameraService::hasPermissionsForCameraPrivacyAllowlist(int callingPid, int callingUid) const{
    AttributionSourceState attributionSource{};
    attributionSource.pid = callingPid;
    attributionSource.uid = callingUid;
    return checkPermission(std::string(), sCameraPrivacyAllowlistPermission, attributionSource,
            std::string(), AppOpsManager::OP_NONE);
}
>>>>>>> 94c719f4
Status CameraService::getNumberOfCameras(int32_t type, int32_t* numCameras) {
    ATRACE_CALL();
    Mutex::Autolock l(mServiceLock);
    bool hasSystemCameraPermissions =
            hasPermissionsForSystemCamera(std::string(), CameraThreadState::getCallingPid(),
                    CameraThreadState::getCallingUid());
    switch (type) {
        case CAMERA_TYPE_BACKWARD_COMPATIBLE:
            if (hasSystemCameraPermissions) {
                *numCameras = static_cast<int>(mNormalDeviceIds.size());
            } else {
                *numCameras = static_cast<int>(mNormalDeviceIdsWithoutSystemCamera.size());
            }
            break;
        case CAMERA_TYPE_ALL:
            if (hasSystemCameraPermissions) {
                *numCameras = mNumberOfCameras;
            } else {
                *numCameras = mNumberOfCamerasWithoutSystemCamera;
            }
            break;
        default:
            ALOGW("%s: Unknown camera type %d",
                    __FUNCTION__, type);
            return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                    "Unknown camera type %d", type);
    }
    return Status::ok();
}
Status CameraService::remapCameraIds(const hardware::CameraIdRemapping& cameraIdRemapping) {
    if (!checkCallingPermission(toString16(sCameraInjectExternalCameraPermission))) {
        const int pid = CameraThreadState::getCallingPid();
        const int uid = CameraThreadState::getCallingUid();
        ALOGE("%s: Permission Denial: can't configure camera ID mapping pid=%d, uid=%d",
                __FUNCTION__, pid, uid);
        return STATUS_ERROR(ERROR_PERMISSION_DENIED,
                "Permission Denial: no permission to configure camera id mapping");
    }
    TCameraIdRemapping cameraIdRemappingMap{};
    binder::Status parseStatus = parseCameraIdRemapping(cameraIdRemapping, &cameraIdRemappingMap);
    if (!parseStatus.isOk()) {
        return parseStatus;
    }
    remapCameraIds(cameraIdRemappingMap);
    return Status::ok();
}
Status CameraService::createDefaultRequest(const std::string& unresolvedCameraId, int templateId,
        hardware::camera2::impl::CameraMetadataNative* request) {
    ATRACE_CALL();
    if (!flags::feature_combination_query()) {
        return STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION,
                "Camera subsystem doesn't support this method!");
    }
    if (!mInitialized) {
        ALOGE("%s: Camera subsystem is not available", __FUNCTION__);
        logServiceError("Camera subsystem is not available", ERROR_DISCONNECTED);
        return STATUS_ERROR(ERROR_DISCONNECTED, "Camera subsystem is not available");
    }
    const std::string cameraId = resolveCameraId(unresolvedCameraId,
            CameraThreadState::getCallingUid());
    binder::Status res;
    if (request == nullptr) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %s: Error creating default request", cameraId.c_str());
        return res;
    }
    camera_request_template_t tempId = camera_request_template_t::CAMERA_TEMPLATE_COUNT;
    res = SessionConfigurationUtils::mapRequestTemplateFromClient(
            cameraId, templateId, &tempId);
    if (!res.isOk()) {
        ALOGE("%s: Camera %s: failed to map request Template %d",
                __FUNCTION__, cameraId.c_str(), templateId);
        return res;
    }
    if (shouldRejectSystemCameraConnection(cameraId)) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to create default"
                "request for system only device %s: ", cameraId.c_str());
    }
    CameraMetadata metadata;
    status_t err = mCameraProviderManager->createDefaultRequest(cameraId, tempId, &metadata);
    if (err == OK) {
        request->swap(metadata);
    } else if (err == BAD_VALUE) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_ILLEGAL_ARGUMENT,
                "Camera %s: Template ID %d is invalid or not supported: %s (%d)",
                cameraId.c_str(), templateId, strerror(-err), err);
    } else {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %s: Error creating default request for template %d: %s (%d)",
                cameraId.c_str(), templateId, strerror(-err), err);
    }
    return res;
}
Status CameraService::isSessionConfigurationWithParametersSupported(
        const std::string& unresolvedCameraId,
        const SessionConfiguration& sessionConfiguration,
        bool* supported) {
    ATRACE_CALL();
    if (!flags::feature_combination_query()) {
        return STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION,
                "Camera subsystem doesn't support this method!");
    }
    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        logServiceError("Camera subsystem is not available", ERROR_DISCONNECTED);
        return STATUS_ERROR(ERROR_DISCONNECTED, "Camera subsystem is not available");
    }
    const std::string cameraId = resolveCameraId(unresolvedCameraId,
            CameraThreadState::getCallingUid());
    if (supported == nullptr) {
        std::string msg = fmt::sprintf("Camera %s: Invalid 'support' input!",
                unresolvedCameraId.c_str());
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }
    if (shouldRejectSystemCameraConnection(cameraId)) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to query "
                "session configuration with parameters support for system only device %s: ",
                cameraId.c_str());
    }
    *supported = false;
    status_t ret = mCameraProviderManager->isSessionConfigurationSupported(cameraId.c_str(),
            sessionConfiguration, false, true,
            supported);
    binder::Status res;
    switch (ret) {
        case OK:
            break;
        case INVALID_OPERATION: {
                std::string msg = fmt::sprintf(
                        "Camera %s: Session configuration query not supported!",
                        cameraId.c_str());
                ALOGD("%s: %s", __FUNCTION__, msg.c_str());
                res = STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.c_str());
            }
            break;
        default: {
                std::string msg = fmt::sprintf( "Camera %s: Error: %s (%d)", cameraId.c_str(),
                        strerror(-ret), ret);
                ALOGE("%s: %s", __FUNCTION__, msg.c_str());
                res = STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT,
                        msg.c_str());
            }
    }
    return res;
}
Status CameraService::getSessionCharacteristics(const std::string& unresolvedCameraId,
                                                int targetSdkVersion, bool overrideToPortrait,
                                                const SessionConfiguration& sessionConfiguration,
                                                        CameraMetadata* outMetadata) {
    ATRACE_CALL();
    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        logServiceError("Camera subsystem is not available", ERROR_DISCONNECTED);
        return STATUS_ERROR(ERROR_DISCONNECTED, "Camera subsystem is not available");
    }
    const std::string cameraId =
            resolveCameraId(unresolvedCameraId, CameraThreadState::getCallingUid());
    if (outMetadata == nullptr) {
        std::string msg =
                fmt::sprintf("Camera %s: Invalid 'outMetadata' input!", unresolvedCameraId.c_str());
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }
    bool overrideForPerfClass = SessionConfigurationUtils::targetPerfClassPrimaryCamera(
            mPerfClassPrimaryCameraIds, cameraId, targetSdkVersion);
    status_t ret = mCameraProviderManager->getSessionCharacteristics(
            cameraId, sessionConfiguration, overrideForPerfClass, overrideToPortrait, outMetadata);
    Status res = Status::ok();
    switch (ret) {
        case OK:
            break;
        case INVALID_OPERATION: {
                std::string msg = fmt::sprintf(
                        "Camera %s: Session characteristics query not supported!",
                        cameraId.c_str());
                ALOGD("%s: %s", __FUNCTION__, msg.c_str());
                res = STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.c_str());
            }
            break;
        default: {
                std::string msg = fmt::sprintf("Camera %s: Error: %s (%d)", cameraId.c_str(),
                                               strerror(-ret), ret);
                ALOGE("%s: %s", __FUNCTION__, msg.c_str());
                res = STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
            }
    }
    return res;
}
Status CameraService::parseCameraIdRemapping(
        const hardware::CameraIdRemapping& cameraIdRemapping,
                  TCameraIdRemapping* cameraIdRemappingMap) {
    std::string packageName;
    std::string cameraIdToReplace, updatedCameraId;
    for(const auto& packageIdRemapping: cameraIdRemapping.packageIdRemappings) {
        packageName = packageIdRemapping.packageName;
        if (packageName.empty()) {
            return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT,
                    "CameraIdRemapping: Package name cannot be empty");
        }
        if (packageIdRemapping.cameraIdsToReplace.size()
            != packageIdRemapping.updatedCameraIds.size()) {
            return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                    "CameraIdRemapping: Mismatch in CameraId Remapping lists sizes for package %s",
                    packageName.c_str());
        }
        for(size_t i = 0; i < packageIdRemapping.cameraIdsToReplace.size(); i++) {
            cameraIdToReplace = packageIdRemapping.cameraIdsToReplace[i];
            updatedCameraId = packageIdRemapping.updatedCameraIds[i];
            if (cameraIdToReplace.empty() || updatedCameraId.empty()) {
                return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                        "CameraIdRemapping: Camera Id cannot be empty for package %s",
                        packageName.c_str());
            }
            if (cameraIdToReplace == updatedCameraId) {
                return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                        "CameraIdRemapping: CameraIdToReplace cannot be the same"
                        " as updatedCameraId for %s",
                        packageName.c_str());
            }
            (*cameraIdRemappingMap)[packageName][cameraIdToReplace] = updatedCameraId;
        }
    }
    return Status::ok();
}
void CameraService::remapCameraIds(const TCameraIdRemapping& cameraIdRemapping) {
    std::unique_ptr<AutoConditionLock> serviceLockWrapper =
            AutoConditionLock::waitAndAcquire(mServiceLockWrapper);
    std::vector<sp<BasicClient>> clientsToDisconnect;
    std::vector<std::string> cameraIdsToUpdate;
    for (const auto& [packageName, injectionMap] : cameraIdRemapping) {
        for (auto& [id0, id1] : injectionMap) {
            ALOGI("%s: UPDATE:= %s: %s: %s", __FUNCTION__, packageName.c_str(),
                    id0.c_str(), id1.c_str());
            auto clientDescriptor = mActiveClientManager.get(id0);
            if (clientDescriptor != nullptr) {
                sp<BasicClient> clientSp = clientDescriptor->getValue();
                if (clientSp->getPackageName() == packageName) {
                    clientsToDisconnect.push_back(clientSp);
                    cameraIdsToUpdate.push_back(id0);
                }
            }
        }
    }
    for (auto& clientSp : clientsToDisconnect) {
        clientSp->notifyError(hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISCONNECTED,
                CaptureResultExtras{});
    }
    mServiceLock.unlock();
    int64_t token = CameraThreadState::clearCallingIdentity();
    for (auto& clientSp : clientsToDisconnect) {
        clientSp->disconnect();
    }
    clientsToDisconnect.clear();
    CameraThreadState::restoreCallingIdentity(token);
    mServiceLock.lock();
    {
        Mutex::Autolock lock(mCameraIdRemappingLock);
        mCameraIdRemapping.clear();
        mCameraIdRemapping.insert(cameraIdRemapping.begin(), cameraIdRemapping.end());
    }
}
Status CameraService::injectSessionParams(
            const std::string& cameraId,
            const CameraMetadata& sessionParams) {
   if (!checkCallingPermission(toString16(sCameraInjectExternalCameraPermission))) {
        const int pid = CameraThreadState::getCallingPid();
        const int uid = CameraThreadState::getCallingUid();
        ALOGE("%s: Permission Denial: can't inject session params pid=%d, uid=%d",
                __FUNCTION__, pid, uid);
        return STATUS_ERROR(ERROR_PERMISSION_DENIED,
                "Permission Denial: no permission to inject session params");
    }
    std::unique_ptr<AutoConditionLock> serviceLockWrapper =
            AutoConditionLock::waitAndAcquire(mServiceLockWrapper);
    auto clientDescriptor = mActiveClientManager.get(cameraId);
    if (clientDescriptor == nullptr) {
        ALOGI("%s: No active client for camera id %s", __FUNCTION__, cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                "No active client for camera id %s", cameraId.c_str());
    }
    sp<BasicClient> clientSp = clientDescriptor->getValue();
    status_t res = clientSp->injectSessionParams(sessionParams);
    if (res != OK) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                "Error injecting session params into camera \"%s\": %s (%d)",
                cameraId.c_str(), strerror(-res), res);
    }
    return Status::ok();
}
std::vector<std::string> CameraService::findOriginalIdsForRemappedCameraId(
    const std::string& inputCameraId, int clientUid) {
    std::string packageName = getPackageNameFromUid(clientUid);
    std::vector<std::string> cameraIds;
    Mutex::Autolock lock(mCameraIdRemappingLock);
    if (auto packageMapIter = mCameraIdRemapping.find(packageName);
        packageMapIter != mCameraIdRemapping.end()) {
        for (auto& [id0, id1]: packageMapIter->second) {
            if (id1 == inputCameraId) {
                cameraIds.push_back(id0);
            }
        }
    }
    return cameraIds;
}
std::string CameraService::resolveCameraId(
    const std::string& inputCameraId,
    int clientUid,
    const std::string& packageName) {
    std::string packageNameVal = packageName;
    if (packageName.empty()) {
        packageNameVal = getPackageNameFromUid(clientUid);
    }
    if (clientUid < AID_APP_START || packageNameVal.empty()) {
        return inputCameraId;
    }
    Mutex::Autolock lock(mCameraIdRemappingLock);
    if (auto packageMapIter = mCameraIdRemapping.find(packageNameVal);
        packageMapIter != mCameraIdRemapping.end()) {
        auto packageMap = packageMapIter->second;
        if (auto replacementIdIter = packageMap.find(inputCameraId);
            replacementIdIter != packageMap.end()) {
            ALOGI("%s: resolveCameraId: remapping cameraId %s for %s to %s",
                    __FUNCTION__, inputCameraId.c_str(),
                    packageNameVal.c_str(),
                    replacementIdIter->second.c_str());
            return replacementIdIter->second;
        }
    }
    return inputCameraId;
}
Status CameraService::getCameraInfo(int cameraId, bool overrideToPortrait,
        CameraInfo* cameraInfo) {
    ATRACE_CALL();
    Mutex::Autolock l(mServiceLock);
    std::string unresolvedCameraId = cameraIdIntToStrLocked(cameraId);
    std::string cameraIdStr = resolveCameraId(
            unresolvedCameraId, CameraThreadState::getCallingUid());
    if (shouldRejectSystemCameraConnection(cameraIdStr)) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to retrieve camera"
                "characteristics for system only device %s: ", cameraIdStr.c_str());
    }
    if (!mInitialized) {
        logServiceError("Camera subsystem is not available", ERROR_DISCONNECTED);
        return STATUS_ERROR(ERROR_DISCONNECTED,
                "Camera subsystem is not available");
    }
    bool hasSystemCameraPermissions = hasPermissionsForSystemCamera(std::to_string(cameraId),
            CameraThreadState::getCallingPid(), CameraThreadState::getCallingUid());
    int cameraIdBound = mNumberOfCamerasWithoutSystemCamera;
    if (hasSystemCameraPermissions) {
        cameraIdBound = mNumberOfCameras;
    }
    if (cameraId < 0 || cameraId >= cameraIdBound) {
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT,
                "CameraId is not valid");
    }
    Status ret = Status::ok();
    int portraitRotation;
    status_t err = mCameraProviderManager->getCameraInfo(
            cameraIdStr, overrideToPortrait, &portraitRotation, cameraInfo);
    if (err != OK) {
        ret = STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                "Error retrieving camera info from device %d: %s (%d)", cameraId,
                strerror(-err), err);
        logServiceError(std::string("Error retrieving camera info from device ")
                + std::to_string(cameraId), ERROR_INVALID_OPERATION);
    }
    return ret;
}
std::string CameraService::cameraIdIntToStrLocked(int cameraIdInt) {
    const std::vector<std::string> *deviceIds = &mNormalDeviceIdsWithoutSystemCamera;
    auto callingPid = CameraThreadState::getCallingPid();
    auto callingUid = CameraThreadState::getCallingUid();
    AttributionSourceState attributionSource{};
    attributionSource.pid = callingPid;
    attributionSource.uid = callingUid;
    bool checkPermissionForSystemCamera = checkPermission(std::to_string(cameraIdInt),
                sSystemCameraPermission, attributionSource, std::string(),
                AppOpsManager::OP_NONE);
    if (checkPermissionForSystemCamera || getpid() == callingPid) {
        deviceIds = &mNormalDeviceIds;
    }
    if (cameraIdInt < 0 || cameraIdInt >= static_cast<int>(deviceIds->size())) {
        ALOGE("%s: input id %d invalid: valid range  (0, %zu)",
                __FUNCTION__, cameraIdInt, deviceIds->size());
        return std::string{};
    }
    return (*deviceIds)[cameraIdInt];
}
std::string CameraService::cameraIdIntToStr(int cameraIdInt) {
    Mutex::Autolock lock(mServiceLock);
    return cameraIdIntToStrLocked(cameraIdInt);
}
Status CameraService::getCameraCharacteristics(const std::string& unresolvedCameraId,
        int targetSdkVersion, bool overrideToPortrait, CameraMetadata* cameraInfo) {
    ATRACE_CALL();
    const std::string cameraId = resolveCameraId(unresolvedCameraId,
            CameraThreadState::getCallingUid());
    if (!cameraInfo) {
        ALOGE("%s: cameraInfo is NULL", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "cameraInfo is NULL");
    }
    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        logServiceError("Camera subsystem is not available", ERROR_DISCONNECTED);
        return STATUS_ERROR(ERROR_DISCONNECTED,
                "Camera subsystem is not available");;
    }
    if (shouldRejectSystemCameraConnection(cameraId)) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to retrieve camera"
                "characteristics for system only device %s: ", cameraId.c_str());
    }
    Status ret{};
    bool overrideForPerfClass =
            SessionConfigurationUtils::targetPerfClassPrimaryCamera(mPerfClassPrimaryCameraIds,
                    cameraId, targetSdkVersion);
    status_t res = mCameraProviderManager->getCameraCharacteristics(
            cameraId, overrideForPerfClass, cameraInfo, overrideToPortrait);
    if (res != OK) {
        if (res == NAME_NOT_FOUND) {
            return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT, "Unable to retrieve camera "
                    "characteristics for unknown device %s: %s (%d)", cameraId.c_str(),
                    strerror(-res), res);
        } else {
            logServiceError(fmt::sprintf("Unable to retrieve camera characteristics for device %s.",
                    cameraId.c_str()), ERROR_INVALID_OPERATION);
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to retrieve camera "
                    "characteristics for device %s: %s (%d)", cameraId.c_str(),
                    strerror(-res), res);
        }
    }
    SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
    if (getSystemCameraKind(cameraId, &deviceKind) != OK) {
        ALOGE("%s: Invalid camera id %s, skipping", __FUNCTION__, cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to retrieve camera kind "
                "for device %s", cameraId.c_str());
    }
    int callingPid = CameraThreadState::getCallingPid();
    int callingUid = CameraThreadState::getCallingUid();
    std::vector<int32_t> tagsRemoved;
    AttributionSourceState attributionSource{};
    attributionSource.pid = callingPid;
    attributionSource.uid = callingUid;
    bool checkPermissionForCamera = checkPermission(cameraId, sCameraPermission,
            attributionSource, std::string(), AppOpsManager::OP_NONE);
    if ((callingPid != getpid()) &&
            (deviceKind != SystemCameraKind::SYSTEM_ONLY_CAMERA) &&
            !checkPermissionForCamera) {
        res = cameraInfo->removePermissionEntries(
                mCameraProviderManager->getProviderTagIdLocked(cameraId),
                &tagsRemoved);
        if (res != OK) {
            cameraInfo->clear();
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Failed to remove camera"
                    " characteristics needing camera permission for device %s: %s (%d)",
                    cameraId.c_str(), strerror(-res), res);
        }
    }
    if (!tagsRemoved.empty()) {
        res = cameraInfo->update(ANDROID_REQUEST_CHARACTERISTIC_KEYS_NEEDING_PERMISSION,
                tagsRemoved.data(), tagsRemoved.size());
        if (res != OK) {
            cameraInfo->clear();
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Failed to insert camera "
                    "keys needing permission for device %s: %s (%d)", cameraId.c_str(),
                    strerror(-res), res);
        }
    }
    return ret;
}
Status CameraService::getTorchStrengthLevel(const std::string& unresolvedCameraId,
        int32_t* torchStrength) {
    ATRACE_CALL();
    Mutex::Autolock l(mServiceLock);
    const std::string cameraId = resolveCameraId(
        unresolvedCameraId, CameraThreadState::getCallingUid());
    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized.", __FUNCTION__);
        return STATUS_ERROR(ERROR_DISCONNECTED, "Camera HAL couldn't be initialized.");
    }
    if(torchStrength == NULL) {
        ALOGE("%s: strength level must not be null.", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Strength level should not be null.");
    }
    status_t res = mCameraProviderManager->getTorchStrengthLevel(cameraId, torchStrength);
    if (res != OK) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to retrieve torch "
            "strength level for device %s: %s (%d)", cameraId.c_str(),
            strerror(-res), res);
    }
    ALOGI("%s: Torch strength level is: %d", __FUNCTION__, *torchStrength);
    return Status::ok();
}
std::string CameraService::getFormattedCurrentTime() {
    time_t now = time(nullptr);
    char formattedTime[64];
    strftime(formattedTime, sizeof(formattedTime), "%m-%d %H:%M:%S", localtime(&now));
    return std::string(formattedTime);
}
Status CameraService::getCameraVendorTagDescriptor(
        hardware::camera2::params::VendorTagDescriptor* desc) {
    ATRACE_CALL();
    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        return STATUS_ERROR(ERROR_DISCONNECTED, "Camera subsystem not available");
    }
    sp<VendorTagDescriptor> globalDescriptor = VendorTagDescriptor::getGlobalVendorTagDescriptor();
    if (globalDescriptor != nullptr) {
        *desc = *(globalDescriptor.get());
    }
    return Status::ok();
}
Status CameraService::getCameraVendorTagCache(
                hardware::camera2::params::VendorTagDescriptorCache* cache) {
    ATRACE_CALL();
    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        return STATUS_ERROR(ERROR_DISCONNECTED,
                "Camera subsystem not available");
    }
    sp<VendorTagDescriptorCache> globalCache =
            VendorTagDescriptorCache::getGlobalVendorTagCache();
    if (globalCache != nullptr) {
        *cache = *(globalCache.get());
    }
    return Status::ok();
}
void CameraService::clearCachedVariables() {
    BasicClient::BasicClient::sCameraService = nullptr;
}
std::pair<int, IPCTransport> CameraService::getDeviceVersion(const std::string& cameraId,
        bool overrideToPortrait, int* portraitRotation, int* facing, int* orientation) {
    ATRACE_CALL();
    int deviceVersion = 0;
    status_t res;
    hardware::hidl_version maxVersion{0,0};
    IPCTransport transport = IPCTransport::INVALID;
    res = mCameraProviderManager->getHighestSupportedVersion(cameraId, &maxVersion, &transport);
    if (res != OK || transport == IPCTransport::INVALID) {
        ALOGE("%s: Unable to get highest supported version for camera id %s", __FUNCTION__,
                cameraId.c_str());
        return std::make_pair(-1, IPCTransport::INVALID) ;
    }
    deviceVersion = HARDWARE_DEVICE_API_VERSION(maxVersion.get_major(), maxVersion.get_minor());
    hardware::CameraInfo info;
    if (facing) {
        res = mCameraProviderManager->getCameraInfo(cameraId, overrideToPortrait,
                portraitRotation, &info);
        if (res != OK) {
            return std::make_pair(-1, IPCTransport::INVALID);
        }
        *facing = info.facing;
        if (orientation) {
            *orientation = info.orientation;
        }
    }
    return std::make_pair(deviceVersion, transport);
}
Status CameraService::filterGetInfoErrorCode(status_t err) {
    switch(err) {
        case NO_ERROR:
            return Status::ok();
        case BAD_VALUE:
            return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT,
                    "CameraId is not valid for HAL module");
        case NO_INIT:
            return STATUS_ERROR(ERROR_DISCONNECTED,
                    "Camera device not available");
        default:
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                    "Camera HAL encountered error %d: %s",
                    err, strerror(-err));
    }
}
Status CameraService::makeClient(const sp<CameraService>& cameraService,
        const sp<IInterface>& cameraCb, const std::string& packageName, bool systemNativeClient,
        const std::optional<std::string>& featureId, const std::string& cameraId,
        int api1CameraId, int facing, int sensorOrientation, int clientPid, uid_t clientUid,
        int servicePid, std::pair<int, IPCTransport> deviceVersionAndTransport,
        apiLevel effectiveApiLevel, bool overrideForPerfClass, bool overrideToPortrait,
        bool forceSlowJpegMode, const std::string& originalCameraId,
               sp<BasicClient>* client) {
    if (deviceVersionAndTransport.second == IPCTransport::HIDL) {
        int deviceVersion = deviceVersionAndTransport.first;
        switch(deviceVersion) {
            case CAMERA_DEVICE_API_VERSION_1_0:
                ALOGE("Camera using old HAL version: %d", deviceVersion);
                return STATUS_ERROR_FMT(ERROR_DEPRECATED_HAL,
                        "Camera device \"%s\" HAL version %d no longer supported",
                        cameraId.c_str(), deviceVersion);
                break;
            case CAMERA_DEVICE_API_VERSION_3_0:
            case CAMERA_DEVICE_API_VERSION_3_1:
            case CAMERA_DEVICE_API_VERSION_3_2:
            case CAMERA_DEVICE_API_VERSION_3_3:
            case CAMERA_DEVICE_API_VERSION_3_4:
            case CAMERA_DEVICE_API_VERSION_3_5:
            case CAMERA_DEVICE_API_VERSION_3_6:
            case CAMERA_DEVICE_API_VERSION_3_7:
                break;
            default:
                ALOGE("Unknown camera device HAL version: %d", deviceVersion);
                return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                        "Camera device \"%s\" has unknown HAL version %d",
                        cameraId.c_str(), deviceVersion);
        }
    }
    if (effectiveApiLevel == API_1) {
        sp<ICameraClient> tmp = static_cast<ICameraClient*>(cameraCb.get());
        *client = new Camera2Client(cameraService, tmp, cameraService->mCameraServiceProxyWrapper,
                packageName, featureId, cameraId,
                api1CameraId, facing, sensorOrientation,
                clientPid, clientUid, servicePid, overrideForPerfClass, overrideToPortrait,
                forceSlowJpegMode);
        ALOGI("%s: Camera1 API (legacy), override to portrait %d, forceSlowJpegMode %d",
                __FUNCTION__, overrideToPortrait, forceSlowJpegMode);
    } else {
        sp<hardware::camera2::ICameraDeviceCallbacks> tmp =
                static_cast<hardware::camera2::ICameraDeviceCallbacks*>(cameraCb.get());
        *client = new CameraDeviceClient(cameraService, tmp,
                cameraService->mCameraServiceProxyWrapper, packageName, systemNativeClient,
                featureId, cameraId, facing, sensorOrientation, clientPid, clientUid, servicePid,
                overrideForPerfClass, overrideToPortrait, originalCameraId);
        ALOGI("%s: Camera2 API, override to portrait %d", __FUNCTION__, overrideToPortrait);
    }
    return Status::ok();
}
std::string CameraService::toString(std::set<userid_t> intSet) {
    std::ostringstream s;
    bool first = true;
    for (userid_t i : intSet) {
        if (first) {
            s << std::to_string(i);
            first = false;
        } else {
            s << ", " << std::to_string(i);
        }
    }
    return std::move(s.str());
}
int32_t CameraService::mapToInterface(TorchModeStatus status) {
    int32_t serviceStatus = ICameraServiceListener::TORCH_STATUS_NOT_AVAILABLE;
    switch (status) {
        case TorchModeStatus::NOT_AVAILABLE:
            serviceStatus = ICameraServiceListener::TORCH_STATUS_NOT_AVAILABLE;
            break;
        case TorchModeStatus::AVAILABLE_OFF:
            serviceStatus = ICameraServiceListener::TORCH_STATUS_AVAILABLE_OFF;
            break;
        case TorchModeStatus::AVAILABLE_ON:
            serviceStatus = ICameraServiceListener::TORCH_STATUS_AVAILABLE_ON;
            break;
        default:
            ALOGW("Unknown new flash status: %d", status);
    }
    return serviceStatus;
}
CameraService::StatusInternal CameraService::mapToInternal(CameraDeviceStatus status) {
    StatusInternal serviceStatus = StatusInternal::NOT_PRESENT;
    switch (status) {
        case CameraDeviceStatus::NOT_PRESENT:
            serviceStatus = StatusInternal::NOT_PRESENT;
            break;
        case CameraDeviceStatus::PRESENT:
            serviceStatus = StatusInternal::PRESENT;
            break;
        case CameraDeviceStatus::ENUMERATING:
            serviceStatus = StatusInternal::ENUMERATING;
            break;
        default:
            ALOGW("Unknown new HAL device status: %d", status);
    }
    return serviceStatus;
}
int32_t CameraService::mapToInterface(StatusInternal status) {
    int32_t serviceStatus = ICameraServiceListener::STATUS_NOT_PRESENT;
    switch (status) {
        case StatusInternal::NOT_PRESENT:
            serviceStatus = ICameraServiceListener::STATUS_NOT_PRESENT;
            break;
        case StatusInternal::PRESENT:
            serviceStatus = ICameraServiceListener::STATUS_PRESENT;
            break;
        case StatusInternal::ENUMERATING:
            serviceStatus = ICameraServiceListener::STATUS_ENUMERATING;
            break;
        case StatusInternal::NOT_AVAILABLE:
            serviceStatus = ICameraServiceListener::STATUS_NOT_AVAILABLE;
            break;
        case StatusInternal::UNKNOWN:
            serviceStatus = ICameraServiceListener::STATUS_UNKNOWN;
            break;
        default:
            ALOGW("Unknown new internal device status: %d", status);
    }
    return serviceStatus;
}
Status CameraService::initializeShimMetadata(int cameraId) {
    int uid = CameraThreadState::getCallingUid();
    std::string cameraIdStr = std::to_string(cameraId);
    Status ret = Status::ok();
    sp<Client> tmp = nullptr;
    if (!(ret = connectHelper<ICameraClient,Client>(
            sp<ICameraClient>{nullptr}, cameraIdStr, cameraId,
            kServiceName, false, {}, uid, USE_CALLING_PID,
            API_1, true, 0,
                                 __ANDROID_API_FUTURE__, true,
                                 false, cameraIdStr, tmp)
            ).isOk()) {
        ALOGE("%s: Error initializing shim metadata: %s", __FUNCTION__, ret.toString8().c_str());
    }
    return ret;
}
Status CameraService::getLegacyParametersLazy(int cameraId,
        CameraParameters* parameters) {
    ALOGV("%s: for cameraId: %d", __FUNCTION__, cameraId);
    Status ret = Status::ok();
    if (parameters == NULL) {
        ALOGE("%s: parameters must not be null", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Parameters must not be null");
    }
    std::string unresolvedCameraId = std::to_string(cameraId);
    std::string cameraIdStr = resolveCameraId(unresolvedCameraId,
            CameraThreadState::getCallingUid());
    {
        Mutex::Autolock lock(mServiceLock);
        auto cameraState = getCameraState(cameraIdStr);
        if (cameraState == nullptr) {
            ALOGE("%s: Invalid camera ID: %s", __FUNCTION__, cameraIdStr.c_str());
            return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                    "Invalid camera ID: %s", cameraIdStr.c_str());
        }
        CameraParameters p = cameraState->getShimParams();
        if (!p.isEmpty()) {
            *parameters = p;
            return ret;
        }
    }
    int64_t token = CameraThreadState::clearCallingIdentity();
    ret = initializeShimMetadata(cameraId);
    CameraThreadState::restoreCallingIdentity(token);
    if (!ret.isOk()) {
        return ret;
    }
    {
        Mutex::Autolock lock(mServiceLock);
        auto cameraState = getCameraState(cameraIdStr);
        if (cameraState == nullptr) {
            ALOGE("%s: Invalid camera ID: %s", __FUNCTION__, cameraIdStr.c_str());
            return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                    "Invalid camera ID: %s", cameraIdStr.c_str());
        }
        CameraParameters p = cameraState->getShimParams();
        if (!p.isEmpty()) {
            *parameters = p;
            return ret;
        }
    }
    ALOGE("%s: Parameters were not initialized, or were empty.  Device may not be present.",
            __FUNCTION__);
    return STATUS_ERROR(ERROR_INVALID_OPERATION, "Unable to initialize legacy parameters");
}
<<<<<<< HEAD
||||||| 38b856f7cf
bool CameraService::isTrustedCallingUid(uid_t uid) const {
    return mAttributionAndPermissionUtils->isTrustedCallingUid(uid);
}
status_t CameraService::getUidForPackage(const std::string &packageName, int userId,
                 uid_t& uid, int err) const {
    return mAttributionAndPermissionUtils->getUidForPackage(packageName, userId, uid, err);
}
=======
static bool isTrustedCallingUid(uid_t uid) {
    switch (uid) {
        case AID_MEDIA:
        case AID_CAMERASERVER:
        case AID_RADIO:
            return true;
        default:
            return false;
    }
}
static status_t getUidForPackage(const std::string &packageName, int userId, uid_t& uid,
        int err) {
    PermissionController pc;
    uid = pc.getPackageUid(toString16(packageName), 0);
    if (uid <= 0) {
        ALOGE("Unknown package: '%s'", packageName.c_str());
        dprintf(err, "Unknown package: '%s'\n", packageName.c_str());
        return BAD_VALUE;
    }
    if (userId < 0) {
        ALOGE("Invalid user: %d", userId);
        dprintf(err, "Invalid user: %d\n", userId);
        return BAD_VALUE;
    }
    uid = multiuser_get_uid(userId, uid);
    return NO_ERROR;
}
>>>>>>> 94c719f4
Status CameraService::validateConnectLocked(const std::string& cameraId,
        const std::string& clientName8, int& clientUid, int& clientPid,
               int& originalClientPid) const {
#ifdef __BRILLO__
    UNUSED(clientName8);
    UNUSED(clientUid);
    UNUSED(clientPid);
    UNUSED(originalClientPid);
#else
    Status allowed = validateClientPermissionsLocked(cameraId, clientName8, clientUid, clientPid,
            originalClientPid);
    if (!allowed.isOk()) {
        return allowed;
    }
#endif
    int callingPid = CameraThreadState::getCallingPid();
    if (!mInitialized) {
        ALOGE("CameraService::connect X (PID %d) rejected (camera HAL module not loaded)",
                callingPid);
        return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                "No camera HAL module available to open camera device \"%s\"", cameraId.c_str());
    }
    if (getCameraState(cameraId) == nullptr) {
        ALOGE("CameraService::connect X (PID %d) rejected (invalid camera ID %s)", callingPid,
                cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                "No camera device with ID \"%s\" available", cameraId.c_str());
    }
    status_t err = checkIfDeviceIsUsable(cameraId);
    if (err != NO_ERROR) {
        switch(err) {
            case -ENODEV:
            case -EBUSY:
                return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                        "No camera device with ID \"%s\" currently available", cameraId.c_str());
            default:
                return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                        "Unknown error connecting to ID \"%s\"", cameraId.c_str());
        }
    }
    return Status::ok();
}
Status CameraService::validateClientPermissionsLocked(const std::string& cameraId,
        const std::string& clientName, int& clientUid, int& clientPid,
               int& originalClientPid) const {
    AttributionSourceState attributionSource{};
    int callingPid = CameraThreadState::getCallingPid();
    int callingUid = CameraThreadState::getCallingUid();
    if (clientUid == USE_CALLING_UID) {
        clientUid = callingUid;
    } else if (!isTrustedCallingUid(callingUid)) {
        ALOGE("CameraService::connect X (calling PID %d, calling UID %d) rejected "
                "(don't trust clientUid %d)", callingPid, callingUid, clientUid);
        return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                "Untrusted caller (calling PID %d, UID %d) trying to "
                "forward camera access to camera %s for client %s (PID %d, UID %d)",
                callingPid, callingUid, cameraId.c_str(),
                clientName.c_str(), clientPid, clientUid);
    }
    if (clientPid == USE_CALLING_PID) {
        clientPid = callingPid;
    } else if (!isTrustedCallingUid(callingUid)) {
        ALOGE("CameraService::connect X (calling PID %d, calling UID %d) rejected "
                "(don't trust clientPid %d)", callingPid, callingUid, clientPid);
        return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                "Untrusted caller (calling PID %d, UID %d) trying to "
                "forward camera access to camera %s for client %s (PID %d, UID %d)",
                callingPid, callingUid, cameraId.c_str(),
                clientName.c_str(), clientPid, clientUid);
    }
    if (shouldRejectSystemCameraConnection(cameraId)) {
        ALOGW("Attempting to connect to system-only camera id %s, connection rejected",
                cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_DISCONNECTED, "No camera device with ID \"%s\" is"
                                "available", cameraId.c_str());
    }
    SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
    if (getSystemCameraKind(cameraId, &deviceKind) != OK) {
        ALOGE("%s: Invalid camera id %s, skipping", __FUNCTION__, cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT, "No camera device with ID \"%s\""
                "found while trying to query device kind", cameraId.c_str());
    }
<<<<<<< HEAD
    bool checkPermissionForCamera =
            hasPermissionsForCamera(cameraId, clientPid, clientUid, clientName);
||||||| 38b856f7cf
    bool checkPermissionForCamera = hasPermissionsForCamera(cameraId, clientPid, clientUid);
=======
    attributionSource.pid = clientPid;
    attributionSource.uid = clientUid;
    attributionSource.packageName = clientName;
    bool checkPermissionForCamera = checkPermission(cameraId, sCameraPermission, attributionSource,
            std::string(), AppOpsManager::OP_NONE);
>>>>>>> 94c719f4
    if (callingPid != getpid() &&
                (deviceKind != SystemCameraKind::SYSTEM_ONLY_CAMERA) && !checkPermissionForCamera) {
        ALOGE("Permission Denial: can't use the camera pid=%d, uid=%d", clientPid, clientUid);
        return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                "Caller \"%s\" (PID %d, UID %d) cannot open camera \"%s\" without camera permission",
                clientName.c_str(), clientPid, clientUid, cameraId.c_str());
    }
    if (!mUidPolicy->isUidActive(callingUid, clientName)) {
        int32_t procState = mUidPolicy->getProcState(callingUid);
        ALOGE("Access Denial: can't use the camera from an idle UID pid=%d, uid=%d",
            clientPid, clientUid);
        return STATUS_ERROR_FMT(ERROR_DISABLED,
                "Caller \"%s\" (PID %d, UID %d) cannot open camera \"%s\" from background ("
                "calling UID %d proc state %" PRId32 ")",
                clientName.c_str(), clientPid, clientUid, cameraId.c_str(),
                callingUid, procState);
    }
    if ((!isAutomotivePrivilegedClient(callingUid) ||
            !isAutomotiveExteriorSystemCamera(cameraId)) &&
            mSensorPrivacyPolicy->isSensorPrivacyEnabled()) {
        ALOGE("Access Denial: cannot use the camera when sensor privacy is enabled");
        return STATUS_ERROR_FMT(ERROR_DISABLED,
                "Caller \"%s\" (PID %d, UID %d) cannot open camera \"%s\" when sensor privacy "
                "is enabled", clientName.c_str(), clientPid, clientUid, cameraId.c_str());
    }
    originalClientPid = clientPid;
    clientPid = callingPid;
    userid_t clientUserId = multiuser_get_user_id(clientUid);
    if (!doesClientHaveSystemUid() && callingPid != getpid() &&
            (mAllowedUsers.find(clientUserId) == mAllowedUsers.end())) {
        ALOGE("CameraService::connect X (PID %d) rejected (cannot connect from "
                "device user %d, currently allowed device users: %s)", callingPid, clientUserId,
                toString(mAllowedUsers).c_str());
        return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                "Callers from device user %d are not currently allowed to connect to camera \"%s\"",
                clientUserId, cameraId.c_str());
    }
    if (flags::camera_hsum_permission()) {
<<<<<<< HEAD
        if (isHeadlessSystemUserMode()
                && (clientUserId == USER_SYSTEM)
                && !hasPermissionsForCameraHeadlessSystemUser(cameraId, callingPid, callingUid)) {
||||||| 38b856f7cf
        if (mAttributionAndPermissionUtils->isHeadlessSystemUserMode()
                && (clientUserId == USER_SYSTEM)
                && !hasPermissionsForCameraHeadlessSystemUser(cameraId, callingPid, callingUid)) {
=======
        if (isHeadlessSystemUserMode() && (clientUserId == USER_SYSTEM) &&
                !hasPermissionsForCameraHeadlessSystemUser(cameraId, callingPid, callingUid)) {
>>>>>>> 94c719f4
            ALOGE("Permission Denial: can't use the camera pid=%d, uid=%d", clientPid, clientUid);
            return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                    "Caller \"%s\" (PID %d, UID %d) cannot open camera \"%s\" as Headless System \
                    User without camera headless system user permission",
                    clientName.c_str(), clientPid, clientUid, cameraId.c_str());
        }
    }
    return Status::ok();
}
status_t CameraService::checkIfDeviceIsUsable(const std::string& cameraId) const {
    auto cameraState = getCameraState(cameraId);
    int callingPid = CameraThreadState::getCallingPid();
    if (cameraState == nullptr) {
        ALOGE("CameraService::connect X (PID %d) rejected (invalid camera ID %s)", callingPid,
                cameraId.c_str());
        return -ENODEV;
    }
    StatusInternal currentStatus = cameraState->getStatus();
    if (currentStatus == StatusInternal::NOT_PRESENT) {
        ALOGE("CameraService::connect X (PID %d) rejected (camera %s is not connected)",
                callingPid, cameraId.c_str());
        return -ENODEV;
    } else if (currentStatus == StatusInternal::ENUMERATING) {
        ALOGE("CameraService::connect X (PID %d) rejected, (camera %s is initializing)",
                callingPid, cameraId.c_str());
        return -EBUSY;
    }
    return NO_ERROR;
}
void CameraService::finishConnectLocked(const sp<BasicClient>& client,
        const CameraService::DescriptorPtr& desc, int oomScoreOffset, bool systemNativeClient) {
    auto clientDescriptor =
            CameraService::CameraClientManager::makeClientDescriptor(client, desc,
                    oomScoreOffset, systemNativeClient);
    auto evicted = mActiveClientManager.addAndEvict(clientDescriptor);
    logConnected(desc->getKey(), static_cast<int>(desc->getOwnerId()),
            client->getPackageName());
    if (evicted.size() > 0) {
        for (auto& i : evicted) {
            ALOGE("%s: Invalid state: Client for camera %s was not removed in disconnect",
                    __FUNCTION__, i->getKey().c_str());
        }
        LOG_ALWAYS_FATAL("%s: Invalid state for CameraService, clients not evicted properly",
                __FUNCTION__);
    }
    sp<IBinder> remoteCallback = client->getRemote();
    if (remoteCallback != nullptr) {
        remoteCallback->linkToDeath(this);
    }
}
status_t CameraService::handleEvictionsLocked(const std::string& cameraId, int clientPid,
        apiLevel effectiveApiLevel, const sp<IBinder>& remoteCallback,
        const std::string& packageName, int oomScoreOffset, bool systemNativeClient,
        sp<BasicClient>* client,
        std::shared_ptr<resource_policy::ClientDescriptor<std::string, sp<BasicClient>>>* partial) {
    ATRACE_CALL();
    status_t ret = NO_ERROR;
    std::vector<DescriptorPtr> evictedClients;
    DescriptorPtr clientDescriptor;
    {
        if (effectiveApiLevel == API_1) {
            auto current = mActiveClientManager.get(cameraId);
            if (current != nullptr) {
                auto clientSp = current->getValue();
                if (clientSp.get() != nullptr) {
                    if (!clientSp->canCastToApiClient(effectiveApiLevel)) {
                        ALOGW("CameraService connect called with a different"
                                " API level, evicting prior client...");
                    } else if (clientSp->getRemote() == remoteCallback) {
                        ALOGI("CameraService::connect X (PID %d) (second call from same"
                                " app binder, returning the same client)", clientPid);
                        *client = clientSp;
                        return NO_ERROR;
                    }
                }
            }
        }
        auto state = getCameraState(cameraId);
        if (state == nullptr) {
            ALOGE("CameraService::connect X (PID %d) rejected (no camera device with ID %s)",
                clientPid, cameraId.c_str());
            return BAD_VALUE;
        }
        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder = sm->checkService(String16(kProcessInfoServiceName));
        if (!binder && isAutomotivePrivilegedClient(CameraThreadState::getCallingUid())) {
            clientDescriptor = CameraClientManager::makeClientDescriptor(cameraId,
                    sp<BasicClient>{nullptr}, static_cast<int32_t>(state->getCost()),
                    state->getConflicting(), resource_policy::NATIVE_ADJ, clientPid,
                    ActivityManager::PROCESS_STATE_BOUND_TOP, oomScoreOffset, systemNativeClient);
        } else {
            std::vector<int> ownerPids(mActiveClientManager.getAllOwners());
            ownerPids.push_back(clientPid);
            std::vector<int> priorityScores(ownerPids.size());
            std::vector<int> states(ownerPids.size());
            status_t err = ProcessInfoService::getProcessStatesScoresFromPids(ownerPids.size(),
                    &ownerPids[0], &states[0], &priorityScores[0]);
            if (err != OK) {
                ALOGE("%s: Priority score query failed: %d", __FUNCTION__, err);
                return err;
            }
            std::map<int,resource_policy::ClientPriority> pidToPriorityMap;
            for (size_t i = 0; i < ownerPids.size() - 1; i++) {
                pidToPriorityMap.emplace(ownerPids[i],
                        resource_policy::ClientPriority(priorityScores[i], states[i],
                                                                  false,
                                                                  0));
            }
            mActiveClientManager.updatePriorities(pidToPriorityMap);
            int32_t actualScore = priorityScores[priorityScores.size() - 1];
            int32_t actualState = states[states.size() - 1];
            clientDescriptor = CameraClientManager::makeClientDescriptor(cameraId,
                    sp<BasicClient>{nullptr}, static_cast<int32_t>(state->getCost()),
                    state->getConflicting(), actualScore, clientPid, actualState,
                    oomScoreOffset, systemNativeClient);
        }
        resource_policy::ClientPriority clientPriority = clientDescriptor->getPriority();
        auto evicted = mActiveClientManager.wouldEvict(clientDescriptor);
        if (std::find(evicted.begin(), evicted.end(), clientDescriptor) != evicted.end()) {
            ALOGE("CameraService::connect X (PID %d) rejected (existing client(s) with higher"
                    " priority).", clientPid);
            sp<BasicClient> clientSp = clientDescriptor->getValue();
            std::string curTime = getFormattedCurrentTime();
            auto incompatibleClients =
                    mActiveClientManager.getIncompatibleClients(clientDescriptor);
            std::string msg = fmt::sprintf("%s : DENIED connect device %s client for package %s "
                    "(PID %d, score %d state %d) due to eviction policy", curTime.c_str(),
                    cameraId.c_str(), packageName.c_str(), clientPid,
                    clientPriority.getScore(), clientPriority.getState());
            for (auto& i : incompatibleClients) {
                msg += fmt::sprintf("\n   - Blocked by existing device %s client for package %s"
                        "(PID %" PRId32 ", score %" PRId32 ", state %" PRId32 ")",
                        i->getKey().c_str(),
                        i->getValue()->getPackageName().c_str(),
                        i->getOwnerId(), i->getPriority().getScore(),
                        i->getPriority().getState());
                ALOGE("   Conflicts with: Device %s, client package %s (PID %"
                        PRId32 ", score %" PRId32 ", state %" PRId32 ")", i->getKey().c_str(),
                        i->getValue()->getPackageName().c_str(), i->getOwnerId(),
                        i->getPriority().getScore(), i->getPriority().getState());
            }
            Mutex::Autolock l(mLogLock);
            mEventLog.add(msg);
            auto current = mActiveClientManager.get(cameraId);
            if (current != nullptr) {
                return -EBUSY;
            } else {
                return -EUSERS;
            }
        }
        for (auto& i : evicted) {
            sp<BasicClient> clientSp = i->getValue();
            if (clientSp.get() == nullptr) {
                ALOGE("%s: Invalid state: Null client in active client list.", __FUNCTION__);
                LOG_ALWAYS_FATAL("%s: Invalid state for CameraService, null client in active list",
                        __FUNCTION__);
                mActiveClientManager.remove(i);
                continue;
            }
            ALOGE("CameraService::connect evicting conflicting client for camera ID %s",
                    i->getKey().c_str());
            evictedClients.push_back(i);
            logEvent(fmt::sprintf("EVICT device %s client held by package %s (PID"
                    " %" PRId32 ", score %" PRId32 ", state %" PRId32 ")\n - Evicted by device %s client for"
                    " package %s (PID %d, score %" PRId32 ", state %" PRId32 ")",
                    i->getKey().c_str(), clientSp->getPackageName().c_str(),
                    i->getOwnerId(), i->getPriority().getScore(),
                    i->getPriority().getState(), cameraId.c_str(),
                    packageName.c_str(), clientPid, clientPriority.getScore(),
                    clientPriority.getState()));
            clientSp->notifyError(hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISCONNECTED,
                    CaptureResultExtras());
        }
    }
    mServiceLock.unlock();
    int64_t token = CameraThreadState::clearCallingIdentity();
    for (auto& i : evictedClients) {
        i->getValue()->disconnect();
    }
    CameraThreadState::restoreCallingIdentity(token);
    for (const auto& i : evictedClients) {
        ALOGV("%s: Waiting for disconnect to complete for client for device %s (PID %" PRId32 ")",
                __FUNCTION__, i->getKey().c_str(), i->getOwnerId());
        ret = mActiveClientManager.waitUntilRemoved(i, DEFAULT_DISCONNECT_TIMEOUT_NS);
        if (ret == TIMED_OUT) {
            ALOGE("%s: Timed out waiting for client for device %s to disconnect, "
                    "current clients:\n%s", __FUNCTION__, i->getKey().c_str(),
                    mActiveClientManager.toString().c_str());
            return -EBUSY;
        }
        if (ret != NO_ERROR) {
            ALOGE("%s: Received error waiting for client for device %s to disconnect: %s (%d), "
                    "current clients:\n%s", __FUNCTION__, i->getKey().c_str(), strerror(-ret),
                    ret, mActiveClientManager.toString().c_str());
            return ret;
        }
    }
    evictedClients.clear();
    mServiceLock.lock();
    if ((ret = checkIfDeviceIsUsable(cameraId)) != NO_ERROR) {
        return ret;
    }
    *partial = clientDescriptor;
    return NO_ERROR;
}
Status CameraService::connect(
        const sp<ICameraClient>& cameraClient,
        int api1CameraId,
        const std::string& clientPackageName,
        int clientUid,
        int clientPid,
        int targetSdkVersion,
        bool overrideToPortrait,
        bool forceSlowJpegMode,
        sp<ICamera>* device) {
    ATRACE_CALL();
    Status ret = Status::ok();
    std::string unresolvedCameraId = cameraIdIntToStr(api1CameraId);
    std::string cameraIdStr = resolveCameraId(unresolvedCameraId,
            CameraThreadState::getCallingUid());
    sp<Client> client = nullptr;
    ret = connectHelper<ICameraClient,Client>(cameraClient, cameraIdStr, api1CameraId,
            clientPackageName, false, {}, clientUid, clientPid, API_1,
                               false, 0, targetSdkVersion,
            overrideToPortrait, forceSlowJpegMode, cameraIdStr, client);
    if(!ret.isOk()) {
        logRejected(cameraIdStr, CameraThreadState::getCallingPid(), clientPackageName,
                toStdString(ret.toString8()));
        return ret;
    }
    *device = client;
    const sp<IServiceManager> sm(defaultServiceManager());
    const auto& mActivityManager = getActivityManager();
    if (mActivityManager) {
        mActivityManager->logFgsApiBegin(LOG_FGS_CAMERA_API,
            CameraThreadState::getCallingUid(),
            CameraThreadState::getCallingPid());
    }
    return ret;
}
bool CameraService::shouldSkipStatusUpdates(SystemCameraKind systemCameraKind,
        bool isVendorListener, int clientPid, int clientUid) {
    if (!isVendorListener && (systemCameraKind == SystemCameraKind::HIDDEN_SECURE_CAMERA ||
            (systemCameraKind == SystemCameraKind::SYSTEM_ONLY_CAMERA &&
            !hasPermissionsForSystemCamera(std::string(), clientPid, clientUid)))) {
        return true;
    }
    return false;
}
bool CameraService::shouldRejectSystemCameraConnection(const std::string& cameraId) const {
    int cPid = CameraThreadState::getCallingPid();
    int cUid = CameraThreadState::getCallingUid();
    bool systemClient = doesClientHaveSystemUid();
    SystemCameraKind systemCameraKind = SystemCameraKind::PUBLIC;
    if (getSystemCameraKind(cameraId, &systemCameraKind) != OK) {
        ALOGV("%s: Unknown camera id %s, ", __FUNCTION__, cameraId.c_str());
        return false;
    }
<<<<<<< HEAD
    if (isCallerCameraServerNotDelegating()) {
||||||| 38b856f7cf
    if (mAttributionAndPermissionUtils->isCallerCameraServerNotDelegating()) {
=======
    if (CameraThreadState::getCallingPid() == getpid()) {
>>>>>>> 94c719f4
        return false;
    }
    if (!systemClient && systemCameraKind == SystemCameraKind::HIDDEN_SECURE_CAMERA) {
        ALOGW("Rejecting access to secure hidden camera %s", cameraId.c_str());
        return true;
    }
    if (!systemClient && systemCameraKind == SystemCameraKind::SYSTEM_ONLY_CAMERA &&
            !hasPermissionsForSystemCamera(cameraId, cPid, cUid)) {
        ALOGW("Rejecting access to system only camera %s, inadequete permissions",
                cameraId.c_str());
        return true;
    }
    return false;
}
Status CameraService::connectDevice(
        const sp<hardware::camera2::ICameraDeviceCallbacks>& cameraCb,
        const std::string& unresolvedCameraId,
        const std::string& clientPackageName,
        const std::optional<std::string>& clientFeatureId,
        int clientUid, int oomScoreOffset, int targetSdkVersion,
        bool overrideToPortrait,
        sp<hardware::camera2::ICameraDeviceUser>* device) {
    ATRACE_CALL();
    Status ret = Status::ok();
    sp<CameraDeviceClient> client = nullptr;
    std::string clientPackageNameAdj = clientPackageName;
    int callingPid = CameraThreadState::getCallingPid();
    int callingUid = CameraThreadState::getCallingUid();
    bool systemNativeClient = false;
    if (doesClientHaveSystemUid() && (clientPackageNameAdj.size() == 0)) {
        std::string systemClient = fmt::sprintf("client.pid<%d>", callingPid);
        clientPackageNameAdj = systemClient;
        systemNativeClient = true;
    }
    const std::string cameraId = resolveCameraId(
            unresolvedCameraId,
            callingUid,
            clientPackageNameAdj);
    if (oomScoreOffset < 0) {
        std::string msg =
                fmt::sprintf("Cannot increase the priority of a client %s pid %d for "
                        "camera id %s", clientPackageNameAdj.c_str(), callingPid,
                        cameraId.c_str());
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }
    userid_t clientUserId = multiuser_get_user_id(clientUid);
    if (clientUid == USE_CALLING_UID) {
        clientUserId = multiuser_get_user_id(callingUid);
    }
    if ((!isAutomotivePrivilegedClient(callingUid) || !isAutomotiveExteriorSystemCamera(cameraId))
            && mCameraServiceProxyWrapper->isCameraDisabled(clientUserId)) {
        std::string msg = "Camera disabled by device policy";
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(ERROR_DISABLED, msg.c_str());
    }
    if (oomScoreOffset > 0
            && !hasPermissionsForSystemCamera(cameraId, callingPid,
                    callingUid)
            && !isTrustedCallingUid(callingUid)) {
        std::string msg = fmt::sprintf("Cannot change the priority of a client %s pid %d for "
                        "camera id %s without SYSTEM_CAMERA permissions",
                        clientPackageNameAdj.c_str(), callingPid, cameraId.c_str());
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(ERROR_PERMISSION_DENIED, msg.c_str());
    }
    ret = connectHelper<hardware::camera2::ICameraDeviceCallbacks,CameraDeviceClient>(cameraCb,
            cameraId, -1, clientPackageNameAdj, systemNativeClient, clientFeatureId,
            clientUid, USE_CALLING_PID, API_2, false, oomScoreOffset,
            targetSdkVersion, overrideToPortrait, false, unresolvedCameraId,
                   client);
    if(!ret.isOk()) {
        logRejected(cameraId, callingPid, clientPackageNameAdj, toStdString(ret.toString8()));
        return ret;
    }
    *device = client;
    Mutex::Autolock lock(mServiceLock);
    if ((ftruncate(mMemFd, 0) == -1) || (lseek(mMemFd, 0, SEEK_SET) == -1)) {
        ALOGE("%s: Error while truncating the file: %s", __FUNCTION__, sFileName);
        close(mMemFd);
        mMemFd = memfd_create(sFileName, MFD_ALLOW_SEALING);
        if (mMemFd == -1) {
            ALOGE("%s: Error while creating the file: %s", __FUNCTION__, sFileName);
        }
    }
    const sp<IServiceManager> sm(defaultServiceManager());
    const auto& mActivityManager = getActivityManager();
    if (mActivityManager) {
        mActivityManager->logFgsApiBegin(LOG_FGS_CAMERA_API,
            callingUid,
            callingPid);
    }
    return ret;
}
bool CameraService::isCameraPrivacyEnabled(const String16& packageName, const std::string& cam_id,
        int callingPid, int callingUid) {
    if (!isAutomotiveDevice()) {
        return mSensorPrivacyPolicy->isCameraPrivacyEnabled();
    }
    if ((isAutomotivePrivilegedClient(callingUid) && isAutomotiveExteriorSystemCamera(cam_id))) {
        ALOGI("Camera privacy cannot be enabled for automotive privileged client %d "
                "using camera %s", callingUid, cam_id.c_str());
        return false;
    }
    if (mSensorPrivacyPolicy->isCameraPrivacyEnabled(packageName)) {
        return true;
    } else if (mSensorPrivacyPolicy->getCameraPrivacyState() == SensorPrivacyManager::DISABLED) {
        return false;
    } else if (mSensorPrivacyPolicy->getCameraPrivacyState()
            == SensorPrivacyManager::ENABLED_EXCEPT_ALLOWLISTED_APPS) {
        if (hasPermissionsForCameraPrivacyAllowlist(callingPid, callingUid)) {
            return false;
        } else {
            return true;
        }
    }
    return false;
}
std::string CameraService::getPackageNameFromUid(int clientUid) {
    std::string packageName("");
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(toString16(kPermissionServiceName));
    if (binder == 0) {
        ALOGE("Cannot get permission service");
        return packageName;
    }
    sp<IPermissionController> permCtrl = interface_cast<IPermissionController>(binder);
    Vector<String16> packages;
    permCtrl->getPackagesForUid(clientUid, packages);
    if (packages.isEmpty()) {
        ALOGE("No packages for calling UID %d", clientUid);
        return packageName;
    }
    packageName = toStdString(packages[0]);
    return packageName;
}
template<class CALLBACK, class CLIENT>
Status CameraService::connectHelper(const sp<CALLBACK>& cameraCb, const std::string& cameraId,
        int api1CameraId, const std::string& clientPackageNameMaybe, bool systemNativeClient,
        const std::optional<std::string>& clientFeatureId, int clientUid, int clientPid,
        apiLevel effectiveApiLevel, bool shimUpdateOnly, int oomScoreOffset, int targetSdkVersion,
        bool overrideToPortrait, bool forceSlowJpegMode, const std::string& originalCameraId,
               sp<CLIENT>& device) {
    binder::Status ret = binder::Status::ok();
    bool isNonSystemNdk = false;
    std::string clientPackageName;
    int packageUid = (clientUid == USE_CALLING_UID) ?
            CameraThreadState::getCallingUid() : clientUid;
    if (clientPackageNameMaybe.size() <= 0) {
        isNonSystemNdk = true;
        clientPackageName = getPackageNameFromUid(packageUid);
    } else {
        clientPackageName = clientPackageNameMaybe;
    }
    int originalClientPid = 0;
    int packagePid = (clientPid == USE_CALLING_PID) ?
        CameraThreadState::getCallingPid() : clientPid;
    ALOGI("CameraService::connect call (PID %d \"%s\", camera ID %s) and "
            "Camera API version %d", packagePid, clientPackageName.c_str(), cameraId.c_str(),
            static_cast<int>(effectiveApiLevel));
    nsecs_t openTimeNs = systemTime();
    sp<CLIENT> client = nullptr;
    int facing = -1;
    int orientation = 0;
    {
        std::unique_ptr<AutoConditionLock> lock =
                AutoConditionLock::waitAndAcquire(mServiceLockWrapper, DEFAULT_CONNECT_TIMEOUT_NS);
        if (lock == nullptr) {
            ALOGE("CameraService::connect (PID %d) rejected (too many other clients connecting)."
                    , clientPid);
            return STATUS_ERROR_FMT(ERROR_MAX_CAMERAS_IN_USE,
                    "Cannot open camera %s for \"%s\" (PID %d): Too many other clients connecting",
                    cameraId.c_str(), clientPackageName.c_str(), clientPid);
        }
        if(!(ret = validateConnectLocked(cameraId, clientPackageName,
                         clientUid, clientPid, originalClientPid)).isOk()) {
            return ret;
        }
        if (shimUpdateOnly) {
            auto cameraState = getCameraState(cameraId);
            if (cameraState != nullptr) {
                if (!cameraState->getShimParams().isEmpty()) return ret;
            }
        }
        status_t err;
        sp<BasicClient> clientTmp = nullptr;
        std::shared_ptr<resource_policy::ClientDescriptor<std::string, sp<BasicClient>>> partial;
        if ((err = handleEvictionsLocked(cameraId, originalClientPid, effectiveApiLevel,
                IInterface::asBinder(cameraCb), clientPackageName, oomScoreOffset,
                systemNativeClient, &clientTmp, &partial)) != NO_ERROR) {
            switch (err) {
                case -ENODEV:
                    return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                            "No camera device with ID \"%s\" currently available",
                            cameraId.c_str());
                case -EBUSY:
                    return STATUS_ERROR_FMT(ERROR_CAMERA_IN_USE,
                            "Higher-priority client using camera, ID \"%s\" currently unavailable",
                            cameraId.c_str());
                case -EUSERS:
                    return STATUS_ERROR_FMT(ERROR_MAX_CAMERAS_IN_USE,
                            "Too many cameras already open, cannot open camera \"%s\"",
                            cameraId.c_str());
                default:
                    return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                            "Unexpected error %s (%d) opening camera \"%s\"",
                            strerror(-err), err, cameraId.c_str());
            }
        }
        if (clientTmp.get() != nullptr) {
            device = static_cast<CLIENT*>(clientTmp.get());
            return ret;
        }
        mFlashlight->prepareDeviceOpen(cameraId);
        int portraitRotation;
        auto deviceVersionAndTransport =
                getDeviceVersion(cameraId, overrideToPortrait, &portraitRotation,
                               &facing, &orientation);
        if (facing == -1) {
            ALOGE("%s: Unable to get camera device \"%s\"  facing", __FUNCTION__, cameraId.c_str());
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                    "Unable to get camera device \"%s\" facing", cameraId.c_str());
        }
        sp<BasicClient> tmp = nullptr;
        bool overrideForPerfClass = SessionConfigurationUtils::targetPerfClassPrimaryCamera(
                mPerfClassPrimaryCameraIds, cameraId, targetSdkVersion);
        if(!(ret = makeClient(this, cameraCb, clientPackageName, systemNativeClient,
                clientFeatureId, cameraId, api1CameraId, facing,
                orientation, clientPid, clientUid, getpid(),
                deviceVersionAndTransport, effectiveApiLevel, overrideForPerfClass,
                overrideToPortrait, forceSlowJpegMode, originalCameraId,
                       &tmp)).isOk()) {
            return ret;
        }
        client = static_cast<CLIENT*>(tmp.get());
        LOG_ALWAYS_FATAL_IF(client.get() == nullptr, "%s: CameraService in invalid state",
                __FUNCTION__);
        std::string monitorTags = isClientWatched(client.get()) ? mMonitorTags : std::string();
        err = client->initialize(mCameraProviderManager, monitorTags);
        if (err != OK) {
            ALOGE("%s: Could not initialize client from HAL.", __FUNCTION__);
            mServiceLock.unlock();
            client->disconnect();
            mServiceLock.lock();
            switch(err) {
                case BAD_VALUE:
                    return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                            "Illegal argument to HAL module for camera \"%s\"", cameraId.c_str());
                case -EBUSY:
                    return STATUS_ERROR_FMT(ERROR_CAMERA_IN_USE,
                            "Camera \"%s\" is already open", cameraId.c_str());
                case -EUSERS:
                    return STATUS_ERROR_FMT(ERROR_MAX_CAMERAS_IN_USE,
                            "Too many cameras already open, cannot open camera \"%s\"",
                            cameraId.c_str());
                case PERMISSION_DENIED:
                    return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                            "No permission to open camera \"%s\"", cameraId.c_str());
                case -EACCES:
                    return STATUS_ERROR_FMT(ERROR_DISABLED,
                            "Camera \"%s\" disabled by policy", cameraId.c_str());
                case -ENODEV:
                default:
                    return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                            "Failed to initialize camera \"%s\": %s (%d)", cameraId.c_str(),
                            strerror(-err), err);
            }
        }
        if (effectiveApiLevel == API_1) {
            sp<Client> shimClient = reinterpret_cast<Client*>(client.get());
            String8 rawParams = shimClient->getParameters();
            CameraParameters params(rawParams);
            auto cameraState = getCameraState(cameraId);
            if (cameraState != nullptr) {
                cameraState->setShimParams(params);
            } else {
                ALOGE("%s: Cannot update shim parameters for camera %s, no such device exists.",
                        __FUNCTION__, cameraId.c_str());
            }
        }
        client->setCameraServiceWatchdog(mCameraServiceWatchdogEnabled);
        CameraMetadata chars;
        bool rotateAndCropSupported = true;
        err = mCameraProviderManager->getCameraCharacteristics(cameraId, overrideForPerfClass,
                &chars, overrideToPortrait);
        if (err == OK) {
            auto availableRotateCropEntry = chars.find(
                    ANDROID_SCALER_AVAILABLE_ROTATE_AND_CROP_MODES);
            if (availableRotateCropEntry.count <= 1) {
                rotateAndCropSupported = false;
            }
        } else {
            ALOGE("%s: Unable to query static metadata for camera %s: %s (%d)", __FUNCTION__,
                    cameraId.c_str(), strerror(-err), err);
        }
        if (rotateAndCropSupported) {
            if (mOverrideRotateAndCropMode != ANDROID_SCALER_ROTATE_AND_CROP_AUTO) {
                client->setRotateAndCropOverride(mOverrideRotateAndCropMode);
            } else if (overrideToPortrait && portraitRotation != 0) {
                uint8_t rotateAndCropMode = ANDROID_SCALER_ROTATE_AND_CROP_AUTO;
                switch (portraitRotation) {
                    case 90:
                        rotateAndCropMode = ANDROID_SCALER_ROTATE_AND_CROP_90;
                        break;
                    case 180:
                        rotateAndCropMode = ANDROID_SCALER_ROTATE_AND_CROP_180;
                        break;
                    case 270:
                        rotateAndCropMode = ANDROID_SCALER_ROTATE_AND_CROP_270;
                        break;
                    default:
                        ALOGE("Unexpected portrait rotation: %d", portraitRotation);
                        break;
                }
                client->setRotateAndCropOverride(rotateAndCropMode);
            } else {
                client->setRotateAndCropOverride(
                    mCameraServiceProxyWrapper->getRotateAndCropOverride(
                        clientPackageName, facing, multiuser_get_user_id(clientUid)));
            }
        }
        bool autoframingSupported = true;
        auto availableAutoframingEntry = chars.find(ANDROID_CONTROL_AUTOFRAMING_AVAILABLE);
        if ((availableAutoframingEntry.count == 1) && (availableAutoframingEntry.data.u8[0] ==
                    ANDROID_CONTROL_AUTOFRAMING_AVAILABLE_FALSE)) {
            autoframingSupported = false;
        }
        if (autoframingSupported) {
            if (mOverrideAutoframingMode != ANDROID_CONTROL_AUTOFRAMING_AUTO) {
                client->setAutoframingOverride(mOverrideAutoframingMode);
            } else {
                client->setAutoframingOverride(
                    mCameraServiceProxyWrapper->getAutoframingOverride(
                        clientPackageName));
            }
        }
        bool isCameraPrivacyEnabled;
        if (flags::camera_privacy_allowlist()) {
            isCameraPrivacyEnabled = this->isCameraPrivacyEnabled(
                    toString16(client->getPackageName()), cameraId, packagePid, packageUid);
        } else {
            isCameraPrivacyEnabled =
                    mSensorPrivacyPolicy->isCameraPrivacyEnabled();
        }
        if (client->supportsCameraMute()) {
            client->setCameraMute(
                    mOverrideCameraMuteMode || isCameraPrivacyEnabled);
        } else if (isCameraPrivacyEnabled) {
            ALOGI("Camera mute not supported for package: %s, camera id: %s",
                    client->getPackageName().c_str(), cameraId.c_str());
            mServiceLock.unlock();
            int64_t token = CameraThreadState::clearCallingIdentity();
            client->noteAppOp();
            client->disconnect();
            CameraThreadState::restoreCallingIdentity(token);
            mServiceLock.lock();
            return STATUS_ERROR_FMT(ERROR_DISABLED,
                    "Camera \"%s\" disabled due to camera mute", cameraId.c_str());
        }
        if (shimUpdateOnly) {
            mServiceLock.unlock();
            client->disconnect();
            mServiceLock.lock();
        } else {
            finishConnectLocked(client, partial, oomScoreOffset, systemNativeClient);
        }
        client->setImageDumpMask(mImageDumpMask);
        client->setStreamUseCaseOverrides(mStreamUseCaseOverrides);
        client->setZoomOverride(mZoomOverrideValue);
    }
    device = client;
    int32_t openLatencyMs = ns2ms(systemTime() - openTimeNs);
    mCameraServiceProxyWrapper->logOpen(cameraId, facing, clientPackageName,
            effectiveApiLevel, isNonSystemNdk, openLatencyMs);
    {
        Mutex::Autolock lock(mInjectionParametersLock);
        if (cameraId == mInjectionInternalCamId && mInjectionInitPending) {
            mInjectionInitPending = false;
            status_t res = NO_ERROR;
            auto clientDescriptor = mActiveClientManager.get(mInjectionInternalCamId);
            if (clientDescriptor != nullptr) {
                sp<BasicClient> clientSp = clientDescriptor->getValue();
                res = checkIfInjectionCameraIsPresent(mInjectionExternalCamId, clientSp);
                if(res != OK) {
                    return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                            "No camera device with ID \"%s\" currently available",
                            mInjectionExternalCamId.c_str());
                }
                res = clientSp->injectCamera(mInjectionExternalCamId, mCameraProviderManager);
                if (res != OK) {
                    mInjectionStatusListener->notifyInjectionError(mInjectionExternalCamId, res);
                }
            } else {
                ALOGE("%s: Internal camera ID = %s 's client does not exist!",
                        __FUNCTION__, mInjectionInternalCamId.c_str());
                res = NO_INIT;
                mInjectionStatusListener->notifyInjectionError(mInjectionExternalCamId, res);
            }
        }
    }
    return ret;
}
status_t CameraService::addOfflineClient(const std::string &cameraId,
        sp<BasicClient> offlineClient) {
    if (offlineClient.get() == nullptr) {
        return BAD_VALUE;
    }
    {
        std::unique_ptr<AutoConditionLock> lock =
                AutoConditionLock::waitAndAcquire(mServiceLockWrapper, DEFAULT_CONNECT_TIMEOUT_NS);
        if (lock == nullptr) {
            ALOGE("%s: (PID %d) rejected (too many other clients connecting)."
                    , __FUNCTION__, offlineClient->getClientPid());
            return TIMED_OUT;
        }
        auto onlineClientDesc = mActiveClientManager.get(cameraId);
        if (onlineClientDesc.get() == nullptr) {
            ALOGE("%s: No active online client using camera id: %s", __FUNCTION__,
                    cameraId.c_str());
            return BAD_VALUE;
        }
        const auto& onlinePriority = onlineClientDesc->getPriority();
        auto offlineClientDesc = CameraClientManager::makeClientDescriptor(
                kOfflineDevice + onlineClientDesc->getKey(), offlineClient, 0,
                                    std::set<std::string>(), onlinePriority.getScore(),
                onlineClientDesc->getOwnerId(), onlinePriority.getState(),
                                   0, false);
        if (offlineClientDesc == nullptr) {
            ALOGE("%s: Offline client descriptor was NULL", __FUNCTION__);
            return BAD_VALUE;
        }
        auto incompatibleClients = mActiveClientManager.getIncompatibleClients(offlineClientDesc);
        if (!incompatibleClients.empty()) {
            ALOGE("%s: Incompatible offline clients present!", __FUNCTION__);
            return BAD_VALUE;
        }
        std::string monitorTags = isClientWatched(offlineClient.get())
                ? mMonitorTags : std::string();
        auto err = offlineClient->initialize(mCameraProviderManager, monitorTags);
        if (err != OK) {
            ALOGE("%s: Could not initialize offline client.", __FUNCTION__);
            return err;
        }
        auto evicted = mActiveClientManager.addAndEvict(offlineClientDesc);
        if (evicted.size() > 0) {
            for (auto& i : evicted) {
                ALOGE("%s: Invalid state: Offline client for camera %s was not removed ",
                        __FUNCTION__, i->getKey().c_str());
            }
            LOG_ALWAYS_FATAL("%s: Invalid state for CameraService, offline clients not evicted "
                    "properly", __FUNCTION__);
            return BAD_VALUE;
        }
        logConnectedOffline(offlineClientDesc->getKey(),
                static_cast<int>(offlineClientDesc->getOwnerId()),
                offlineClient->getPackageName());
        sp<IBinder> remoteCallback = offlineClient->getRemote();
        if (remoteCallback != nullptr) {
            remoteCallback->linkToDeath(this);
        }
    }
    return OK;
}
Status CameraService::turnOnTorchWithStrengthLevel(const std::string& unresolvedCameraId,
        int32_t torchStrength, const sp<IBinder>& clientBinder) {
    Mutex::Autolock lock(mServiceLock);
    ATRACE_CALL();
    if (clientBinder == nullptr) {
        ALOGE("%s: torch client binder is NULL", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT,
                "Torch client binder in null.");
    }
    int uid = CameraThreadState::getCallingUid();
    const std::string cameraId = resolveCameraId(unresolvedCameraId, uid);
    if (shouldRejectSystemCameraConnection(cameraId)) {
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT, "Unable to change the strength level"
                "for system only device %s: ", cameraId.c_str());
    }
    auto state = getCameraState(cameraId);
    if (state == nullptr) {
        ALOGE("%s: camera id is invalid %s", __FUNCTION__, cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
            "Camera ID \"%s\" is a not valid camera ID", cameraId.c_str());
    }
    StatusInternal cameraStatus = state->getStatus();
    if (cameraStatus != StatusInternal::NOT_AVAILABLE &&
            cameraStatus != StatusInternal::PRESENT) {
        ALOGE("%s: camera id is invalid %s, status %d", __FUNCTION__, cameraId.c_str(),
            (int)cameraStatus);
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                "Camera ID \"%s\" is a not valid camera ID", cameraId.c_str());
    }
    {
        Mutex::Autolock al(mTorchStatusMutex);
        TorchModeStatus status;
        status_t err = getTorchStatusLocked(cameraId, &status);
        if (err != OK) {
            if (err == NAME_NOT_FOUND) {
             return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                    "Camera \"%s\" does not have a flash unit", cameraId.c_str());
            }
            ALOGE("%s: getting current torch status failed for camera %s",
                    __FUNCTION__, cameraId.c_str());
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                    "Error changing torch strength level for camera \"%s\": %s (%d)",
                    cameraId.c_str(), strerror(-err), err);
        }
        if (status == TorchModeStatus::NOT_AVAILABLE) {
            if (cameraStatus == StatusInternal::NOT_AVAILABLE) {
                ALOGE("%s: torch mode of camera %s is not available because "
                        "camera is in use.", __FUNCTION__, cameraId.c_str());
                return STATUS_ERROR_FMT(ERROR_CAMERA_IN_USE,
                        "Torch for camera \"%s\" is not available due to an existing camera user",
                        cameraId.c_str());
            } else {
                ALOGE("%s: torch mode of camera %s is not available due to "
                       "insufficient resources", __FUNCTION__, cameraId.c_str());
                return STATUS_ERROR_FMT(ERROR_MAX_CAMERAS_IN_USE,
                        "Torch for camera \"%s\" is not available due to insufficient resources",
                        cameraId.c_str());
            }
        }
    }
    {
        Mutex::Autolock al(mTorchUidMapMutex);
        updateTorchUidMapLocked(cameraId, uid);
    }
    bool shouldSkipTorchStrengthUpdates = mCameraProviderManager->shouldSkipTorchStrengthUpdate(
            cameraId, torchStrength);
    status_t err = mFlashlight->turnOnTorchWithStrengthLevel(cameraId, torchStrength);
    if (err != OK) {
        int32_t errorCode;
        std::string msg;
        switch (err) {
            case -ENOSYS:
                msg = fmt::sprintf("Camera \"%s\" has no flashlight.",
                    cameraId.c_str());
                errorCode = ERROR_ILLEGAL_ARGUMENT;
                break;
            case -EBUSY:
                msg = fmt::sprintf("Camera \"%s\" is in use",
                    cameraId.c_str());
                errorCode = ERROR_CAMERA_IN_USE;
                break;
            case -EINVAL:
                msg = fmt::sprintf("Torch strength level %d is not within the "
                        "valid range.", torchStrength);
                errorCode = ERROR_ILLEGAL_ARGUMENT;
                break;
            default:
                msg = "Changing torch strength level failed.";
                errorCode = ERROR_INVALID_OPERATION;
        }
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(errorCode, msg.c_str());
    }
    {
        Mutex::Autolock al(mTorchClientMapMutex);
        ssize_t index = mTorchClientMap.indexOfKey(cameraId);
        if (index == NAME_NOT_FOUND) {
            mTorchClientMap.add(cameraId, clientBinder);
        } else {
            mTorchClientMap.valueAt(index)->unlinkToDeath(this);
            mTorchClientMap.replaceValueAt(index, clientBinder);
        }
        clientBinder->linkToDeath(this);
    }
    int clientPid = CameraThreadState::getCallingPid();
    ALOGI("%s: Torch strength for camera id %s changed to %d for client PID %d",
            __FUNCTION__, cameraId.c_str(), torchStrength, clientPid);
    if (!shouldSkipTorchStrengthUpdates) {
        broadcastTorchStrengthLevel(cameraId, torchStrength);
    }
    return Status::ok();
}
Status CameraService::setTorchMode(const std::string& unresolvedCameraId, bool enabled,
        const sp<IBinder>& clientBinder) {
    Mutex::Autolock lock(mServiceLock);
    ATRACE_CALL();
    if (enabled && clientBinder == nullptr) {
        ALOGE("%s: torch client binder is NULL", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT,
                "Torch client Binder is null");
    }
    int uid = CameraThreadState::getCallingUid();
    const std::string cameraId = resolveCameraId(unresolvedCameraId, uid);
    if (shouldRejectSystemCameraConnection(cameraId)) {
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT, "Unable to set torch mode"
                " for system only device %s: ", cameraId.c_str());
    }
    auto state = getCameraState(cameraId);
    if (state == nullptr) {
        ALOGE("%s: camera id is invalid %s", __FUNCTION__, cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                "Camera ID \"%s\" is a not valid camera ID", cameraId.c_str());
    }
    StatusInternal cameraStatus = state->getStatus();
    if (cameraStatus != StatusInternal::PRESENT &&
            cameraStatus != StatusInternal::NOT_AVAILABLE) {
        ALOGE("%s: camera id is invalid %s, status %d", __FUNCTION__, cameraId.c_str(),
                (int)cameraStatus);
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                "Camera ID \"%s\" is a not valid camera ID", cameraId.c_str());
    }
    {
        Mutex::Autolock al(mTorchStatusMutex);
        TorchModeStatus status;
        status_t err = getTorchStatusLocked(cameraId, &status);
        if (err != OK) {
            if (err == NAME_NOT_FOUND) {
                return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                        "Camera \"%s\" does not have a flash unit", cameraId.c_str());
            }
            ALOGE("%s: getting current torch status failed for camera %s",
                    __FUNCTION__, cameraId.c_str());
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                    "Error updating torch status for camera \"%s\": %s (%d)", cameraId.c_str(),
                    strerror(-err), err);
        }
        if (status == TorchModeStatus::NOT_AVAILABLE) {
            if (cameraStatus == StatusInternal::NOT_AVAILABLE) {
                ALOGE("%s: torch mode of camera %s is not available because "
                        "camera is in use", __FUNCTION__, cameraId.c_str());
                return STATUS_ERROR_FMT(ERROR_CAMERA_IN_USE,
                        "Torch for camera \"%s\" is not available due to an existing camera user",
                        cameraId.c_str());
            } else {
                ALOGE("%s: torch mode of camera %s is not available due to "
                        "insufficient resources", __FUNCTION__, cameraId.c_str());
                return STATUS_ERROR_FMT(ERROR_MAX_CAMERAS_IN_USE,
                        "Torch for camera \"%s\" is not available due to insufficient resources",
                        cameraId.c_str());
            }
        }
    }
    {
        Mutex::Autolock al(mTorchUidMapMutex);
        updateTorchUidMapLocked(cameraId, uid);
    }
    status_t err = mFlashlight->setTorchMode(cameraId, enabled);
    if (err != OK) {
        int32_t errorCode;
        std::string msg;
        switch (err) {
            case -ENOSYS:
                msg = fmt::sprintf("Camera \"%s\" has no flashlight",
                    cameraId.c_str());
                errorCode = ERROR_ILLEGAL_ARGUMENT;
                break;
            case -EBUSY:
                msg = fmt::sprintf("Camera \"%s\" is in use",
                    cameraId.c_str());
                errorCode = ERROR_CAMERA_IN_USE;
                break;
            default:
                msg = fmt::sprintf(
                    "Setting torch mode of camera \"%s\" to %d failed: %s (%d)",
                    cameraId.c_str(), enabled, strerror(-err), err);
                errorCode = ERROR_INVALID_OPERATION;
        }
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        logServiceError(msg, errorCode);
        return STATUS_ERROR(errorCode, msg.c_str());
    }
    {
        Mutex::Autolock al(mTorchClientMapMutex);
        ssize_t index = mTorchClientMap.indexOfKey(cameraId);
        if (enabled) {
            if (index == NAME_NOT_FOUND) {
                mTorchClientMap.add(cameraId, clientBinder);
            } else {
                mTorchClientMap.valueAt(index)->unlinkToDeath(this);
                mTorchClientMap.replaceValueAt(index, clientBinder);
            }
            clientBinder->linkToDeath(this);
        } else if (index != NAME_NOT_FOUND) {
            mTorchClientMap.valueAt(index)->unlinkToDeath(this);
        }
    }
    int clientPid = CameraThreadState::getCallingPid();
    std::string torchState = enabled ? "on" : "off";
    ALOGI("Torch for camera id %s turned %s for client PID %d", cameraId.c_str(),
            torchState.c_str(), clientPid);
    logTorchEvent(cameraId, torchState, clientPid);
    return Status::ok();
}
void CameraService::updateTorchUidMapLocked(const std::string& cameraId, int uid) {
    if (mTorchUidMap.find(cameraId) == mTorchUidMap.end()) {
        mTorchUidMap[cameraId].first = uid;
        mTorchUidMap[cameraId].second = uid;
    } else {
        mTorchUidMap[cameraId].first = uid;
    }
}
Status CameraService::notifySystemEvent(int32_t eventId,
        const std::vector<int32_t>& args) {
    const int pid = CameraThreadState::getCallingPid();
    const int selfPid = getpid();
    if (pid != selfPid) {
        if (!checkCallingPermission(toString16(sCameraSendSystemEventsPermission))) {
            const int uid = CameraThreadState::getCallingUid();
            ALOGE("Permission Denial: cannot send updates to camera service about system"
                    " events from pid=%d, uid=%d", pid, uid);
            return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                    "No permission to send updates to camera service about system events"
                    " from pid=%d, uid=%d", pid, uid);
        }
    }
    ATRACE_CALL();
    switch(eventId) {
        case ICameraService::EVENT_USER_SWITCHED: {
            mUidPolicy->registerSelf();
            mSensorPrivacyPolicy->registerSelf();
            doUserSwitch( args);
            break;
        }
        case ICameraService::EVENT_USB_DEVICE_ATTACHED:
        case ICameraService::EVENT_USB_DEVICE_DETACHED: {
            if (args.size() != 1) {
                return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT,
                    "USB Device Event requires 1 argument");
            }
            mCameraProviderManager->notifyUsbDeviceEvent(eventId,
                                                        std::to_string(args[0]));
            break;
        }
        case ICameraService::EVENT_NONE:
        default: {
            ALOGW("%s: Received invalid system event from system_server: %d", __FUNCTION__,
                    eventId);
            break;
        }
    }
    return Status::ok();
}
void CameraService::notifyMonitoredUids() {
    Mutex::Autolock lock(mStatusListenerLock);
    for (const auto& it : mListenerList) {
        auto ret = it->getListener()->onCameraAccessPrioritiesChanged();
        it->handleBinderStatus(ret, "%s: Failed to trigger permission callback for %d:%d: %d",
                __FUNCTION__, it->getListenerUid(), it->getListenerPid(), ret.exceptionCode());
    }
}
void CameraService::notifyMonitoredUids(const std::unordered_set<uid_t> &notifyUidSet) {
    Mutex::Autolock lock(mStatusListenerLock);
    for (const auto& it : mListenerList) {
        if (notifyUidSet.find(it->getListenerUid()) != notifyUidSet.end()) {
            ALOGV("%s: notifying uid %d", __FUNCTION__, it->getListenerUid());
            auto ret = it->getListener()->onCameraAccessPrioritiesChanged();
            it->handleBinderStatus(ret, "%s: Failed to trigger permission callback for %d:%d: %d",
                    __FUNCTION__, it->getListenerUid(), it->getListenerPid(), ret.exceptionCode());
        }
    }
}
Status CameraService::notifyDeviceStateChange(int64_t newState) {
    const int pid = CameraThreadState::getCallingPid();
    const int selfPid = getpid();
    if (pid != selfPid) {
        if (!checkCallingPermission(toString16(sCameraSendSystemEventsPermission))) {
            const int uid = CameraThreadState::getCallingUid();
            ALOGE("Permission Denial: cannot send updates to camera service about device"
                    " state changes from pid=%d, uid=%d", pid, uid);
            return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                    "No permission to send updates to camera service about device state"
                    " changes from pid=%d, uid=%d", pid, uid);
        }
    }
    ATRACE_CALL();
    {
        Mutex::Autolock lock(mServiceLock);
        mDeviceState = newState;
    }
    mCameraProviderManager->notifyDeviceStateChange(newState);
    return Status::ok();
}
Status CameraService::notifyDisplayConfigurationChange() {
    ATRACE_CALL();
    const int callingPid = CameraThreadState::getCallingPid();
    const int selfPid = getpid();
    if (callingPid != selfPid) {
        if (!checkCallingPermission(toString16(sCameraSendSystemEventsPermission))) {
            const int uid = CameraThreadState::getCallingUid();
            ALOGE("Permission Denial: cannot send updates to camera service about orientation"
                    " changes from pid=%d, uid=%d", callingPid, uid);
            return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                    "No permission to send updates to camera service about orientation"
                    " changes from pid=%d, uid=%d", callingPid, uid);
        }
    }
    Mutex::Autolock lock(mServiceLock);
    if (mOverrideRotateAndCropMode != ANDROID_SCALER_ROTATE_AND_CROP_AUTO) return Status::ok();
    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr && !basicClient->getOverrideToPortrait()) {
                basicClient->setRotateAndCropOverride(
                        mCameraServiceProxyWrapper->getRotateAndCropOverride(
                                basicClient->getPackageName(),
                                basicClient->getCameraFacing(),
                                multiuser_get_user_id(basicClient->getClientUid())));
            }
        }
    }
    return Status::ok();
}
Status CameraService::getConcurrentCameraIds(
        std::vector<ConcurrentCameraIdCombination>* concurrentCameraIds) {
    ATRACE_CALL();
    if (!concurrentCameraIds) {
        ALOGE("%s: concurrentCameraIds is NULL", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "concurrentCameraIds is NULL");
    }
    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        logServiceError("Camera subsystem is not available", ERROR_DISCONNECTED);
        return STATUS_ERROR(ERROR_DISCONNECTED,
                "Camera subsystem is not available");
    }
    std::vector<std::unordered_set<std::string>> concurrentCameraCombinations =
            mCameraProviderManager->getConcurrentCameraIds();
    for (auto &combination : concurrentCameraCombinations) {
        std::vector<std::string> validCombination;
        for (auto &cameraId : combination) {
            auto state = getCameraState(cameraId);
            if (state == nullptr) {
                ALOGW("%s: camera id %s does not exist", __FUNCTION__, cameraId.c_str());
                continue;
            }
            StatusInternal status = state->getStatus();
            if (status == StatusInternal::NOT_PRESENT || status == StatusInternal::ENUMERATING) {
                continue;
            }
            if (shouldRejectSystemCameraConnection(cameraId)) {
                continue;
            }
            validCombination.push_back(cameraId);
        }
        if (validCombination.size() != 0) {
            concurrentCameraIds->push_back(std::move(validCombination));
        }
    }
    return Status::ok();
}
bool CameraService::hasCameraPermissions() const {
    int callingPid = CameraThreadState::getCallingPid();
    int callingUid = CameraThreadState::getCallingUid();
    AttributionSourceState attributionSource{};
    attributionSource.pid = callingPid;
    attributionSource.uid = callingUid;
    bool res = checkPermission(std::string(), sCameraPermission,
            attributionSource, std::string(), AppOpsManager::OP_NONE);
    bool hasPermission = ((callingPid == getpid()) || res);
    if (!hasPermission) {
        ALOGE("%s: pid %d doesn't have camera permissions", __FUNCTION__, callingPid);
    }
    return hasPermission;
}
Status CameraService::isConcurrentSessionConfigurationSupported(
        const std::vector<CameraIdAndSessionConfiguration>& cameraIdsAndSessionConfigurations,
        int targetSdkVersion, bool* isSupported) {
    if (!isSupported) {
        ALOGE("%s: isSupported is NULL", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "isSupported is NULL");
    }
    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        return STATUS_ERROR(ERROR_DISCONNECTED,
                "Camera subsystem is not available");
    }
    if (!hasCameraPermissions()) {
        return STATUS_ERROR(ERROR_PERMISSION_DENIED,
                "android.permission.CAMERA needed to call"
                "isConcurrentSessionConfigurationSupported");
    }
    status_t res =
            mCameraProviderManager->isConcurrentSessionConfigurationSupported(
                    cameraIdsAndSessionConfigurations, mPerfClassPrimaryCameraIds,
                    targetSdkVersion, isSupported);
    if (res != OK) {
        logServiceError("Unable to query session configuration support",
            ERROR_INVALID_OPERATION);
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to query session configuration "
                "support %s (%d)", strerror(-res), res);
    }
    return Status::ok();
}
Status CameraService::addListener(const sp<ICameraServiceListener>& listener,
        std::vector<hardware::CameraStatus> *cameraStatuses) {
    return addListenerHelper(listener, cameraStatuses);
}
binder::Status CameraService::addListenerTest(const sp<hardware::ICameraServiceListener>& listener,
            std::vector<hardware::CameraStatus>* cameraStatuses) {
    return addListenerHelper(listener, cameraStatuses, false, true);
}
Status CameraService::addListenerHelper(const sp<ICameraServiceListener>& listener,
        std::vector<hardware::CameraStatus> *cameraStatuses,
        bool isVendorListener, bool isProcessLocalTest) {
    ATRACE_CALL();
    ALOGV("%s: Add listener %p", __FUNCTION__, listener.get());
    if (listener == nullptr) {
        ALOGE("%s: Listener must not be null", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Null listener given to addListener");
    }
    auto clientUid = CameraThreadState::getCallingUid();
    auto clientPid = CameraThreadState::getCallingPid();
    AttributionSourceState attributionSource{};
    attributionSource.uid = clientUid;
    attributionSource.pid = clientPid;
   bool openCloseCallbackAllowed = checkPermission(std::string(),
            sCameraOpenCloseListenerPermission, attributionSource, std::string(),
            AppOpsManager::OP_NONE);
    Mutex::Autolock lock(mServiceLock);
    {
        Mutex::Autolock lock(mStatusListenerLock);
        for (const auto &it : mListenerList) {
            if (IInterface::asBinder(it->getListener()) == IInterface::asBinder(listener)) {
                ALOGW("%s: Tried to add listener %p which was already subscribed",
                      __FUNCTION__, listener.get());
                return STATUS_ERROR(ERROR_ALREADY_EXISTS, "Listener already registered");
            }
        }
        sp<ServiceListener> serviceListener =
                new ServiceListener(this, listener, clientUid, clientPid, isVendorListener,
                        openCloseCallbackAllowed);
        auto ret = serviceListener->initialize(isProcessLocalTest);
        if (ret != NO_ERROR) {
            std::string msg = fmt::sprintf("Failed to initialize service listener: %s (%d)",
                    strerror(-ret), ret);
            logServiceError(msg, ERROR_ILLEGAL_ARGUMENT);
            ALOGE("%s: %s", __FUNCTION__, msg.c_str());
            return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, msg.c_str());
        }
        mListenerList.emplace_back(serviceListener);
        mUidPolicy->registerMonitorUid(clientUid, false);
    }
    {
        Mutex::Autolock lock(mCameraStatesLock);
        for (auto& i : mCameraStates) {
            cameraStatuses->emplace_back(i.first,
                    mapToInterface(i.second->getStatus()), i.second->getUnavailablePhysicalIds(),
                    openCloseCallbackAllowed ? i.second->getClientPackage() : std::string());
        }
    }
    cameraStatuses->erase(std::remove_if(cameraStatuses->begin(), cameraStatuses->end(),
                [this, &isVendorListener, &clientPid, &clientUid](const hardware::CameraStatus& s) {
                    SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
                    if (getSystemCameraKind(s.cameraId, &deviceKind) != OK) {
                        ALOGE("%s: Invalid camera id %s, skipping status update",
                                __FUNCTION__, s.cameraId.c_str());
                        return true;
                    }
                    return shouldSkipStatusUpdates(deviceKind, isVendorListener, clientPid,
                            clientUid);}), cameraStatuses->end());
    std::set<std::string> idsChosenForCallback;
    for (const auto &s : *cameraStatuses) {
        idsChosenForCallback.insert(s.cameraId);
    }
    {
        Mutex::Autolock al(mTorchStatusMutex);
        for (size_t i = 0; i < mTorchStatusMap.size(); i++ ) {
            const std::string &id = mTorchStatusMap.keyAt(i);
            if (idsChosenForCallback.find(id) != idsChosenForCallback.end()) {
                listener->onTorchStatusChanged(mapToInterface(mTorchStatusMap.valueAt(i)), id);
            }
        }
    }
    return Status::ok();
}
Status CameraService::removeListener(const sp<ICameraServiceListener>& listener) {
    ATRACE_CALL();
    ALOGV("%s: Remove listener %p", __FUNCTION__, listener.get());
    if (listener == 0) {
        ALOGE("%s: Listener must not be null", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Null listener given to removeListener");
    }
    Mutex::Autolock lock(mServiceLock);
    {
        Mutex::Autolock lock(mStatusListenerLock);
        for (auto it = mListenerList.begin(); it != mListenerList.end(); it++) {
            if (IInterface::asBinder((*it)->getListener()) == IInterface::asBinder(listener)) {
                mUidPolicy->unregisterMonitorUid((*it)->getListenerUid(), false);
                IInterface::asBinder(listener)->unlinkToDeath(*it);
                mListenerList.erase(it);
                return Status::ok();
            }
        }
    }
    ALOGW("%s: Tried to remove a listener %p which was not subscribed",
          __FUNCTION__, listener.get());
    return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Unregistered listener given to removeListener");
}
Status CameraService::getLegacyParameters(int cameraId, std::string* parameters) {
    ATRACE_CALL();
    ALOGV("%s: for camera ID = %d", __FUNCTION__, cameraId);
    if (parameters == NULL) {
        ALOGE("%s: parameters must not be null", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Parameters must not be null");
    }
    Status ret = Status::ok();
    CameraParameters shimParams;
    if (!(ret = getLegacyParametersLazy(cameraId, &shimParams)).isOk()) {
        return ret;
    }
    String8 shimParamsString8 = shimParams.flatten();
    *parameters = toStdString(shimParamsString8);
    return ret;
}
Status CameraService::supportsCameraApi(const std::string& unresolvedCameraId, int apiVersion,
                bool *isSupported) {
    ATRACE_CALL();
    const std::string cameraId = resolveCameraId(
            unresolvedCameraId, CameraThreadState::getCallingUid());
    ALOGV("%s: for camera ID = %s", __FUNCTION__, cameraId.c_str());
    switch (apiVersion) {
        case API_VERSION_1:
        case API_VERSION_2:
            break;
        default:
            std::string msg = fmt::sprintf("Unknown API version %d", apiVersion);
            ALOGE("%s: %s", __FUNCTION__, msg.c_str());
            return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }
    int portraitRotation;
    auto deviceVersionAndTransport = getDeviceVersion(cameraId, false, &portraitRotation);
    if (deviceVersionAndTransport.first == -1) {
        std::string msg = fmt::sprintf("Unknown camera ID %s", cameraId.c_str());
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }
    if (deviceVersionAndTransport.second == IPCTransport::HIDL) {
        int deviceVersion = deviceVersionAndTransport.first;
        switch (deviceVersion) {
            case CAMERA_DEVICE_API_VERSION_1_0:
            case CAMERA_DEVICE_API_VERSION_3_0:
            case CAMERA_DEVICE_API_VERSION_3_1:
                if (apiVersion == API_VERSION_2) {
                    ALOGV("%s: Camera id %s uses HAL version %d <3.2, doesn't support api2 without "
                            "shim", __FUNCTION__, cameraId.c_str(), deviceVersion);
                    *isSupported = false;
                } else {
                    ALOGV("%s: Camera id %s uses older HAL before 3.2, but api1 is always "
                            "supported", __FUNCTION__, cameraId.c_str());
                    *isSupported = true;
                }
                break;
            case CAMERA_DEVICE_API_VERSION_3_2:
            case CAMERA_DEVICE_API_VERSION_3_3:
            case CAMERA_DEVICE_API_VERSION_3_4:
            case CAMERA_DEVICE_API_VERSION_3_5:
            case CAMERA_DEVICE_API_VERSION_3_6:
            case CAMERA_DEVICE_API_VERSION_3_7:
                ALOGV("%s: Camera id %s uses HAL3.2 or newer, supports api1/api2 directly",
                        __FUNCTION__, cameraId.c_str());
                *isSupported = true;
                break;
            default: {
                std::string msg = fmt::sprintf("Unknown device version %x for device %s",
                        deviceVersion, cameraId.c_str());
                ALOGE("%s: %s", __FUNCTION__, msg.c_str());
                return STATUS_ERROR(ERROR_INVALID_OPERATION, msg.c_str());
            }
        }
    } else {
        *isSupported = true;
    }
    return Status::ok();
}
Status CameraService::isHiddenPhysicalCamera(const std::string& unresolvedCameraId,
                bool *isSupported) {
    ATRACE_CALL();
    const std::string cameraId = resolveCameraId(unresolvedCameraId,
            CameraThreadState::getCallingUid());
    ALOGV("%s: for camera ID = %s", __FUNCTION__, cameraId.c_str());
    *isSupported = mCameraProviderManager->isHiddenPhysicalCamera(cameraId);
    return Status::ok();
}
Status CameraService::injectCamera(
        const std::string& packageName, const std::string& internalCamId,
        const std::string& externalCamId,
        const sp<ICameraInjectionCallback>& callback,
        sp<ICameraInjectionSession>* cameraInjectionSession) {
    ATRACE_CALL();
    if (!checkCallingPermission(toString16(sCameraInjectExternalCameraPermission))) {
        const int pid = CameraThreadState::getCallingPid();
        const int uid = CameraThreadState::getCallingUid();
        ALOGE("Permission Denial: can't inject camera pid=%d, uid=%d", pid, uid);
        return STATUS_ERROR(ERROR_PERMISSION_DENIED,
                        "Permission Denial: no permission to inject camera");
    }
    ALOGV(
        "%s: Package name = %s, Internal camera ID = %s, External camera ID = "
        "%s",
        __FUNCTION__, packageName.c_str(),
        internalCamId.c_str(), externalCamId.c_str());
    {
        Mutex::Autolock lock(mInjectionParametersLock);
        mInjectionInternalCamId = internalCamId;
        mInjectionExternalCamId = externalCamId;
        mInjectionStatusListener->addListener(callback);
        *cameraInjectionSession = new CameraInjectionSession(this);
        status_t res = NO_ERROR;
        auto clientDescriptor = mActiveClientManager.get(mInjectionInternalCamId);
        if (clientDescriptor != nullptr) {
            mInjectionInitPending = false;
            sp<BasicClient> clientSp = clientDescriptor->getValue();
            res = checkIfInjectionCameraIsPresent(mInjectionExternalCamId, clientSp);
            if(res != OK) {
                return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                        "No camera device with ID \"%s\" currently available",
                        mInjectionExternalCamId.c_str());
            }
            res = clientSp->injectCamera(mInjectionExternalCamId, mCameraProviderManager);
            if(res != OK) {
                mInjectionStatusListener->notifyInjectionError(mInjectionExternalCamId, res);
            }
        } else {
            mInjectionInitPending = true;
        }
    }
    return binder::Status::ok();
}
Status CameraService::reportExtensionSessionStats(
        const hardware::CameraExtensionSessionStats& stats, std::string* sessionKey ) {
    ALOGV("%s: reported %s", __FUNCTION__, stats.toString().c_str());
    *sessionKey = mCameraServiceProxyWrapper->updateExtensionStats(stats);
    return Status::ok();
}
void CameraService::removeByClient(const BasicClient* client) {
    Mutex::Autolock lock(mServiceLock);
    for (auto& i : mActiveClientManager.getAll()) {
        auto clientSp = i->getValue();
        if (clientSp.get() == client) {
            cacheClientTagDumpIfNeeded(client->mCameraIdStr, clientSp.get());
            mActiveClientManager.remove(i);
        }
    }
    updateAudioRestrictionLocked();
}
bool CameraService::evictClientIdByRemote(const wp<IBinder>& remote) {
    bool ret = false;
    {
        std::unique_ptr<AutoConditionLock> lock =
                AutoConditionLock::waitAndAcquire(mServiceLockWrapper);
        std::vector<sp<BasicClient>> evicted;
        for (auto& i : mActiveClientManager.getAll()) {
            auto clientSp = i->getValue();
            if (clientSp.get() == nullptr) {
                ALOGE("%s: Dead client still in mActiveClientManager.", __FUNCTION__);
                mActiveClientManager.remove(i);
                continue;
            }
            if (remote == clientSp->getRemote()) {
                mActiveClientManager.remove(i);
                evicted.push_back(clientSp);
                clientSp->notifyError(
                        hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISCONNECTED,
                        CaptureResultExtras());
            }
        }
        mServiceLock.unlock();
        for (auto& i : evicted) {
            if (i.get() != nullptr) {
                i->disconnect();
                ret = true;
            }
        }
        mServiceLock.lock();
    }
    return ret;
}
std::shared_ptr<CameraService::CameraState> CameraService::getCameraState(
        const std::string& cameraId) const {
    std::shared_ptr<CameraState> state;
    {
        Mutex::Autolock lock(mCameraStatesLock);
        auto iter = mCameraStates.find(cameraId);
        if (iter != mCameraStates.end()) {
            state = iter->second;
        }
    }
    return state;
}
sp<CameraService::BasicClient> CameraService::removeClientLocked(const std::string& cameraId) {
    auto clientDescriptorPtr = mActiveClientManager.remove(cameraId);
    if (clientDescriptorPtr == nullptr) {
        ALOGW("%s: Could not evict client, no client for camera ID %s", __FUNCTION__,
                cameraId.c_str());
        return sp<BasicClient>{nullptr};
    }
    sp<BasicClient> client = clientDescriptorPtr->getValue();
    if (client.get() != nullptr) {
        cacheClientTagDumpIfNeeded(clientDescriptorPtr->getKey(), client.get());
    }
    return client;
}
void CameraService::doUserSwitch(const std::vector<int32_t>& newUserIds) {
    std::unique_ptr<AutoConditionLock> lock =
            AutoConditionLock::waitAndAcquire(mServiceLockWrapper);
    std::set<userid_t> newAllowedUsers;
    for (size_t i = 0; i < newUserIds.size(); i++) {
        if (newUserIds[i] < 0) {
            ALOGE("%s: Bad user ID %d given during user switch, ignoring.",
                    __FUNCTION__, newUserIds[i]);
            return;
        }
        newAllowedUsers.insert(static_cast<userid_t>(newUserIds[i]));
    }
    if (newAllowedUsers == mAllowedUsers) {
        ALOGW("%s: Received notification of user switch with no updated user IDs.", __FUNCTION__);
        return;
    }
    logUserSwitch(mAllowedUsers, newAllowedUsers);
    mAllowedUsers = std::move(newAllowedUsers);
    std::vector<sp<BasicClient>> evicted;
    for (auto& i : mActiveClientManager.getAll()) {
        auto clientSp = i->getValue();
        if (clientSp.get() == nullptr) {
            ALOGE("%s: Dead client still in mActiveClientManager.", __FUNCTION__);
            continue;
        }
        uid_t clientUid = clientSp->getClientUid();
        userid_t clientUserId = multiuser_get_user_id(clientUid);
        if (mAllowedUsers.find(clientUserId) != mAllowedUsers.end()) {
            continue;
        }
        evicted.push_back(clientSp);
        ALOGE("Evicting conflicting client for camera ID %s due to user change",
                i->getKey().c_str());
        logEvent(fmt::sprintf("EVICT device %s client held by package %s (PID %"
                PRId32 ", score %" PRId32 ", state %" PRId32 ")\n   - Evicted due"
                " to user switch.", i->getKey().c_str(),
                clientSp->getPackageName().c_str(),
                i->getOwnerId(), i->getPriority().getScore(),
                i->getPriority().getState()));
    }
    mServiceLock.unlock();
    int64_t token = CameraThreadState::clearCallingIdentity();
    for (auto& i : evicted) {
        i->disconnect();
    }
    CameraThreadState::restoreCallingIdentity(token);
    mServiceLock.lock();
}
void CameraService::logEvent(const std::string &event) {
    std::string curTime = getFormattedCurrentTime();
    Mutex::Autolock l(mLogLock);
    std::string msg = curTime + " : " + event;
    if (msg.find("SERVICE ERROR") != std::string::npos) {
        mEventLog.add(msg);
    } else if(sServiceErrorEventSet.find(msg) == sServiceErrorEventSet.end()) {
        mEventLog.add(msg);
        sServiceErrorEventSet.insert(msg);
    }
}
void CameraService::logDisconnected(const std::string &cameraId, int clientPid,
        const std::string &clientPackage) {
    logEvent(fmt::sprintf("DISCONNECT device %s client for package %s (PID %d)", cameraId.c_str(),
            clientPackage.c_str(), clientPid));
}
void CameraService::logDisconnectedOffline(const std::string &cameraId, int clientPid,
        const std::string &clientPackage) {
    logEvent(fmt::sprintf("DISCONNECT offline device %s client for package %s (PID %d)",
            cameraId.c_str(), clientPackage.c_str(), clientPid));
}
void CameraService::logConnected(const std::string &cameraId, int clientPid,
        const std::string &clientPackage) {
    logEvent(fmt::sprintf("CONNECT device %s client for package %s (PID %d)", cameraId.c_str(),
            clientPackage.c_str(), clientPid));
}
void CameraService::logConnectedOffline(const std::string &cameraId, int clientPid,
        const std::string &clientPackage) {
    logEvent(fmt::sprintf("CONNECT offline device %s client for package %s (PID %d)",
            cameraId.c_str(), clientPackage.c_str(), clientPid));
}
void CameraService::logRejected(const std::string &cameraId, int clientPid,
        const std::string &clientPackage, const std::string &reason) {
    logEvent(fmt::sprintf("REJECT device %s client for package %s (PID %d), reason: (%s)",
            cameraId.c_str(), clientPackage.c_str(), clientPid, reason.c_str()));
}
void CameraService::logTorchEvent(const std::string &cameraId, const std::string &torchState,
        int clientPid) {
    logEvent(fmt::sprintf("Torch for camera id %s turned %s for client PID %d", cameraId.c_str(),
            torchState.c_str(), clientPid));
}
void CameraService::logUserSwitch(const std::set<userid_t>& oldUserIds,
        const std::set<userid_t>& newUserIds) {
    std::string newUsers = toString(newUserIds);
    std::string oldUsers = toString(oldUserIds);
    if (oldUsers.size() == 0) {
        oldUsers = "<None>";
    }
    logEvent(fmt::sprintf("USER_SWITCH previous allowed user IDs: %s, current allowed user IDs: %s",
            oldUsers.c_str(), newUsers.c_str()));
}
void CameraService::logDeviceRemoved(const std::string &cameraId, const std::string &reason) {
    logEvent(fmt::sprintf("REMOVE device %s, reason: (%s)", cameraId.c_str(), reason.c_str()));
}
void CameraService::logDeviceAdded(const std::string &cameraId, const std::string &reason) {
    logEvent(fmt::sprintf("ADD device %s, reason: (%s)", cameraId.c_str(), reason.c_str()));
}
void CameraService::logClientDied(int clientPid, const std::string &reason) {
    logEvent(fmt::sprintf("DIED client(s) with PID %d, reason: (%s)", clientPid, reason.c_str()));
}
void CameraService::logServiceError(const std::string &msg, int errorCode) {
    logEvent(fmt::sprintf("SERVICE ERROR: %s : %d (%s)", msg.c_str(), errorCode,
            strerror(-errorCode)));
}
status_t CameraService::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
        uint32_t flags) {
    switch (code) {
        case SHELL_COMMAND_TRANSACTION: {
            int in = data.readFileDescriptor();
            int out = data.readFileDescriptor();
            int err = data.readFileDescriptor();
            int argc = data.readInt32();
            Vector<String16> args;
            for (int i = 0; i < argc && data.dataAvail() > 0; i++) {
               args.add(data.readString16());
            }
            sp<IBinder> unusedCallback;
            sp<IResultReceiver> resultReceiver;
            status_t status;
            if ((status = data.readNullableStrongBinder(&unusedCallback)) != NO_ERROR) {
                return status;
            }
            if ((status = data.readNullableStrongBinder(&resultReceiver)) != NO_ERROR) {
                return status;
            }
            status = shellCommand(in, out, err, args);
            if (resultReceiver != nullptr) {
                resultReceiver->send(status);
            }
            return NO_ERROR;
        }
    }
    return BnCameraService::onTransact(code, data, reply, flags);
}
sp<MediaPlayer> CameraService::newMediaPlayer(const char *file) {
    sp<MediaPlayer> mp = new MediaPlayer();
    status_t error;
    if ((error = mp->setDataSource(NULL , file, NULL)) == NO_ERROR) {
        mp->setAudioStreamType(AUDIO_STREAM_ENFORCED_AUDIBLE);
        error = mp->prepare();
    }
    if (error != NO_ERROR) {
        ALOGE("Failed to load CameraService sounds: %s", file);
        mp->disconnect();
        mp.clear();
        return nullptr;
    }
    return mp;
}
void CameraService::increaseSoundRef() {
    Mutex::Autolock lock(mSoundLock);
    mSoundRef++;
}
void CameraService::loadSoundLocked(sound_kind kind) {
    ATRACE_CALL();
    LOG1("CameraService::loadSoundLocked ref=%d", mSoundRef);
    if (SOUND_SHUTTER == kind && mSoundPlayer[SOUND_SHUTTER] == NULL) {
        mSoundPlayer[SOUND_SHUTTER] = newMediaPlayer("/product/media/audio/ui/camera_click.ogg");
        if (mSoundPlayer[SOUND_SHUTTER] == nullptr) {
            mSoundPlayer[SOUND_SHUTTER] = newMediaPlayer("/system/media/audio/ui/camera_click.ogg");
        }
    } else if (SOUND_RECORDING_START == kind && mSoundPlayer[SOUND_RECORDING_START] == NULL) {
        mSoundPlayer[SOUND_RECORDING_START] = newMediaPlayer("/product/media/audio/ui/VideoRecord.ogg");
        if (mSoundPlayer[SOUND_RECORDING_START] == nullptr) {
            mSoundPlayer[SOUND_RECORDING_START] =
                newMediaPlayer("/system/media/audio/ui/VideoRecord.ogg");
        }
    } else if (SOUND_RECORDING_STOP == kind && mSoundPlayer[SOUND_RECORDING_STOP] == NULL) {
        mSoundPlayer[SOUND_RECORDING_STOP] = newMediaPlayer("/product/media/audio/ui/VideoStop.ogg");
        if (mSoundPlayer[SOUND_RECORDING_STOP] == nullptr) {
            mSoundPlayer[SOUND_RECORDING_STOP] = newMediaPlayer("/system/media/audio/ui/VideoStop.ogg");
        }
    }
}
void CameraService::decreaseSoundRef() {
    Mutex::Autolock lock(mSoundLock);
    LOG1("CameraService::decreaseSoundRef ref=%d", mSoundRef);
    if (--mSoundRef) return;
    for (int i = 0; i < NUM_SOUNDS; i++) {
        if (mSoundPlayer[i] != 0) {
            mSoundPlayer[i]->disconnect();
            mSoundPlayer[i].clear();
        }
    }
}
void CameraService::playSound(sound_kind kind) {
    ATRACE_CALL();
    LOG1("playSound(%d)", kind);
    if (kind < 0 || kind >= NUM_SOUNDS) {
        ALOGE("%s: Invalid sound id requested: %d", __FUNCTION__, kind);
        return;
    }
    Mutex::Autolock lock(mSoundLock);
    loadSoundLocked(kind);
    sp<MediaPlayer> player = mSoundPlayer[kind];
    if (player != 0) {
        player->seekTo(0);
        player->start();
    }
}
CameraService::Client::Client(const sp<CameraService>& cameraService,
        const sp<ICameraClient>& cameraClient,
        const std::string& clientPackageName, bool systemNativeClient,
        const std::optional<std::string>& clientFeatureId,
        const std::string& cameraIdStr,
        int api1CameraId, int cameraFacing, int sensorOrientation,
        int clientPid, uid_t clientUid,
        int servicePid, bool overrideToPortrait) :
        CameraService::BasicClient(cameraService,
                IInterface::asBinder(cameraClient),
                clientPackageName, systemNativeClient, clientFeatureId,
                cameraIdStr, cameraFacing, sensorOrientation,
                clientPid, clientUid,
                servicePid, overrideToPortrait),
        mCameraId(api1CameraId)
{
    int callingPid = CameraThreadState::getCallingPid();
    LOG1("Client::Client E (pid %d, id %d)", callingPid, mCameraId);
    mRemoteCallback = cameraClient;
    cameraService->increaseSoundRef();
    LOG1("Client::Client X (pid %d, id %d)", callingPid, mCameraId);
}
CameraService::Client::~Client() {
    ALOGV("~Client");
    mDestructionStarted = true;
    sCameraService->decreaseSoundRef();
    Client::disconnect();
}
sp<CameraService> CameraService::BasicClient::BasicClient::sCameraService;
CameraService::BasicClient::BasicClient(const sp<CameraService>& cameraService,
        const sp<IBinder>& remoteCallback,
        const std::string& clientPackageName, bool nativeClient,
        const std::optional<std::string>& clientFeatureId, const std::string& cameraIdStr,
        int cameraFacing, int sensorOrientation, int clientPid, uid_t clientUid,
        int servicePid, bool overrideToPortrait):
<<<<<<< HEAD
        AttributionAndPermissionUtilsEncapsulator(attributionAndPermissionUtils),
||||||| 38b856f7cf
        mAttributionAndPermissionUtils(attributionAndPermissionUtils),
=======
>>>>>>> 94c719f4
        mDestructionStarted(false),
        mCameraIdStr(cameraIdStr), mCameraFacing(cameraFacing), mOrientation(sensorOrientation),
        mClientPackageName(clientPackageName), mSystemNativeClient(nativeClient),
        mClientFeatureId(clientFeatureId),
        mClientPid(clientPid), mClientUid(clientUid),
        mServicePid(servicePid),
        mDisconnected(false), mUidIsTrusted(false),
        mOverrideToPortrait(overrideToPortrait),
        mAudioRestriction(hardware::camera2::ICameraDeviceUser::AUDIO_RESTRICTION_NONE),
        mRemoteBinder(remoteCallback),
        mOpsActive(false),
        mOpsStreaming(false)
{
    if (sCameraService == nullptr) {
        sCameraService = cameraService;
    }
    if (!mSystemNativeClient) {
        mAppOpsManager = std::make_unique<AppOpsManager>();
    }
    mUidIsTrusted = isTrustedCallingUid(mClientUid);
}
CameraService::BasicClient::~BasicClient() {
    ALOGV("~BasicClient");
    mDestructionStarted = true;
}
binder::Status CameraService::BasicClient::disconnect() {
    binder::Status res = Status::ok();
    if (mDisconnected) {
        return res;
    }
    mDisconnected = true;
    sCameraService->removeByClient(this);
    sCameraService->logDisconnected(mCameraIdStr, mClientPid, mClientPackageName);
    sCameraService->mCameraProviderManager->removeRef(CameraProviderManager::DeviceMode::CAMERA,
            mCameraIdStr);
    sp<IBinder> remote = getRemote();
    if (remote != nullptr) {
        remote->unlinkToDeath(sCameraService);
    }
    finishCameraOps();
    sCameraService->mFlashlight->deviceClosed(mCameraIdStr);
    ALOGI("%s: Disconnected client for camera %s for PID %d", __FUNCTION__, mCameraIdStr.c_str(),
            mClientPid);
    mClientPid = 0;
    const auto& mActivityManager = getActivityManager();
    if (mActivityManager) {
        mActivityManager->logFgsApiEnd(LOG_FGS_CAMERA_API,
            CameraThreadState::getCallingUid(),
            CameraThreadState::getCallingPid());
    }
    return res;
}
status_t CameraService::BasicClient::dump(int, const Vector<String16>&) {
    android_errorWriteWithInfoLog(SN_EVENT_LOG_ID, "26265403",
            CameraThreadState::getCallingUid(), NULL, 0);
    return OK;
}
status_t CameraService::BasicClient::startWatchingTags(const std::string&, int) {
    return OK;
}
status_t CameraService::BasicClient::stopWatchingTags(int) {
    return OK;
}
status_t CameraService::BasicClient::dumpWatchedEventsToVector(std::vector<std::string> &) {
    return OK;
}
std::string CameraService::BasicClient::getPackageName() const {
    return mClientPackageName;
}
int CameraService::BasicClient::getCameraFacing() const {
    return mCameraFacing;
}
int CameraService::BasicClient::getCameraOrientation() const {
    return mOrientation;
}
int CameraService::BasicClient::getClientPid() const {
    return mClientPid;
}
uid_t CameraService::BasicClient::getClientUid() const {
    return mClientUid;
}
bool CameraService::BasicClient::canCastToApiClient(apiLevel level) const {
    return level == API_2;
}
status_t CameraService::BasicClient::setAudioRestriction(int32_t mode) {
    {
        Mutex::Autolock l(mAudioRestrictionLock);
        mAudioRestriction = mode;
    }
    sCameraService->updateAudioRestriction();
    return OK;
}
int32_t CameraService::BasicClient::getServiceAudioRestriction() const {
    return sCameraService->updateAudioRestriction();
}
int32_t CameraService::BasicClient::getAudioRestriction() const {
    Mutex::Autolock l(mAudioRestrictionLock);
    return mAudioRestriction;
}
bool CameraService::BasicClient::isValidAudioRestriction(int32_t mode) {
    switch (mode) {
        case hardware::camera2::ICameraDeviceUser::AUDIO_RESTRICTION_NONE:
        case hardware::camera2::ICameraDeviceUser::AUDIO_RESTRICTION_VIBRATION:
        case hardware::camera2::ICameraDeviceUser::AUDIO_RESTRICTION_VIBRATION_SOUND:
            return true;
        default:
            return false;
    }
}
status_t CameraService::BasicClient::handleAppOpMode(int32_t mode) {
    if (mode == AppOpsManager::MODE_ERRORED) {
        ALOGI("Camera %s: Access for \"%s\" has been revoked",
                mCameraIdStr.c_str(), mClientPackageName.c_str());
        return PERMISSION_DENIED;
    } else if (!mUidIsTrusted && mode == AppOpsManager::MODE_IGNORED) {
        bool isUidActive = sCameraService->mUidPolicy->isUidActive(mClientUid,
                mClientPackageName);
        bool isCameraPrivacyEnabled;
        if (flags::camera_privacy_allowlist()) {
            isCameraPrivacyEnabled = sCameraService->isCameraPrivacyEnabled(
                    toString16(mClientPackageName), std::string(), mClientPid, mClientUid);
        } else {
            isCameraPrivacyEnabled =
                sCameraService->mSensorPrivacyPolicy->isCameraPrivacyEnabled();
        }
        if (!isUidActive || !isCameraPrivacyEnabled) {
            ALOGI("Camera %s: Access for \"%s\" has been restricted",
                    mCameraIdStr.c_str(), mClientPackageName.c_str());
            return -EACCES;
        }
    }
    return OK;
}
status_t CameraService::BasicClient::startCameraOps() {
    ATRACE_CALL();
    {
        ALOGV("%s: Start camera ops, package name = %s, client UID = %d",
              __FUNCTION__, mClientPackageName.c_str(), mClientUid);
    }
    if (mAppOpsManager != nullptr) {
        mOpsCallback = new OpsCallback(this);
        if (flags::watch_foreground_changes()) {
            mAppOpsManager->startWatchingMode(AppOpsManager::OP_CAMERA,
                toString16(mClientPackageName),
                AppOpsManager::WATCH_FOREGROUND_CHANGES, mOpsCallback);
        } else {
            mAppOpsManager->startWatchingMode(AppOpsManager::OP_CAMERA,
                toString16(mClientPackageName), mOpsCallback);
        }
        int32_t mode = mAppOpsManager->checkOp(AppOpsManager::OP_CAMERA, mClientUid,
                toString16(mClientPackageName));
        status_t res = handleAppOpMode(mode);
        if (res != OK) {
            return res;
        }
    }
    mOpsActive = true;
    sCameraService->updateStatus(StatusInternal::NOT_AVAILABLE, mCameraIdStr);
    sCameraService->mUidPolicy->registerMonitorUid(mClientUid, true);
    sCameraService->updateOpenCloseStatus(mCameraIdStr, true , mClientPackageName);
    return OK;
}
status_t CameraService::BasicClient::startCameraStreamingOps() {
    ATRACE_CALL();
    if (!mOpsActive) {
        ALOGE("%s: Calling streaming start when not yet active", __FUNCTION__);
        return INVALID_OPERATION;
    }
    if (mOpsStreaming) {
        ALOGV("%s: Streaming already active!", __FUNCTION__);
        return OK;
    }
    ALOGV("%s: Start camera streaming ops, package name = %s, client UID = %d",
            __FUNCTION__, mClientPackageName.c_str(), mClientUid);
    if (mAppOpsManager != nullptr) {
        int32_t mode = mAppOpsManager->startOpNoThrow(AppOpsManager::OP_CAMERA, mClientUid,
                toString16(mClientPackageName), false,
                toString16(mClientFeatureId),
                toString16("start camera ") + toString16(mCameraIdStr));
        status_t res = handleAppOpMode(mode);
        if (res != OK) {
            return res;
        }
    }
    mOpsStreaming = true;
    return OK;
}
status_t CameraService::BasicClient::noteAppOp() {
    ATRACE_CALL();
    ALOGV("%s: Start camera noteAppOp, package name = %s, client UID = %d",
            __FUNCTION__, mClientPackageName.c_str(), mClientUid);
    if (mAppOpsManager != nullptr) {
        int32_t mode = mAppOpsManager->noteOp(AppOpsManager::OP_CAMERA, mClientUid,
                toString16(mClientPackageName), toString16(mClientFeatureId),
                toString16("start camera ") + toString16(mCameraIdStr));
        status_t res = handleAppOpMode(mode);
        if (res != OK) {
            return res;
        }
    }
    return OK;
}
status_t CameraService::BasicClient::finishCameraStreamingOps() {
    ATRACE_CALL();
    if (!mOpsActive) {
        ALOGE("%s: Calling streaming start when not yet active", __FUNCTION__);
        return INVALID_OPERATION;
    }
    if (!mOpsStreaming) {
        ALOGV("%s: Streaming not active!", __FUNCTION__);
        return OK;
    }
    if (mAppOpsManager != nullptr) {
        mAppOpsManager->finishOp(AppOpsManager::OP_CAMERA, mClientUid,
                toString16(mClientPackageName), toString16(mClientFeatureId));
        mOpsStreaming = false;
    }
    return OK;
}
status_t CameraService::BasicClient::finishCameraOps() {
    ATRACE_CALL();
    if (mOpsStreaming) {
        finishCameraStreamingOps();
    }
    if (mOpsActive) {
        mOpsActive = false;
        std::initializer_list<StatusInternal> rejected = {StatusInternal::PRESENT,
                StatusInternal::ENUMERATING, StatusInternal::NOT_PRESENT};
        sCameraService->updateStatus(StatusInternal::PRESENT,
                mCameraIdStr, rejected);
    }
    if (mOpsCallback != nullptr && mAppOpsManager != nullptr) {
        mAppOpsManager->stopWatchingMode(mOpsCallback);
    }
    mOpsCallback.clear();
    sCameraService->mUidPolicy->unregisterMonitorUid(mClientUid, true);
    sCameraService->updateOpenCloseStatus(mCameraIdStr, false , mClientPackageName);
    return OK;
}
void CameraService::BasicClient::opChanged(int32_t op, const String16&) {
    ATRACE_CALL();
    if (mAppOpsManager == nullptr) {
        return;
    }
    if (op != AppOpsManager::OP_CAMERA) {
        ALOGW("Unexpected app ops notification received: %d", op);
        return;
    }
    int32_t res;
    res = mAppOpsManager->checkOp(AppOpsManager::OP_CAMERA,
            mClientUid, toString16(mClientPackageName));
    ALOGV("checkOp returns: %d, %s ", res,
            res == AppOpsManager::MODE_ALLOWED ? "ALLOWED" :
            res == AppOpsManager::MODE_IGNORED ? "IGNORED" :
            res == AppOpsManager::MODE_ERRORED ? "ERRORED" :
            "UNKNOWN");
    if (res == AppOpsManager::MODE_ERRORED) {
        ALOGI("Camera %s: Access for \"%s\" revoked", mCameraIdStr.c_str(),
              mClientPackageName.c_str());
        block();
    } else if (res == AppOpsManager::MODE_IGNORED) {
        bool isUidActive = sCameraService->mUidPolicy->isUidActive(mClientUid, mClientPackageName);
        int32_t procState = sCameraService->mUidPolicy->getProcState(mClientUid);
        bool isUidVisible = (procState <= ActivityManager::PROCESS_STATE_BOUND_TOP);
        bool isCameraPrivacyEnabled;
        if (flags::camera_privacy_allowlist()) {
            isCameraPrivacyEnabled = sCameraService->isCameraPrivacyEnabled(
                    toString16(mClientPackageName),std::string(),mClientPid,mClientUid);
        } else {
            isCameraPrivacyEnabled =
                sCameraService->mSensorPrivacyPolicy->isCameraPrivacyEnabled();
        }
        ALOGI("Camera %s: Access for \"%s\" has been restricted, isUidTrusted %d, isUidActive %d"
                " isUidVisible %d, isCameraPrivacyEnabled %d", mCameraIdStr.c_str(),
                mClientPackageName.c_str(), mUidIsTrusted, isUidActive, isUidVisible,
                isCameraPrivacyEnabled);
        if (!mUidIsTrusted) {
            if (flags::watch_foreground_changes()) {
                if (isUidVisible && isCameraPrivacyEnabled && supportsCameraMute()) {
                    setCameraMute(true);
                } else {
                    block();
                }
            } else {
                if (isUidActive && isCameraPrivacyEnabled && supportsCameraMute()) {
                    setCameraMute(true);
                } else if (!isUidActive
                    || (isCameraPrivacyEnabled && !supportsCameraMute())) {
                    block();
                }
            }
        }
    } else if (res == AppOpsManager::MODE_ALLOWED) {
        setCameraMute(sCameraService->mOverrideCameraMuteMode);
    }
}
void CameraService::BasicClient::block() {
    ATRACE_CALL();
    mClientPid = CameraThreadState::getCallingPid();
    CaptureResultExtras resultExtras;
    notifyError(hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISABLED, resultExtras);
    disconnect();
}
void CameraService::Client::notifyError(int32_t errorCode,
        [[maybe_unused]] const CaptureResultExtras& resultExtras) {
    if (mRemoteCallback != NULL) {
        int32_t api1ErrorCode = CAMERA_ERROR_RELEASED;
        if (errorCode == hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISABLED) {
            api1ErrorCode = CAMERA_ERROR_DISABLED;
        }
        mRemoteCallback->notifyCallback(CAMERA_MSG_ERROR, api1ErrorCode, 0);
    } else {
        ALOGE("mRemoteCallback is NULL!!");
    }
}
binder::Status CameraService::Client::disconnect() {
    ALOGV("Client::disconnect");
    return BasicClient::disconnect();
}
bool CameraService::Client::canCastToApiClient(apiLevel level) const {
    return level == API_1;
}
CameraService::Client::OpsCallback::OpsCallback(wp<BasicClient> client):
        mClient(client) {
}
void CameraService::Client::OpsCallback::opChanged(int32_t op,
        const String16& packageName) {
    sp<BasicClient> client = mClient.promote();
    if (client != NULL) {
        client->opChanged(op, packageName);
    }
}
void CameraService::UidPolicy::registerWithActivityManager() {
    Mutex::Autolock _l(mUidLock);
    int32_t emptyUidArray[] = { };
    if (mRegistered) return;
    status_t res = mAm.linkToDeath(this);
    mAm.registerUidObserverForUids(this, ActivityManager::UID_OBSERVER_GONE
            | ActivityManager::UID_OBSERVER_IDLE
            | ActivityManager::UID_OBSERVER_ACTIVE | ActivityManager::UID_OBSERVER_PROCSTATE
            | ActivityManager::UID_OBSERVER_PROC_OOM_ADJ,
            ActivityManager::PROCESS_STATE_UNKNOWN,
            toString16(kServiceName), emptyUidArray, 0, mObserverToken);
    if (res == OK) {
        mRegistered = true;
        ALOGV("UidPolicy: Registered with ActivityManager");
    } else {
        ALOGE("UidPolicy: Failed to register with ActivityManager: 0x%08x", res);
    }
}
void CameraService::UidPolicy::onServiceRegistration(const String16& name, const sp<IBinder>&) {
    if (name != toString16(kActivityServiceName)) {
        return;
    }
    registerWithActivityManager();
}
void CameraService::UidPolicy::registerSelf() {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->checkService(toString16(kActivityServiceName));
    if (!binder) {
        sm->registerForNotifications(toString16(kActivityServiceName), this);
    } else {
        registerWithActivityManager();
    }
}
void CameraService::UidPolicy::unregisterSelf() {
    Mutex::Autolock _l(mUidLock);
    mAm.unregisterUidObserver(this);
    mAm.unlinkToDeath(this);
    mRegistered = false;
    mActiveUids.clear();
    ALOGV("UidPolicy: Unregistered with ActivityManager");
}
void CameraService::UidPolicy::onUidGone(uid_t uid, bool disabled) {
    onUidIdle(uid, disabled);
}
void CameraService::UidPolicy::onUidActive(uid_t uid) {
    Mutex::Autolock _l(mUidLock);
    mActiveUids.insert(uid);
}
void CameraService::UidPolicy::onUidIdle(uid_t uid, bool ) {
    bool deleted = false;
    {
        Mutex::Autolock _l(mUidLock);
        if (mActiveUids.erase(uid) > 0) {
            deleted = true;
        }
    }
    if (deleted) {
        sp<CameraService> service = mService.promote();
        if (service != nullptr) {
            service->blockClientsForUid(uid);
        }
    }
}
void CameraService::UidPolicy::onUidStateChanged(uid_t uid, int32_t procState,
        int64_t procStateSeq __unused, int32_t capability __unused) {
    bool procStateChange = false;
    {
        Mutex::Autolock _l(mUidLock);
        if (mMonitoredUids.find(uid) != mMonitoredUids.end() &&
                mMonitoredUids[uid].procState != procState) {
            mMonitoredUids[uid].procState = procState;
            procStateChange = true;
        }
    }
    if (procStateChange) {
        sp<CameraService> service = mService.promote();
        if (service != nullptr) {
            service->notifyMonitoredUids();
        }
    }
}
void CameraService::UidPolicy::onUidProcAdjChanged(uid_t uid, int32_t adj) {
    std::unordered_set<uid_t> notifyUidSet;
    {
        Mutex::Autolock _l(mUidLock);
        auto it = mMonitoredUids.find(uid);
        if (it != mMonitoredUids.end()) {
            if (it->second.hasCamera) {
                for (auto &monitoredUid : mMonitoredUids) {
                    if (monitoredUid.first != uid && adj > monitoredUid.second.procAdj) {
                        ALOGV("%s: notify uid %d", __FUNCTION__, monitoredUid.first);
                        notifyUidSet.emplace(monitoredUid.first);
                    }
                }
                ALOGV("%s: notify uid %d", __FUNCTION__, uid);
                notifyUidSet.emplace(uid);
            } else {
                for (auto &monitoredUid : mMonitoredUids) {
                    if (monitoredUid.second.hasCamera && adj < monitoredUid.second.procAdj) {
                        ALOGV("%s: notify uid %d", __FUNCTION__, uid);
                        notifyUidSet.emplace(uid);
                    }
                }
            }
            it->second.procAdj = adj;
        }
    }
    if (notifyUidSet.size() > 0) {
        sp<CameraService> service = mService.promote();
        if (service != nullptr) {
            service->notifyMonitoredUids(notifyUidSet);
        }
    }
}
void CameraService::UidPolicy::registerMonitorUid(uid_t uid, bool openCamera) {
    Mutex::Autolock _l(mUidLock);
    auto it = mMonitoredUids.find(uid);
    if (it != mMonitoredUids.end()) {
        it->second.refCount++;
    } else {
        MonitoredUid monitoredUid;
        monitoredUid.procState = ActivityManager::PROCESS_STATE_NONEXISTENT;
        monitoredUid.procAdj = resource_policy::UNKNOWN_ADJ;
        monitoredUid.refCount = 1;
        it = mMonitoredUids.emplace(std::pair<uid_t, MonitoredUid>(uid, monitoredUid)).first;
        status_t res = mAm.addUidToObserver(mObserverToken, toString16(kServiceName), uid);
        if (res != OK) {
            ALOGE("UidPolicy: Failed to add uid to observer: 0x%08x", res);
        }
    }
    if (openCamera) {
        it->second.hasCamera = true;
    }
}
void CameraService::UidPolicy::unregisterMonitorUid(uid_t uid, bool closeCamera) {
    Mutex::Autolock _l(mUidLock);
    auto it = mMonitoredUids.find(uid);
    if (it != mMonitoredUids.end()) {
        it->second.refCount--;
        if (it->second.refCount == 0) {
            mMonitoredUids.erase(it);
            status_t res = mAm.removeUidFromObserver(mObserverToken, toString16(kServiceName), uid);
            if (res != OK) {
                ALOGE("UidPolicy: Failed to remove uid from observer: 0x%08x", res);
            }
        } else if (closeCamera) {
            it->second.hasCamera = false;
        }
    } else {
        ALOGE("%s: Trying to unregister uid: %d which is not monitored!", __FUNCTION__, uid);
    }
}
bool CameraService::UidPolicy::isUidActive(uid_t uid, const std::string &callingPackage) {
    Mutex::Autolock _l(mUidLock);
    return isUidActiveLocked(uid, callingPackage);
}
static const int64_t kPollUidActiveTimeoutTotalMillis = 300;
static const int64_t kPollUidActiveTimeoutMillis = 50;
bool CameraService::UidPolicy::isUidActiveLocked(uid_t uid, const std::string &callingPackage) {
    if (uid < FIRST_APPLICATION_UID || !mRegistered) {
        return true;
    }
    auto it = mOverrideUids.find(uid);
    if (it != mOverrideUids.end()) {
        return it->second;
    }
    bool active = mActiveUids.find(uid) != mActiveUids.end();
    if (!active) {
        ActivityManager am;
        int64_t startTimeMillis = 0;
        do {
            active = mActiveUids.find(uid) != mActiveUids.end();
            if (!active) active = am.isUidActive(uid, toString16(callingPackage));
            if (active) {
                break;
            }
            if (startTimeMillis <= 0) {
                startTimeMillis = uptimeMillis();
            }
            int64_t ellapsedTimeMillis = uptimeMillis() - startTimeMillis;
            int64_t remainingTimeMillis = kPollUidActiveTimeoutTotalMillis - ellapsedTimeMillis;
            if (remainingTimeMillis <= 0) {
                break;
            }
            remainingTimeMillis = std::min(kPollUidActiveTimeoutMillis, remainingTimeMillis);
            mUidLock.unlock();
            usleep(remainingTimeMillis * 1000);
            mUidLock.lock();
        } while (true);
        if (active) {
            mActiveUids.insert(uid);
        }
    }
    return active;
}
int32_t CameraService::UidPolicy::getProcState(uid_t uid) {
    Mutex::Autolock _l(mUidLock);
    return getProcStateLocked(uid);
}
int32_t CameraService::UidPolicy::getProcStateLocked(uid_t uid) {
    int32_t procState = ActivityManager::PROCESS_STATE_UNKNOWN;
    if (mMonitoredUids.find(uid) != mMonitoredUids.end()) {
        procState = mMonitoredUids[uid].procState;
    }
    return procState;
}
void CameraService::UidPolicy::addOverrideUid(uid_t uid,
        const std::string &callingPackage, bool active) {
    updateOverrideUid(uid, callingPackage, active, true);
}
void CameraService::UidPolicy::removeOverrideUid(uid_t uid, const std::string &callingPackage) {
    updateOverrideUid(uid, callingPackage, false, false);
}
void CameraService::UidPolicy::binderDied(const wp<IBinder>& ) {
    Mutex::Autolock _l(mUidLock);
    ALOGV("UidPolicy: ActivityManager has died");
    mRegistered = false;
    mActiveUids.clear();
}
void CameraService::UidPolicy::updateOverrideUid(uid_t uid, const std::string &callingPackage,
        bool active, bool insert) {
    bool wasActive = false;
    bool isActive = false;
    {
        Mutex::Autolock _l(mUidLock);
        wasActive = isUidActiveLocked(uid, callingPackage);
        mOverrideUids.erase(uid);
        if (insert) {
            mOverrideUids.insert(std::pair<uid_t, bool>(uid, active));
        }
        isActive = isUidActiveLocked(uid, callingPackage);
    }
    if (wasActive != isActive && !isActive) {
        sp<CameraService> service = mService.promote();
        if (service != nullptr) {
            service->blockClientsForUid(uid);
        }
    }
}
void CameraService::SensorPrivacyPolicy::registerWithSensorPrivacyManager()
{
    Mutex::Autolock _l(mSensorPrivacyLock);
    if (mRegistered) {
        return;
    }
    hasCameraPrivacyFeature();
    mSpm.addSensorPrivacyListener(this);
    if (isAutomotiveDevice()) {
        mSpm.addToggleSensorPrivacyListener(this);
    }
    mSensorPrivacyEnabled = mSpm.isSensorPrivacyEnabled();
    if (flags::camera_privacy_allowlist()) {
        mCameraPrivacyState = mSpm.getToggleSensorPrivacyState(
                SensorPrivacyManager::TOGGLE_TYPE_SOFTWARE,
                SensorPrivacyManager::TOGGLE_SENSOR_CAMERA);
    }
    status_t res = mSpm.linkToDeath(this);
    if (res == OK) {
        mRegistered = true;
        ALOGV("SensorPrivacyPolicy: Registered with SensorPrivacyManager");
    }
}
void CameraService::SensorPrivacyPolicy::onServiceRegistration(const String16& name,
                                                               const sp<IBinder>&) {
    if (name != toString16(kSensorPrivacyServiceName)) {
        return;
    }
    registerWithSensorPrivacyManager();
}
void CameraService::SensorPrivacyPolicy::registerSelf() {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->checkService(toString16(kSensorPrivacyServiceName));
    if (!binder) {
        sm->registerForNotifications(toString16(kSensorPrivacyServiceName),this);
    } else {
        registerWithSensorPrivacyManager();
    }
}
void CameraService::SensorPrivacyPolicy::unregisterSelf() {
    Mutex::Autolock _l(mSensorPrivacyLock);
    mSpm.removeSensorPrivacyListener(this);
    if (isAutomotiveDevice()) {
        mSpm.removeToggleSensorPrivacyListener(this);
    }
    mSpm.unlinkToDeath(this);
    mRegistered = false;
    ALOGV("SensorPrivacyPolicy: Unregistered with SensorPrivacyManager");
}
bool CameraService::SensorPrivacyPolicy::isSensorPrivacyEnabled() {
    if (!mRegistered) {
      registerWithSensorPrivacyManager();
    }
    Mutex::Autolock _l(mSensorPrivacyLock);
    return mSensorPrivacyEnabled;
}
int CameraService::SensorPrivacyPolicy::getCameraPrivacyState() {
    if (!mRegistered) {
        registerWithSensorPrivacyManager();
    }
    Mutex::Autolock _l(mSensorPrivacyLock);
    return mCameraPrivacyState;
}
bool CameraService::SensorPrivacyPolicy::isCameraPrivacyEnabled() {
    if (!hasCameraPrivacyFeature()) {
        return false;
    }
    return mSpm.isToggleSensorPrivacyEnabled(SensorPrivacyManager::TOGGLE_SENSOR_CAMERA);
}
bool CameraService::SensorPrivacyPolicy::isCameraPrivacyEnabled(const String16& packageName) {
    if (!hasCameraPrivacyFeature()) {
        return SensorPrivacyManager::DISABLED;
    }
    return mSpm.isCameraPrivacyEnabled(packageName);
}
binder::Status CameraService::SensorPrivacyPolicy::onSensorPrivacyChanged(
    int toggleType, int sensor, bool enabled) {
    if ((toggleType == SensorPrivacyManager::TOGGLE_TYPE_UNKNOWN)
            && (sensor == SensorPrivacyManager::TOGGLE_SENSOR_UNKNOWN)) {
        {
            Mutex::Autolock _l(mSensorPrivacyLock);
            mSensorPrivacyEnabled = enabled;
        }
        if (enabled) {
            sp<CameraService> service = mService.promote();
            if (service != nullptr) {
                service->blockAllClients();
            }
        }
    }
    return binder::Status::ok();
}
binder::Status CameraService::SensorPrivacyPolicy::onSensorPrivacyStateChanged(
    int, int sensor, int state) {
    if (!flags::camera_privacy_allowlist()
            || (sensor != SensorPrivacyManager::TOGGLE_SENSOR_CAMERA)) {
        return binder::Status::ok();
    }
    {
        Mutex::Autolock _l(mSensorPrivacyLock);
        mCameraPrivacyState = state;
    }
    sp<CameraService> service = mService.promote();
    if (!service) {
        return binder::Status::ok();
    }
    if (state == SensorPrivacyManager::ENABLED) {
        service->blockAllClients();
    } else if (state == SensorPrivacyManager::ENABLED_EXCEPT_ALLOWLISTED_APPS) {
        service->blockPrivacyEnabledClients();
    }
    return binder::Status::ok();
}
void CameraService::SensorPrivacyPolicy::binderDied(const wp<IBinder>& ) {
    Mutex::Autolock _l(mSensorPrivacyLock);
    ALOGV("SensorPrivacyPolicy: SensorPrivacyManager has died");
    mRegistered = false;
}
bool CameraService::SensorPrivacyPolicy::hasCameraPrivacyFeature() {
    bool supportsSoftwareToggle = mSpm.supportsSensorToggle(
            SensorPrivacyManager::TOGGLE_TYPE_SOFTWARE, SensorPrivacyManager::TOGGLE_SENSOR_CAMERA);
    bool supportsHardwareToggle = mSpm.supportsSensorToggle(
            SensorPrivacyManager::TOGGLE_TYPE_HARDWARE, SensorPrivacyManager::TOGGLE_SENSOR_CAMERA);
    return supportsSoftwareToggle || supportsHardwareToggle;
}
CameraService::CameraState::CameraState(const std::string& id, int cost,
        const std::set<std::string>& conflicting, SystemCameraKind systemCameraKind,
        const std::vector<std::string>& physicalCameras) : mId(id),
        mStatus(StatusInternal::NOT_PRESENT), mCost(cost), mConflicting(conflicting),
        mSystemCameraKind(systemCameraKind), mPhysicalCameras(physicalCameras) {}
CameraService::CameraState::~CameraState() {}
CameraService::StatusInternal CameraService::CameraState::getStatus() const {
    Mutex::Autolock lock(mStatusLock);
    return mStatus;
}
std::vector<std::string> CameraService::CameraState::getUnavailablePhysicalIds() const {
    Mutex::Autolock lock(mStatusLock);
    std::vector<std::string> res(mUnavailablePhysicalIds.begin(), mUnavailablePhysicalIds.end());
    return res;
}
CameraParameters CameraService::CameraState::getShimParams() const {
    return mShimParams;
}
void CameraService::CameraState::setShimParams(const CameraParameters& params) {
    mShimParams = params;
}
int CameraService::CameraState::getCost() const {
    return mCost;
}
std::set<std::string> CameraService::CameraState::getConflicting() const {
    return mConflicting;
}
SystemCameraKind CameraService::CameraState::getSystemCameraKind() const {
    return mSystemCameraKind;
}
bool CameraService::CameraState::containsPhysicalCamera(const std::string& physicalCameraId) const {
    return std::find(mPhysicalCameras.begin(), mPhysicalCameras.end(), physicalCameraId)
            != mPhysicalCameras.end();
}
bool CameraService::CameraState::addUnavailablePhysicalId(const std::string& physicalId) {
    Mutex::Autolock lock(mStatusLock);
    auto result = mUnavailablePhysicalIds.insert(physicalId);
    return result.second;
}
bool CameraService::CameraState::removeUnavailablePhysicalId(const std::string& physicalId) {
    Mutex::Autolock lock(mStatusLock);
    auto count = mUnavailablePhysicalIds.erase(physicalId);
    return count > 0;
}
void CameraService::CameraState::setClientPackage(const std::string& clientPackage) {
    Mutex::Autolock lock(mStatusLock);
    mClientPackage = clientPackage;
}
std::string CameraService::CameraState::getClientPackage() const {
    Mutex::Autolock lock(mStatusLock);
    return mClientPackage;
}
void CameraService::ClientEventListener::onClientAdded(
        const resource_policy::ClientDescriptor<std::string,
        sp<CameraService::BasicClient>>& descriptor) {
    const auto& basicClient = descriptor.getValue();
    if (basicClient.get() != nullptr) {
        BatteryNotifier& notifier(BatteryNotifier::getInstance());
        notifier.noteStartCamera(toString8(descriptor.getKey()),
                static_cast<int>(basicClient->getClientUid()));
    }
}
void CameraService::ClientEventListener::onClientRemoved(
        const resource_policy::ClientDescriptor<std::string,
        sp<CameraService::BasicClient>>& descriptor) {
    const auto& basicClient = descriptor.getValue();
    if (basicClient.get() != nullptr) {
        BatteryNotifier& notifier(BatteryNotifier::getInstance());
        notifier.noteStopCamera(toString8(descriptor.getKey()),
                static_cast<int>(basicClient->getClientUid()));
    }
}
CameraService::CameraClientManager::CameraClientManager() {
    setListener(std::make_shared<ClientEventListener>());
}
CameraService::CameraClientManager::~CameraClientManager() {}
sp<CameraService::BasicClient> CameraService::CameraClientManager::getCameraClient(
        const std::string& id) const {
    auto descriptor = get(id);
    if (descriptor == nullptr) {
        return sp<BasicClient>{nullptr};
    }
    return descriptor->getValue();
}
std::string CameraService::CameraClientManager::toString() const {
    auto all = getAll();
    std::ostringstream ret;
    ret << "[";
    bool hasAny = false;
    for (auto& i : all) {
        hasAny = true;
        std::string key = i->getKey();
        int32_t cost = i->getCost();
        int32_t pid = i->getOwnerId();
        int32_t score = i->getPriority().getScore();
        int32_t state = i->getPriority().getState();
        auto conflicting = i->getConflicting();
        auto clientSp = i->getValue();
        std::string packageName;
        userid_t clientUserId = 0;
        if (clientSp.get() != nullptr) {
            packageName = clientSp->getPackageName();
            uid_t clientUid = clientSp->getClientUid();
            clientUserId = multiuser_get_user_id(clientUid);
        }
        ret << fmt::sprintf("\n(Camera ID: %s, Cost: %" PRId32 ", PID: %" PRId32 ", Score: %"
                PRId32 ", State: %" PRId32, key.c_str(), cost, pid, score, state);
        if (clientSp.get() != nullptr) {
            ret << fmt::sprintf("User Id: %d, ", clientUserId);
        }
        if (packageName.size() != 0) {
            ret << fmt::sprintf("Client Package Name: %s", packageName.c_str());
        }
        ret << ", Conflicting Client Devices: {";
        for (auto& j : conflicting) {
            ret << fmt::sprintf("%s, ", j.c_str());
        }
        ret << "})";
    }
    if (hasAny) ret << "\n";
    ret << "]\n";
    return std::move(ret.str());
}
CameraService::DescriptorPtr CameraService::CameraClientManager::makeClientDescriptor(
        const std::string& key, const sp<BasicClient>& value, int32_t cost,
        const std::set<std::string>& conflictingKeys, int32_t score, int32_t ownerId,
        int32_t state, int32_t oomScoreOffset, bool systemNativeClient) {
    int32_t score_adj = systemNativeClient ? kSystemNativeClientScore : score;
    int32_t state_adj = systemNativeClient ? kSystemNativeClientState : state;
    return std::make_shared<resource_policy::ClientDescriptor<std::string, sp<BasicClient>>>(
            key, value, cost, conflictingKeys, score_adj, ownerId, state_adj,
            systemNativeClient, oomScoreOffset);
}
CameraService::DescriptorPtr CameraService::CameraClientManager::makeClientDescriptor(
        const sp<BasicClient>& value, const CameraService::DescriptorPtr& partial,
        int32_t oomScoreOffset, bool systemNativeClient) {
    return makeClientDescriptor(partial->getKey(), value, partial->getCost(),
            partial->getConflicting(), partial->getPriority().getScore(),
            partial->getOwnerId(), partial->getPriority().getState(), oomScoreOffset,
            systemNativeClient);
}
void CameraService::InjectionStatusListener::addListener(
        const sp<ICameraInjectionCallback>& callback) {
    Mutex::Autolock lock(mListenerLock);
    if (mCameraInjectionCallback) return;
    status_t res = IInterface::asBinder(callback)->linkToDeath(this);
    if (res == OK) {
        mCameraInjectionCallback = callback;
    }
}
void CameraService::InjectionStatusListener::removeListener() {
    Mutex::Autolock lock(mListenerLock);
    if (mCameraInjectionCallback == nullptr) {
        ALOGW("InjectionStatusListener: mCameraInjectionCallback == nullptr");
        return;
    }
    IInterface::asBinder(mCameraInjectionCallback)->unlinkToDeath(this);
    mCameraInjectionCallback = nullptr;
}
void CameraService::InjectionStatusListener::notifyInjectionError(
        const std::string &injectedCamId, status_t err) {
    if (mCameraInjectionCallback == nullptr) {
        ALOGW("InjectionStatusListener: mCameraInjectionCallback == nullptr");
        return;
    }
    switch (err) {
        case -ENODEV:
            mCameraInjectionCallback->onInjectionError(
                    ICameraInjectionCallback::ERROR_INJECTION_SESSION);
            ALOGE("No camera device with ID \"%s\" currently available!",
                    injectedCamId.c_str());
            break;
        case -EBUSY:
            mCameraInjectionCallback->onInjectionError(
                    ICameraInjectionCallback::ERROR_INJECTION_SESSION);
            ALOGE("Higher-priority client using camera, ID \"%s\" currently unavailable!",
                    injectedCamId.c_str());
            break;
        case DEAD_OBJECT:
            mCameraInjectionCallback->onInjectionError(
                    ICameraInjectionCallback::ERROR_INJECTION_SESSION);
            ALOGE("Camera ID \"%s\" object is dead!",
                    injectedCamId.c_str());
            break;
        case INVALID_OPERATION:
            mCameraInjectionCallback->onInjectionError(
                    ICameraInjectionCallback::ERROR_INJECTION_SESSION);
            ALOGE("Camera ID \"%s\" encountered an operating or internal error!",
                    injectedCamId.c_str());
            break;
        case UNKNOWN_TRANSACTION:
            mCameraInjectionCallback->onInjectionError(
                    ICameraInjectionCallback::ERROR_INJECTION_UNSUPPORTED);
            ALOGE("Camera ID \"%s\" method doesn't support!",
                    injectedCamId.c_str());
            break;
        default:
            mCameraInjectionCallback->onInjectionError(
                    ICameraInjectionCallback::ERROR_INJECTION_INVALID_ERROR);
            ALOGE("Unexpected error %s (%d) opening camera \"%s\"!",
                    strerror(-err), err, injectedCamId.c_str());
    }
}
void CameraService::InjectionStatusListener::binderDied(
        const wp<IBinder>& ) {
    ALOGV("InjectionStatusListener: ICameraInjectionCallback has died");
    auto parent = mParent.promote();
    if (parent != nullptr) {
        auto clientDescriptor = parent->mActiveClientManager.get(parent->mInjectionInternalCamId);
        if (clientDescriptor != nullptr) {
            BasicClient* baseClientPtr = clientDescriptor->getValue().get();
            baseClientPtr->stopInjection();
        }
        parent->clearInjectionParameters();
    }
}
binder::Status CameraService::CameraInjectionSession::stopInjection() {
    Mutex::Autolock lock(mInjectionSessionLock);
    auto parent = mParent.promote();
    if (parent == nullptr) {
        ALOGE("CameraInjectionSession: Parent is gone");
        return STATUS_ERROR(ICameraInjectionCallback::ERROR_INJECTION_SERVICE,
                "Camera service encountered error");
    }
    status_t res = NO_ERROR;
    auto clientDescriptor = parent->mActiveClientManager.get(parent->mInjectionInternalCamId);
    if (clientDescriptor != nullptr) {
        BasicClient* baseClientPtr = clientDescriptor->getValue().get();
        res = baseClientPtr->stopInjection();
        if (res != OK) {
            ALOGE("CameraInjectionSession: Failed to stop the injection camera!"
                " ret != NO_ERROR: %d", res);
            return STATUS_ERROR(ICameraInjectionCallback::ERROR_INJECTION_SESSION,
                "Camera session encountered error");
        }
    }
    parent->clearInjectionParameters();
    return binder::Status::ok();
}
static const int kDumpLockRetries = 50;
static const int kDumpLockSleep = 60000;
static bool tryLock(Mutex& mutex)
{
    bool locked = false;
    for (int i = 0; i < kDumpLockRetries; ++i) {
        if (mutex.tryLock() == NO_ERROR) {
            locked = true;
            break;
        }
        usleep(kDumpLockSleep);
    }
    return locked;
}
void CameraService::cacheDump() {
    if (mMemFd != -1) {
        const Vector<String16> args;
        ATRACE_CALL();
        Mutex::Autolock lock(mServiceLock);
        Mutex::Autolock l(mCameraStatesLock);
        for (const auto& state : mCameraStates) {
            std::string cameraId = state.first;
            auto clientDescriptor = mActiveClientManager.get(cameraId);
            if (clientDescriptor != nullptr) {
                dprintf(mMemFd, "== Camera device %s dynamic info: ==\n", cameraId.c_str());
                dumpOpenSessionClientLogs(mMemFd, args, cameraId);
            }
        }
    }
}
status_t CameraService::dump(int fd, const Vector<String16>& args) {
    ATRACE_CALL();
    if (checkCallingPermission(toString16(sDumpPermission)) == false) {
        dprintf(fd, "Permission Denial: can't dump CameraService from pid=%d, uid=%d\n",
                CameraThreadState::getCallingPid(),
                CameraThreadState::getCallingUid());
        return NO_ERROR;
    }
    bool locked = tryLock(mServiceLock);
    if (!locked) {
        dprintf(fd, "!! CameraService may be deadlocked !!\n");
    }
    if (!mInitialized) {
        dprintf(fd, "!! No camera HAL available !!\n");
        dumpEventLog(fd);
        if (locked) mServiceLock.unlock();
        return NO_ERROR;
    }
    dprintf(fd, "\n== Service global info: ==\n\n");
    dprintf(fd, "Number of camera devices: %d\n", mNumberOfCameras);
    dprintf(fd, "Number of normal camera devices: %zu\n", mNormalDeviceIds.size());
    dprintf(fd, "Number of public camera devices visible to API1: %zu\n",
            mNormalDeviceIdsWithoutSystemCamera.size());
    for (size_t i = 0; i < mNormalDeviceIds.size(); i++) {
        dprintf(fd, "    Device %zu maps to \"%s\"\n", i, mNormalDeviceIds[i].c_str());
    }
    std::string activeClientString = mActiveClientManager.toString();
    dprintf(fd, "Active Camera Clients:\n%s", activeClientString.c_str());
    dprintf(fd, "Allowed user IDs: %s\n", toString(mAllowedUsers).c_str());
    if (mStreamUseCaseOverrides.size() > 0) {
        dprintf(fd, "Active stream use case overrides:");
        for (int64_t useCaseOverride : mStreamUseCaseOverrides) {
            dprintf(fd, " %" PRId64, useCaseOverride);
        }
        dprintf(fd, "\n");
    }
    dumpEventLog(fd);
    bool stateLocked = tryLock(mCameraStatesLock);
    if (!stateLocked) {
        dprintf(fd, "CameraStates in use, may be deadlocked\n");
    }
    int argSize = args.size();
    for (int i = 0; i < argSize; i++) {
        if (args[i] == toString16(TagMonitor::kMonitorOption)) {
            if (i + 1 < argSize) {
                mMonitorTags = toStdString(args[i + 1]);
            }
            break;
        }
    }
    for (auto& state : mCameraStates) {
        const std::string &cameraId = state.first;
        dprintf(fd, "== Camera device %s dynamic info: ==\n", cameraId.c_str());
        CameraParameters p = state.second->getShimParams();
        if (!p.isEmpty()) {
            dprintf(fd, "  Camera1 API shim is using parameters:\n        ");
            p.dump(fd, args);
        }
        auto clientDescriptor = mActiveClientManager.get(cameraId);
        if (clientDescriptor != nullptr) {
            dumpOpenSessionClientLogs(fd, args, cameraId);
        } else {
            dumpClosedSessionClientLogs(fd, cameraId);
        }
    }
    if (stateLocked) mCameraStatesLock.unlock();
    if (locked) mServiceLock.unlock();
    mCameraProviderManager->dump(fd, args);
    dprintf(fd, "\n== Vendor tags: ==\n\n");
    sp<VendorTagDescriptor> desc = VendorTagDescriptor::getGlobalVendorTagDescriptor();
    if (desc == NULL) {
        sp<VendorTagDescriptorCache> cache =
                VendorTagDescriptorCache::getGlobalVendorTagCache();
        if (cache == NULL) {
            dprintf(fd, "No vendor tags.\n");
        } else {
            cache->dump(fd, 2, 2);
        }
    } else {
        desc->dump(fd, 2, 2);
    }
    dprintf(fd, "\n");
    camera3::CameraTraces::dump(fd);
    int n = args.size();
    String16 verboseOption("-v");
    String16 unreachableOption("--unreachable");
    for (int i = 0; i < n; i++) {
        if (args[i] == verboseOption) {
            if (i + 1 >= n) continue;
            std::string levelStr = toStdString(args[i+1]);
            int level = atoi(levelStr.c_str());
            dprintf(fd, "\nSetting log level to %d.\n", level);
            setLogLevel(level);
        } else if (args[i] == unreachableOption) {
            UnreachableMemoryInfo info;
            bool success = GetUnreachableMemory(info, 10000);
            if (!success) {
                dprintf(fd, "\n== Unable to dump unreachable memory. "
                        "Try disabling SELinux enforcement. ==\n");
            } else {
                dprintf(fd, "\n== Dumping unreachable memory: ==\n");
                std::string s = info.ToString( true);
                write(fd, s.c_str(), s.size());
            }
        }
    }
    bool serviceLocked = tryLock(mServiceLock);
    if ((mMemFd >= 0) && (lseek(mMemFd, 0, SEEK_SET) != -1)) {
        dprintf(fd, "\n**********Dumpsys from previous open session**********\n");
        ssize_t size_read;
        char buf[4096];
        while ((size_read = read(mMemFd, buf, (sizeof(buf) - 1))) > 0) {
            write(fd, buf, size_read);
            if (size_read == -1) {
                ALOGE("%s: Error during reading the file: %s", __FUNCTION__, sFileName);
                break;
            }
        }
        dprintf(fd, "\n**********End of Dumpsys from previous open session**********\n");
    } else {
        ALOGE("%s: Error during reading the file: %s", __FUNCTION__, sFileName);
    }
    if (serviceLocked) mServiceLock.unlock();
    return NO_ERROR;
}
void CameraService::dumpOpenSessionClientLogs(int fd,
        const Vector<String16>& args, const std::string& cameraId) {
    auto clientDescriptor = mActiveClientManager.get(cameraId);
    dprintf(fd, "  %s : Device %s is open. Client instance dump:\n",
            getFormattedCurrentTime().c_str(),
            cameraId.c_str());
    dprintf(fd, "    Client priority score: %d state: %d\n",
        clientDescriptor->getPriority().getScore(),
        clientDescriptor->getPriority().getState());
    dprintf(fd, "    Client PID: %d\n", clientDescriptor->getOwnerId());
    auto client = clientDescriptor->getValue();
    dprintf(fd, "    Client package: %s\n",
        client->getPackageName().c_str());
    client->dumpClient(fd, args);
}
void CameraService::dumpClosedSessionClientLogs(int fd, const std::string& cameraId) {
    dprintf(fd, "  Device %s is closed, no client instance\n",
                    cameraId.c_str());
}
void CameraService::dumpEventLog(int fd) {
    dprintf(fd, "\n== Camera service events log (most recent at top): ==\n");
    Mutex::Autolock l(mLogLock);
    for (const auto& msg : mEventLog) {
        dprintf(fd, "  %s\n", msg.c_str());
    }
    if (mEventLog.size() == DEFAULT_EVENT_LOG_LENGTH) {
        dprintf(fd, "  ...\n");
    } else if (mEventLog.size() == 0) {
        dprintf(fd, "  [no events yet]\n");
    }
    dprintf(fd, "\n");
}
void CameraService::cacheClientTagDumpIfNeeded(const std::string &cameraId, BasicClient* client) {
    Mutex::Autolock lock(mLogLock);
    if (!isClientWatchedLocked(client)) { return; }
    std::vector<std::string> dumpVector;
    client->dumpWatchedEventsToVector(dumpVector);
    if (dumpVector.empty()) { return; }
    std::ostringstream dumpString;
    std::string currentTime = getFormattedCurrentTime();
    dumpString << "Cached @ ";
    dumpString << currentTime;
    dumpString << "\n";
    size_t i = dumpVector.size();
    while (i > 0) {
         i--;
         dumpString << cameraId;
         dumpString << ":";
         dumpString << client->getPackageName();
         dumpString << "  ";
         dumpString << dumpVector[i];
    }
    mWatchedClientsDumpCache[client->getPackageName()] = dumpString.str();
}
void CameraService::handleTorchClientBinderDied(const wp<IBinder> &who) {
    Mutex::Autolock al(mTorchClientMapMutex);
    for (size_t i = 0; i < mTorchClientMap.size(); i++) {
        if (mTorchClientMap[i] == who) {
            std::string cameraId = mTorchClientMap.keyAt(i);
            status_t res = mFlashlight->setTorchMode(cameraId, false);
            if (res) {
                ALOGE("%s: torch client died but couldn't turn off torch: "
                    "%s (%d)", __FUNCTION__, strerror(-res), res);
                return;
            }
            mTorchClientMap.removeItemsAt(i);
            break;
        }
    }
}
           void CameraService::binderDied(const wp<IBinder> &who) {
    logClientDied(CameraThreadState::getCallingPid(), "Binder died unexpectedly");
    handleTorchClientBinderDied(who);
    if(!evictClientIdByRemote(who)) {
        ALOGV("%s: Java client's binder death already cleaned up (normal case)", __FUNCTION__);
        return;
    }
    ALOGE("%s: Java client's binder died, removing it from the list of active clients",
            __FUNCTION__);
}
void CameraService::updateStatus(StatusInternal status, const std::string& cameraId) {
    updateStatus(status, cameraId, {});
}
void CameraService::updateStatus(StatusInternal status, const std::string& cameraId,
        std::initializer_list<StatusInternal> rejectSourceStates) {
    auto state = getCameraState(cameraId);
    if (state == nullptr) {
        ALOGW("%s: Could not update the status for %s, no such device exists", __FUNCTION__,
                cameraId.c_str());
        return;
    }
    SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
    if (getSystemCameraKind(cameraId, &deviceKind) != OK) {
        ALOGE("%s: Invalid camera id %s, skipping", __FUNCTION__, cameraId.c_str());
        return;
    }
    auto logicalCameraIds = getLogicalCameras(cameraId);
    state->updateStatus(status, cameraId, rejectSourceStates, [this, &deviceKind,
                        &logicalCameraIds]
            (const std::string& cameraId, StatusInternal status) {
            if (status != StatusInternal::ENUMERATING) {
                Mutex::Autolock al(mTorchStatusMutex);
                TorchModeStatus torchStatus;
                if (getTorchStatusLocked(cameraId, &torchStatus) !=
                        NAME_NOT_FOUND) {
                    TorchModeStatus newTorchStatus =
                            status == StatusInternal::PRESENT ?
                            TorchModeStatus::AVAILABLE_OFF :
                            TorchModeStatus::NOT_AVAILABLE;
                    if (torchStatus != newTorchStatus) {
                        onTorchStatusChangedLocked(cameraId, newTorchStatus, deviceKind);
                    }
                }
            }
            Mutex::Autolock lock(mStatusListenerLock);
            notifyPhysicalCameraStatusLocked(mapToInterface(status), cameraId,
                    logicalCameraIds, deviceKind);
            for (auto& listener : mListenerList) {
                bool isVendorListener = listener->isVendorListener();
                if (shouldSkipStatusUpdates(deviceKind, isVendorListener,
                        listener->getListenerPid(), listener->getListenerUid())) {
                    ALOGV("Skipping discovery callback for system-only camera device %s",
                            cameraId.c_str());
                    continue;
                }
                auto ret = listener->getListener()->onStatusChanged(mapToInterface(status),
                        cameraId);
                listener->handleBinderStatus(ret,
                         "%s: Failed to trigger onStatusChanged callback for %d:%d: %d",
                        __FUNCTION__, listener->getListenerUid(), listener->getListenerPid(),
                        ret.exceptionCode());
                std::vector<std::string> remappedCameraIds =
                        findOriginalIdsForRemappedCameraId(cameraId, listener->getListenerUid());
                for (auto& remappedCameraId : remappedCameraIds) {
                    ret = listener->getListener()->onStatusChanged(
                            mapToInterface(status), remappedCameraId);
                    listener->handleBinderStatus(ret,
                             "%s: Failed to trigger onStatusChanged callback for %d:%d: %d",
                            __FUNCTION__, listener->getListenerUid(), listener->getListenerPid(),
                            ret.exceptionCode());
                }
            }
        });
}
void CameraService::updateOpenCloseStatus(const std::string& cameraId, bool open,
        const std::string& clientPackageName) {
    auto state = getCameraState(cameraId);
    if (state == nullptr) {
        ALOGW("%s: Could not update the status for %s, no such device exists", __FUNCTION__,
                cameraId.c_str());
        return;
    }
    if (open) {
        state->setClientPackage(clientPackageName);
    } else {
        state->setClientPackage(std::string());
    }
    Mutex::Autolock lock(mStatusListenerLock);
    for (const auto& it : mListenerList) {
        if (!it->isOpenCloseCallbackAllowed()) {
            continue;
        }
        binder::Status ret;
        if (open) {
            ret = it->getListener()->onCameraOpened(cameraId, clientPackageName);
        } else {
            ret = it->getListener()->onCameraClosed(cameraId);
        }
        it->handleBinderStatus(ret,
                "%s: Failed to trigger onCameraOpened/onCameraClosed callback for %d:%d: %d",
                __FUNCTION__, it->getListenerUid(), it->getListenerPid(), ret.exceptionCode());
    }
}
template<class Func>
void CameraService::CameraState::updateStatus(StatusInternal status,
        const std::string& cameraId,
        std::initializer_list<StatusInternal> rejectSourceStates,
        Func onStatusUpdatedLocked) {
    Mutex::Autolock lock(mStatusLock);
    StatusInternal oldStatus = mStatus;
    mStatus = status;
    if (oldStatus == status) {
        return;
    }
    ALOGV("%s: Status has changed for camera ID %s from %#x to %#x", __FUNCTION__,
            cameraId.c_str(), oldStatus, status);
    if (oldStatus == StatusInternal::NOT_PRESENT &&
            (status != StatusInternal::PRESENT &&
             status != StatusInternal::ENUMERATING)) {
        ALOGW("%s: From NOT_PRESENT can only transition into PRESENT or ENUMERATING",
                __FUNCTION__);
        mStatus = oldStatus;
        return;
    }
    for (auto& rejectStatus : rejectSourceStates) {
        if (oldStatus == rejectStatus) {
            ALOGV("%s: Rejecting status transition for Camera ID %s,  since the source "
                    "state was was in one of the bad states.", __FUNCTION__, cameraId.c_str());
            mStatus = oldStatus;
            return;
        }
    }
    onStatusUpdatedLocked(cameraId, status);
}
status_t CameraService::getTorchStatusLocked(
        const std::string& cameraId,
        TorchModeStatus *status) const {
    if (!status) {
        return BAD_VALUE;
    }
    ssize_t index = mTorchStatusMap.indexOfKey(cameraId);
    if (index == NAME_NOT_FOUND) {
        return NAME_NOT_FOUND;
    }
    *status = mTorchStatusMap.valueAt(index);
    return OK;
}
status_t CameraService::setTorchStatusLocked(const std::string& cameraId,
        TorchModeStatus status) {
    ssize_t index = mTorchStatusMap.indexOfKey(cameraId);
    if (index == NAME_NOT_FOUND) {
        return BAD_VALUE;
    }
    mTorchStatusMap.editValueAt(index) = status;
    return OK;
}
std::list<std::string> CameraService::getLogicalCameras(
        const std::string& physicalCameraId) {
    std::list<std::string> retList;
    Mutex::Autolock lock(mCameraStatesLock);
    for (const auto& state : mCameraStates) {
        if (state.second->containsPhysicalCamera(physicalCameraId)) {
            retList.emplace_back(state.first);
        }
    }
    return retList;
}
void CameraService::notifyPhysicalCameraStatusLocked(int32_t status,
        const std::string& physicalCameraId, const std::list<std::string>& logicalCameraIds,
        SystemCameraKind deviceKind) {
    for (const auto& logicalCameraId : logicalCameraIds) {
        for (auto& listener : mListenerList) {
            if (shouldSkipStatusUpdates(deviceKind, listener->isVendorListener(),
                    listener->getListenerPid(), listener->getListenerUid())) {
                ALOGV("Skipping discovery callback for system-only camera device %s",
                        physicalCameraId.c_str());
                continue;
            }
            auto ret = listener->getListener()->onPhysicalCameraStatusChanged(status,
                    logicalCameraId, physicalCameraId);
            listener->handleBinderStatus(ret,
                    "%s: Failed to trigger onPhysicalCameraStatusChanged for %d:%d: %d",
                    __FUNCTION__, listener->getListenerUid(), listener->getListenerPid(),
                    ret.exceptionCode());
        }
    }
}
void CameraService::blockClientsForUid(uid_t uid) {
    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr && basicClient->getClientUid() == uid) {
                basicClient->block();
            }
        }
    }
}
void CameraService::blockAllClients() {
    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                basicClient->block();
            }
        }
    }
}
void CameraService::blockPrivacyEnabledClients() {
    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                std::string pkgName = basicClient->getPackageName();
                bool cameraPrivacyEnabled =
                        mSensorPrivacyPolicy->isCameraPrivacyEnabled(toString16(pkgName));
                if (cameraPrivacyEnabled) {
                    basicClient->block();
                }
           }
        }
    }
}
status_t CameraService::shellCommand(int in, int out, int err, const Vector<String16>& args) {
    if (!checkCallingPermission(toString16(sManageCameraPermission), nullptr, nullptr)) {
        return PERMISSION_DENIED;
    }
    if (in == BAD_TYPE || out == BAD_TYPE || err == BAD_TYPE) {
        return BAD_VALUE;
    }
    if (args.size() >= 3 && args[0] == toString16("set-uid-state")) {
        return handleSetUidState(args, err);
    } else if (args.size() >= 2 && args[0] == toString16("reset-uid-state")) {
        return handleResetUidState(args, err);
    } else if (args.size() >= 2 && args[0] == toString16("get-uid-state")) {
        return handleGetUidState(args, out, err);
    } else if (args.size() >= 2 && args[0] == toString16("set-rotate-and-crop")) {
        return handleSetRotateAndCrop(args);
    } else if (args.size() >= 1 && args[0] == toString16("get-rotate-and-crop")) {
        return handleGetRotateAndCrop(out);
    } else if (args.size() >= 2 && args[0] == toString16("set-autoframing")) {
        return handleSetAutoframing(args);
    } else if (args.size() >= 1 && args[0] == toString16("get-autoframing")) {
        return handleGetAutoframing(out);
    } else if (args.size() >= 2 && args[0] == toString16("set-image-dump-mask")) {
        return handleSetImageDumpMask(args);
    } else if (args.size() >= 1 && args[0] == toString16("get-image-dump-mask")) {
        return handleGetImageDumpMask(out);
    } else if (args.size() >= 2 && args[0] == toString16("set-camera-mute")) {
        return handleSetCameraMute(args);
    } else if (args.size() >= 2 && args[0] == toString16("set-stream-use-case-override")) {
        return handleSetStreamUseCaseOverrides(args);
    } else if (args.size() >= 1 && args[0] == toString16("clear-stream-use-case-override")) {
        handleClearStreamUseCaseOverrides();
        return OK;
    } else if (args.size() >= 1 && args[0] == toString16("set-zoom-override")) {
        return handleSetZoomOverride(args);
    } else if (args.size() >= 2 && args[0] == toString16("watch")) {
        return handleWatchCommand(args, in, out);
    } else if (args.size() >= 2 && args[0] == toString16("set-watchdog")) {
        return handleSetCameraServiceWatchdog(args);
    } else if (args.size() >= 4 && args[0] == toString16("remap-camera-id")) {
        return handleCameraIdRemapping(args, err);
    } else if (args.size() == 1 && args[0] == toString16("help")) {
        printHelp(out);
        return OK;
    }
    printHelp(err);
    return BAD_VALUE;
}
status_t CameraService::handleCameraIdRemapping(const Vector<String16>& args, int err) {
    uid_t uid = IPCThreadState::self()->getCallingUid();
    if (uid != AID_ROOT) {
        dprintf(err, "Must be adb root\n");
        return PERMISSION_DENIED;
    }
    if (args.size() != 4) {
        dprintf(err, "Expected format: remap-camera-id <PACKAGE> <Id0> <Id1>\n");
        return BAD_VALUE;
    }
    std::string packageName = toStdString(args[1]);
    std::string cameraIdToReplace = toStdString(args[2]);
    std::string cameraIdNew = toStdString(args[3]);
    remapCameraIds({{packageName, {{cameraIdToReplace, cameraIdNew}}}});
    return OK;
}
status_t CameraService::handleSetUidState(const Vector<String16>& args, int err) {
    std::string packageName = toStdString(args[1]);
    bool active = false;
    if (args[2] == toString16("active")) {
        active = true;
    } else if ((args[2] != toString16("idle"))) {
        ALOGE("Expected active or idle but got: '%s'", toStdString(args[2]).c_str());
        return BAD_VALUE;
    }
    int userId = 0;
    if (args.size() >= 5 && args[3] == toString16("--user")) {
        userId = atoi(toStdString(args[4]).c_str());
    }
    uid_t uid;
    if (getUidForPackage(packageName, userId, uid, err) == BAD_VALUE) {
        return BAD_VALUE;
    }
    mUidPolicy->addOverrideUid(uid, packageName, active);
    return NO_ERROR;
}
status_t CameraService::handleResetUidState(const Vector<String16>& args, int err) {
    std::string packageName = toStdString(args[1]);
    int userId = 0;
    if (args.size() >= 4 && args[2] == toString16("--user")) {
        userId = atoi(toStdString(args[3]).c_str());
    }
    uid_t uid;
    if (getUidForPackage(packageName, userId, uid, err) == BAD_VALUE) {
        return BAD_VALUE;
    }
    mUidPolicy->removeOverrideUid(uid, packageName);
    return NO_ERROR;
}
status_t CameraService::handleGetUidState(const Vector<String16>& args, int out, int err) {
    std::string packageName = toStdString(args[1]);
    int userId = 0;
    if (args.size() >= 4 && args[2] == toString16("--user")) {
        userId = atoi(toStdString(args[3]).c_str());
    }
    uid_t uid;
    if (getUidForPackage(packageName, userId, uid, err) == BAD_VALUE) {
        return BAD_VALUE;
    }
    if (mUidPolicy->isUidActive(uid, packageName)) {
        return dprintf(out, "active\n");
    } else {
        return dprintf(out, "idle\n");
    }
}
status_t CameraService::handleSetRotateAndCrop(const Vector<String16>& args) {
    int rotateValue = atoi(toStdString(args[1]).c_str());
    if (rotateValue < ANDROID_SCALER_ROTATE_AND_CROP_NONE ||
            rotateValue > ANDROID_SCALER_ROTATE_AND_CROP_AUTO) return BAD_VALUE;
    Mutex::Autolock lock(mServiceLock);
    mOverrideRotateAndCropMode = rotateValue;
    if (rotateValue == ANDROID_SCALER_ROTATE_AND_CROP_AUTO) return OK;
    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                basicClient->setRotateAndCropOverride(rotateValue);
            }
        }
    }
    return OK;
}
status_t CameraService::handleSetAutoframing(const Vector<String16>& args) {
    char* end;
    int autoframingValue = (int) strtol(toStdString(args[1]).c_str(), &end, 10);
    if ((*end != '\0') ||
            (autoframingValue != ANDROID_CONTROL_AUTOFRAMING_OFF &&
             autoframingValue != ANDROID_CONTROL_AUTOFRAMING_ON &&
             autoframingValue != ANDROID_CONTROL_AUTOFRAMING_AUTO)) {
        return BAD_VALUE;
    }
    Mutex::Autolock lock(mServiceLock);
    mOverrideAutoframingMode = autoframingValue;
    if (autoframingValue == ANDROID_CONTROL_AUTOFRAMING_AUTO) return OK;
    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                basicClient->setAutoframingOverride(autoframingValue);
            }
        }
    }
    return OK;
}
status_t CameraService::handleSetCameraServiceWatchdog(const Vector<String16>& args) {
    int enableWatchdog = atoi(toStdString(args[1]).c_str());
    if (enableWatchdog < 0 || enableWatchdog > 1) return BAD_VALUE;
    Mutex::Autolock lock(mServiceLock);
    mCameraServiceWatchdogEnabled = enableWatchdog;
    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                basicClient->setCameraServiceWatchdog(enableWatchdog);
            }
        }
    }
    return OK;
}
status_t CameraService::handleGetRotateAndCrop(int out) {
    Mutex::Autolock lock(mServiceLock);
    return dprintf(out, "rotateAndCrop override: %d\n", mOverrideRotateAndCropMode);
}
status_t CameraService::handleGetAutoframing(int out) {
    Mutex::Autolock lock(mServiceLock);
    return dprintf(out, "autoframing override: %d\n", mOverrideAutoframingMode);
}
status_t CameraService::handleSetImageDumpMask(const Vector<String16>& args) {
    char *endPtr;
    errno = 0;
    std::string maskString = toStdString(args[1]);
    long maskValue = strtol(maskString.c_str(), &endPtr, 10);
    if (errno != 0) return BAD_VALUE;
    if (endPtr != maskString.c_str() + maskString.size()) return BAD_VALUE;
    if (maskValue < 0 || maskValue > 1) return BAD_VALUE;
    Mutex::Autolock lock(mServiceLock);
    mImageDumpMask = maskValue;
    return OK;
}
status_t CameraService::handleGetImageDumpMask(int out) {
    Mutex::Autolock lock(mServiceLock);
    return dprintf(out, "Image dump mask: %d\n", mImageDumpMask);
}
status_t CameraService::handleSetCameraMute(const Vector<String16>& args) {
    int muteValue = strtol(toStdString(args[1]).c_str(), nullptr, 10);
    if (errno != 0) return BAD_VALUE;
    if (muteValue < 0 || muteValue > 1) return BAD_VALUE;
    Mutex::Autolock lock(mServiceLock);
    mOverrideCameraMuteMode = (muteValue == 1);
    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                if (basicClient->supportsCameraMute()) {
                    basicClient->setCameraMute(mOverrideCameraMuteMode);
                }
            }
        }
    }
    return OK;
}
status_t CameraService::handleSetStreamUseCaseOverrides(const Vector<String16>& args) {
    std::vector<int64_t> useCasesOverride;
    for (size_t i = 1; i < args.size(); i++) {
        int64_t useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_DEFAULT;
        std::string arg = toStdString(args[i]);
        if (arg == "DEFAULT") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_DEFAULT;
        } else if (arg == "PREVIEW") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_PREVIEW;
        } else if (arg == "STILL_CAPTURE") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_STILL_CAPTURE;
        } else if (arg == "VIDEO_RECORD") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_VIDEO_RECORD;
        } else if (arg == "PREVIEW_VIDEO_STILL") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_PREVIEW_VIDEO_STILL;
        } else if (arg == "VIDEO_CALL") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_VIDEO_CALL;
        } else if (arg == "CROPPED_RAW") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_CROPPED_RAW;
        } else {
            ALOGE("%s: Invalid stream use case %s", __FUNCTION__, arg.c_str());
            return BAD_VALUE;
        }
        useCasesOverride.push_back(useCase);
    }
    Mutex::Autolock lock(mServiceLock);
    mStreamUseCaseOverrides = std::move(useCasesOverride);
    return OK;
}
void CameraService::handleClearStreamUseCaseOverrides() {
    Mutex::Autolock lock(mServiceLock);
    mStreamUseCaseOverrides.clear();
}
status_t CameraService::handleSetZoomOverride(const Vector<String16>& args) {
    char* end;
    int zoomOverrideValue = strtol(toStdString(args[1]).c_str(), &end, 10);
    if ((*end != '\0') ||
            (zoomOverrideValue != -1 &&
             zoomOverrideValue != ANDROID_CONTROL_SETTINGS_OVERRIDE_OFF &&
             zoomOverrideValue != ANDROID_CONTROL_SETTINGS_OVERRIDE_ZOOM)) {
        return BAD_VALUE;
    }
    Mutex::Autolock lock(mServiceLock);
    mZoomOverrideValue = zoomOverrideValue;
    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                if (basicClient->supportsZoomOverride()) {
                    basicClient->setZoomOverride(mZoomOverrideValue);
                }
            }
        }
    }
    return OK;
}
status_t CameraService::handleWatchCommand(const Vector<String16>& args, int inFd, int outFd) {
    if (args.size() >= 3 && args[1] == toString16("start")) {
        return startWatchingTags(args, outFd);
    } else if (args.size() == 2 && args[1] == toString16("stop")) {
        return stopWatchingTags(outFd);
    } else if (args.size() == 2 && args[1] == toString16("dump")) {
        return printWatchedTags(outFd);
    } else if (args.size() >= 2 && args[1] == toString16("live")) {
        return printWatchedTagsUntilInterrupt(args, inFd, outFd);
    } else if (args.size() == 2 && args[1] == toString16("clear")) {
        return clearCachedMonitoredTagDumps(outFd);
    }
    dprintf(outFd, "Camera service watch commands:\n"
                 "  start -m <comma_separated_tag_list> [-c <comma_separated_client_list>]\n"
                 "        starts watching the provided tags for clients with provided package\n"
                 "        recognizes tag shorthands like '3a'\n"
                 "        watches all clients if no client is passed, or if 'all' is listed\n"
                 "  dump dumps the monitoring information and exits\n"
                 "  stop stops watching all tags\n"
                 "  live [-n <refresh_interval_ms>]\n"
                 "        prints the monitored information in real time\n"
                 "        Hit return to exit\n"
                 "  clear clears all buffers storing information for watch command");
  return BAD_VALUE;
}
status_t CameraService::startWatchingTags(const Vector<String16> &args, int outFd) {
    Mutex::Autolock lock(mLogLock);
    size_t tagsIdx;
    String16 tags("");
    for (tagsIdx = 2; tagsIdx < args.size() && args[tagsIdx] != toString16("-m"); tagsIdx++);
    if (tagsIdx < args.size() - 1) {
        tags = args[tagsIdx + 1];
    } else {
        dprintf(outFd, "No tags provided.\n");
        return BAD_VALUE;
    }
    size_t clientsIdx;
    String16 clients = toString16(kWatchAllClientsFlag);
    for (clientsIdx = 2; clientsIdx < args.size() && args[clientsIdx] != toString16("-c");
         clientsIdx++);
    if (clientsIdx < args.size() - 1) {
        clients = args[clientsIdx + 1];
    }
    parseClientsToWatchLocked(toStdString(clients));
    mMonitorTags = toStdString(tags);
    bool serviceLock = tryLock(mServiceLock);
    int numWatchedClients = 0;
    auto cameraClients = mActiveClientManager.getAll();
    for (const auto &clientDescriptor: cameraClients) {
        if (clientDescriptor == nullptr) { continue; }
        sp<BasicClient> client = clientDescriptor->getValue();
        if (client.get() == nullptr) { continue; }
        if (isClientWatchedLocked(client.get())) {
            client->startWatchingTags(mMonitorTags, outFd);
            numWatchedClients++;
        }
    }
    dprintf(outFd, "Started watching %d active clients\n", numWatchedClients);
    if (serviceLock) { mServiceLock.unlock(); }
    return OK;
}
status_t CameraService::stopWatchingTags(int outFd) {
    Mutex::Autolock lock(mLogLock);
    mMonitorTags = "";
    mWatchedClientPackages.clear();
    mWatchedClientsDumpCache.clear();
    bool serviceLock = tryLock(mServiceLock);
    auto cameraClients = mActiveClientManager.getAll();
    for (const auto &clientDescriptor : cameraClients) {
        if (clientDescriptor == nullptr) { continue; }
        sp<BasicClient> client = clientDescriptor->getValue();
        if (client.get() == nullptr) { continue; }
        client->stopWatchingTags(outFd);
    }
    dprintf(outFd, "Stopped watching all clients.\n");
    if (serviceLock) { mServiceLock.unlock(); }
    return OK;
}
status_t CameraService::clearCachedMonitoredTagDumps(int outFd) {
    Mutex::Autolock lock(mLogLock);
    size_t clearedSize = mWatchedClientsDumpCache.size();
    mWatchedClientsDumpCache.clear();
    dprintf(outFd, "Cleared tag information of %zu cached clients.\n", clearedSize);
    return OK;
}
status_t CameraService::printWatchedTags(int outFd) {
    Mutex::Autolock logLock(mLogLock);
    std::set<std::string> connectedMonitoredClients;
    bool printedSomething = false;
    bool serviceLock = tryLock(mServiceLock);
    for (const auto &clientDescriptor: mActiveClientManager.getAll()) {
        if (clientDescriptor == nullptr) { continue; }
        sp<BasicClient> client = clientDescriptor->getValue();
        if (client.get() == nullptr) { continue; }
        if (!isClientWatchedLocked(client.get())) { continue; }
        std::vector<std::string> dumpVector;
        client->dumpWatchedEventsToVector(dumpVector);
        size_t printIdx = dumpVector.size();
        if (printIdx == 0) {
            continue;
        }
        const std::string &cameraId = clientDescriptor->getKey();
        dprintf(outFd, "Client: %s (active)\n", client->getPackageName().c_str());
        while(printIdx > 0) {
            printIdx--;
            dprintf(outFd, "%s:%s  %s", cameraId.c_str(), client->getPackageName().c_str(),
                    dumpVector[printIdx].c_str());
        }
        dprintf(outFd, "\n");
        printedSomething = true;
        connectedMonitoredClients.emplace(client->getPackageName());
    }
    if (serviceLock) { mServiceLock.unlock(); }
    for (const auto &kv: mWatchedClientsDumpCache) {
        const std::string &package = kv.first;
        if (connectedMonitoredClients.find(package) != connectedMonitoredClients.end()) {
            continue;
        }
        dprintf(outFd, "Client: %s (cached)\n", package.c_str());
        dprintf(outFd, "%s\n", kv.second.c_str());
        printedSomething = true;
    }
    if (!printedSomething) {
        dprintf(outFd, "No monitoring information to print.\n");
    }
    return OK;
}
void printNewWatchedEvents(int outFd,
                           const std::string &cameraId,
                           const std::string &packageName,
                           const std::vector<std::string> &events,
                           const std::string &lastPrintedEvent) {
    if (events.empty()) { return; }
    size_t lastPrintedIdx;
    for (lastPrintedIdx = 0;
         lastPrintedIdx < events.size() && lastPrintedEvent != events[lastPrintedIdx];
         lastPrintedIdx++);
    if (lastPrintedIdx == 0) { return; }
    size_t idxToPrint = lastPrintedIdx;
    do {
        idxToPrint--;
        dprintf(outFd, "%s:%s  %s", cameraId.c_str(), packageName.c_str(),
                events[idxToPrint].c_str());
    } while (idxToPrint != 0);
}
bool shouldInterruptWatchCommand(int inFd, int outFd, long timeoutMs) {
    struct timeval startTime;
    int startTimeError = gettimeofday(&startTime, nullptr);
    if (startTimeError) {
        dprintf(outFd, "Failed waiting for interrupt, aborting.\n");
        return true;
    }
    const nfds_t numFds = 1;
    struct pollfd pollFd = { .fd = inFd, .events = POLLIN, .revents = 0 };
    struct timeval currTime;
    char buffer[2];
    while(true) {
        int currTimeError = gettimeofday(&currTime, nullptr);
        if (currTimeError) {
            dprintf(outFd, "Failed waiting for interrupt, aborting.\n");
            return true;
        }
        long elapsedTimeMs = ((currTime.tv_sec - startTime.tv_sec) * 1000L)
                + ((currTime.tv_usec - startTime.tv_usec) / 1000L);
        int remainingTimeMs = (int) (timeoutMs - elapsedTimeMs);
        if (remainingTimeMs <= 0) {
            return false;
        }
        int numFdsUpdated = poll(&pollFd, numFds, remainingTimeMs);
        if (numFdsUpdated < 0) {
            dprintf(outFd, "Failed while waiting for user input. Exiting.\n");
            return true;
        }
        if (numFdsUpdated == 0) {
            return false;
        }
        if (!(pollFd.revents & POLLIN)) {
            dprintf(outFd, "Failed while waiting for user input. Exiting.\n");
            return true;
        }
        ssize_t sizeRead = read(inFd, buffer, sizeof(buffer) - 1);
        if (sizeRead < 0) {
            dprintf(outFd, "Error reading user input. Exiting.\n");
            return true;
        }
        if (sizeRead == 0) {
            return true;
        }
        if (buffer[0] == '\n') {
            return true;
        }
    }
}
status_t CameraService::printWatchedTagsUntilInterrupt(const Vector<String16> &args,
                                                       int inFd, int outFd) {
    long refreshTimeoutMs = 1000L;
    if (args.size() > 2) {
        size_t intervalIdx;
        for (intervalIdx = 2; intervalIdx < args.size() && toString16("-n") != args[intervalIdx];
             intervalIdx++);
        size_t intervalValIdx = intervalIdx + 1;
        if (intervalValIdx < args.size()) {
            refreshTimeoutMs = strtol(toStdString(args[intervalValIdx]).c_str(), nullptr, 10);
            if (errno) { return BAD_VALUE; }
        }
    }
    refreshTimeoutMs = refreshTimeoutMs < 10 ? 10 : refreshTimeoutMs;
    dprintf(outFd, "Press return to exit...\n\n");
    std::map<std::string, std::string> packageNameToLastEvent;
    while (true) {
        bool serviceLock = tryLock(mServiceLock);
        auto cameraClients = mActiveClientManager.getAll();
        if (serviceLock) { mServiceLock.unlock(); }
        for (const auto& clientDescriptor : cameraClients) {
            Mutex::Autolock lock(mLogLock);
            if (clientDescriptor == nullptr) { continue; }
            sp<BasicClient> client = clientDescriptor->getValue();
            if (client.get() == nullptr) { continue; }
            if (!isClientWatchedLocked(client.get())) { continue; }
            const std::string &packageName = client->getPackageName();
            const std::string& lastPrintedEvent = packageNameToLastEvent[packageName];
            std::vector<std::string> latestEvents;
            client->dumpWatchedEventsToVector(latestEvents);
            if (!latestEvents.empty()) {
                printNewWatchedEvents(outFd,
                                      clientDescriptor->getKey(),
                                      packageName,
                                      latestEvents,
                                      lastPrintedEvent);
                packageNameToLastEvent[packageName] = latestEvents[0];
            }
        }
        if (shouldInterruptWatchCommand(inFd, outFd, refreshTimeoutMs)) {
            break;
        }
    }
    return OK;
}
void CameraService::parseClientsToWatchLocked(const std::string &clients) {
    mWatchedClientPackages.clear();
    std::istringstream iss(clients);
    std::string nextClient;
    while (std::getline(iss, nextClient, ',')) {
        if (nextClient == kWatchAllClientsFlag) {
            mWatchedClientPackages.clear();
            mWatchedClientPackages.emplace(kWatchAllClientsFlag);
            break;
        }
        mWatchedClientPackages.emplace(nextClient);
    }
}
status_t CameraService::printHelp(int out) {
    return dprintf(out, "Camera service commands:\n"
        "  get-uid-state <PACKAGE> [--user USER_ID] gets the uid state\n"
        "  set-uid-state <PACKAGE> <active|idle> [--user USER_ID] overrides the uid state\n"
        "  reset-uid-state <PACKAGE> [--user USER_ID] clears the uid state override\n"
        "  set-rotate-and-crop <ROTATION> overrides the rotate-and-crop value for AUTO backcompat\n"
        "      Valid values 0=0 deg, 1=90 deg, 2=180 deg, 3=270 deg, 4=No override\n"
        "  get-rotate-and-crop returns the current override rotate-and-crop value\n"
        "  set-autoframing <VALUE> overrides the autoframing value for AUTO\n"
        "      Valid values 0=false, 1=true, 2=auto\n"
        "  get-autoframing returns the current override autoframing value\n"
        "  set-image-dump-mask <MASK> specifies the formats to be saved to disk\n"
        "      Valid values 0=OFF, 1=ON for JPEG\n"
        "  get-image-dump-mask returns the current image-dump-mask value\n"
        "  set-camera-mute <0/1> enable or disable camera muting\n"
        "  set-stream-use-case-override <usecase1> <usecase2> ... override stream use cases\n"
        "      Use cases applied in descending resolutions. So usecase1 is assigned to the\n"
        "      largest resolution, usecase2 is assigned to the 2nd largest resolution, and so\n"
        "      on. In case the number of usecases is smaller than the number of streams, the\n"
        "      last use case is assigned to all the remaining streams. In case of multiple\n"
        "      streams with the same resolution, the tie-breaker is (JPEG, RAW, YUV, and PRIV)\n"
        "      Valid values are (case sensitive): DEFAULT, PREVIEW, STILL_CAPTURE, VIDEO_RECORD,\n"
        "      PREVIEW_VIDEO_STILL, VIDEO_CALL, CROPPED_RAW\n"
        "  clear-stream-use-case-override clear the stream use case override\n"
        "  set-zoom-override <-1/0/1> enable or disable zoom override\n"
        "      Valid values -1: do not override, 0: override to OFF, 1: override to ZOOM\n"
        "  set-watchdog <VALUE> enables or disables the camera service watchdog\n"
        "      Valid values 0=disable, 1=enable\n"
        "  watch <start|stop|dump|print|clear> manages tag monitoring in connected clients\n"
        "  remap-camera-id <PACKAGE> <Id0> <Id1> remaps camera ids. Must use adb root\n"
        "  help print this message\n");
}
bool CameraService::isClientWatched(const BasicClient *client) {
    Mutex::Autolock lock(mLogLock);
    return isClientWatchedLocked(client);
}
bool CameraService::isClientWatchedLocked(const BasicClient *client) {
    return mWatchedClientPackages.find(kWatchAllClientsFlag) != mWatchedClientPackages.end() ||
           mWatchedClientPackages.find(client->getPackageName()) != mWatchedClientPackages.end();
}
int32_t CameraService::updateAudioRestriction() {
    Mutex::Autolock lock(mServiceLock);
    return updateAudioRestrictionLocked();
}
int32_t CameraService::updateAudioRestrictionLocked() {
    int32_t mode = 0;
    for (const auto& i : mActiveClientManager.getAll()) {
        const auto clientSp = i->getValue();
        mode |= clientSp->getAudioRestriction();
    }
    bool modeChanged = (mAudioRestriction != mode);
    mAudioRestriction = mode;
    if (modeChanged) {
        mAppOps.setCameraAudioRestriction(mode);
    }
    return mode;
}
status_t CameraService::checkIfInjectionCameraIsPresent(const std::string& externalCamId,
        sp<BasicClient> clientSp) {
    std::unique_ptr<AutoConditionLock> lock =
            AutoConditionLock::waitAndAcquire(mServiceLockWrapper);
    status_t res = NO_ERROR;
    if ((res = checkIfDeviceIsUsable(externalCamId)) != NO_ERROR) {
        ALOGW("Device %s is not usable!", externalCamId.c_str());
        mInjectionStatusListener->notifyInjectionError(
                externalCamId, UNKNOWN_TRANSACTION);
        clientSp->notifyError(
                hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISCONNECTED,
                CaptureResultExtras());
        mServiceLock.unlock();
        int64_t token = CameraThreadState::clearCallingIdentity();
        clientSp->disconnect();
        CameraThreadState::restoreCallingIdentity(token);
        mServiceLock.lock();
    }
    return res;
}
void CameraService::clearInjectionParameters() {
    {
        Mutex::Autolock lock(mInjectionParametersLock);
        mInjectionInitPending = false;
        mInjectionInternalCamId = "";
    }
    mInjectionExternalCamId = "";
    mInjectionStatusListener->removeListener();
}
};
