#define LOG_TAG "ServiceUtilities"
#include <audio_utils/clock.h>
#include <binder/AppOpsManager.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/PermissionCache.h>
#include "mediautils/ServiceUtilities.h"
#include <system/audio-hal-enums.h>
#include <media/AidlConversion.h>
#include <media/AidlConversionUtil.h>
#include <android/content/AttributionSourceState.h>
#include <iterator>
#include <algorithm>
#include <pwd.h>
namespace android {
using content::AttributionSourceState;
static const String16 sAndroidPermissionRecordAudio("android.permission.RECORD_AUDIO");
static const String16 sModifyPhoneState("android.permission.MODIFY_PHONE_STATE");
static const String16 sModifyAudioRouting("android.permission.MODIFY_AUDIO_ROUTING");
static const String16 sCallAudioInterception("android.permission.CALL_AUDIO_INTERCEPTION");
static String16 resolveCallingPackage(PermissionController& permissionController,
        const std::optional<String16> opPackageName, uid_t uid) {
    if (opPackageName.has_value() && opPackageName.value().size() > 0) {
        return opPackageName.value();
    }
    Vector<String16> packages;
    permissionController.getPackagesForUid(uid, packages);
    if (packages.isEmpty()) {
        ALOGE("No packages for uid %d", uid);
        return String16();
    }
    return packages[0];
}
int32_t getOpForSource(audio_source_t source) {
  switch (source) {
    case AUDIO_SOURCE_HOTWORD:
      return AppOpsManager::OP_RECORD_AUDIO_HOTWORD;
    case AUDIO_SOURCE_ECHO_REFERENCE:
    case AUDIO_SOURCE_REMOTE_SUBMIX:
      return AppOpsManager::OP_RECORD_AUDIO_OUTPUT;
    case AUDIO_SOURCE_VOICE_DOWNLINK:
      return AppOpsManager::OP_RECORD_INCOMING_PHONE_AUDIO;
    case AUDIO_SOURCE_DEFAULT:
    default:
      return AppOpsManager::OP_RECORD_AUDIO;
  }
}
std::optional<AttributionSourceState> resolveAttributionSource(
        const AttributionSourceState& callerAttributionSource, const uint32_t virtualDeviceId) {
    AttributionSourceState nextAttributionSource = callerAttributionSource;
    if (!nextAttributionSource.packageName.has_value()) {
        nextAttributionSource = AttributionSourceState(nextAttributionSource);
        PermissionController permissionController;
        const uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(nextAttributionSource.uid));
        nextAttributionSource.packageName = VALUE_OR_FATAL(legacy2aidl_String16_string(
                resolveCallingPackage(permissionController, VALUE_OR_FATAL(
                aidl2legacy_string_view_String16(nextAttributionSource.packageName.value_or(""))),
                uid)));
        if (!nextAttributionSource.packageName.has_value()) {
            return std::nullopt;
        }
    }
    nextAttributionSource.deviceId = virtualDeviceId;
    AttributionSourceState myAttributionSource;
    myAttributionSource.uid = VALUE_OR_FATAL(android::legacy2aidl_uid_t_int32_t(getuid()));
    myAttributionSource.pid = VALUE_OR_FATAL(android::legacy2aidl_pid_t_int32_t(getpid()));
    static sp<BBinder> appOpsToken = sp<BBinder>::make();
    myAttributionSource.token = appOpsToken;
    myAttributionSource.deviceId = virtualDeviceId;
    myAttributionSource.next.push_back(nextAttributionSource);
    return std::optional<AttributionSourceState>{myAttributionSource};
}
    static bool checkRecordingInternal(const AttributionSourceState &attributionSource,
                                       const uint32_t virtualDeviceId,
                                       const String16 &msg, bool start, audio_source_t source) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    if (isAudioServerOrMediaServerOrSystemServerOrRootUid(uid)) return true;
    std::optional<AttributionSourceState> resolvedAttributionSource =
            resolveAttributionSource(attributionSource, virtualDeviceId);
    if (!resolvedAttributionSource.has_value()) {
        return false;
    }
    const int32_t attributedOpCode = getOpForSource(source);
    permission::PermissionChecker permissionChecker;
    bool permitted = false;
    if (start) {
        permitted = (permissionChecker.checkPermissionForStartDataDeliveryFromDatasource(
                sAndroidPermissionRecordAudio, resolvedAttributionSource.value(), msg,
                attributedOpCode) != permission::PermissionChecker::PERMISSION_HARD_DENIED);
    } else {
        permitted = (permissionChecker.checkPermissionForPreflightFromDatasource(
                sAndroidPermissionRecordAudio, resolvedAttributionSource.value(), msg,
                attributedOpCode) != permission::PermissionChecker::PERMISSION_HARD_DENIED);
    }
    return permitted;
}
static constexpr int DEVICE_ID_DEFAULT = 0;
bool recordingAllowed(const AttributionSourceState &attributionSource, audio_source_t source) {
    return checkRecordingInternal(attributionSource, DEVICE_ID_DEFAULT, String16(), false,
                                  source);
}
bool recordingAllowed(const AttributionSourceState &attributionSource,
                      const uint32_t virtualDeviceId,
                      audio_source_t source) {
    return checkRecordingInternal(attributionSource, virtualDeviceId,
                                  String16(), false, source);
}
bool startRecording(const AttributionSourceState& attributionSource,
                    const uint32_t virtualDeviceId,
                    const String16& msg,
                    audio_source_t source) {
    return checkRecordingInternal(attributionSource, virtualDeviceId, msg, true,
                                  source);
}
void finishRecording(const AttributionSourceState &attributionSource, uint32_t virtualDeviceId,
                     audio_source_t source) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    if (isAudioServerOrMediaServerOrSystemServerOrRootUid(uid)) return;
    const std::optional<AttributionSourceState> resolvedAttributionSource =
            resolveAttributionSource(attributionSource, virtualDeviceId);
    if (!resolvedAttributionSource.has_value()) {
        return;
    }
    const int32_t attributedOpCode = getOpForSource(source);
    permission::PermissionChecker permissionChecker;
    permissionChecker.finishDataDeliveryFromDatasource(attributedOpCode,
            resolvedAttributionSource.value());
}
bool captureAudioOutputAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    if (isAudioServerOrRootUid(uid)) return true;
    static const String16 sCaptureAudioOutput("android.permission.CAPTURE_AUDIO_OUTPUT");
    permission::PermissionChecker permissionChecker;
    bool ok = (permissionChecker.checkPermissionForPreflight(
            sCaptureAudioOutput, attributionSource, String16(),
            AppOpsManager::OP_NONE) != permission::PermissionChecker::PERMISSION_HARD_DENIED);
    if (!ok) ALOGV("Request requires android.permission.CAPTURE_AUDIO_OUTPUT");
    return ok;
}
bool captureMediaOutputAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerOrRootUid(uid)) return true;
    static const String16 sCaptureMediaOutput("android.permission.CAPTURE_MEDIA_OUTPUT");
    bool ok = PermissionCache::checkPermission(sCaptureMediaOutput, pid, uid);
    if (!ok) ALOGE("Request requires android.permission.CAPTURE_MEDIA_OUTPUT");
    return ok;
}
bool captureTunerAudioInputAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerOrRootUid(uid)) return true;
    static const String16 sCaptureTunerAudioInput("android.permission.CAPTURE_TUNER_AUDIO_INPUT");
    bool ok = PermissionCache::checkPermission(sCaptureTunerAudioInput, pid, uid);
    if (!ok) ALOGV("Request requires android.permission.CAPTURE_TUNER_AUDIO_INPUT");
    return ok;
}
bool captureVoiceCommunicationOutputAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    uid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerOrRootUid(uid)) return true;
    static const String16 sCaptureVoiceCommOutput(
        "android.permission.CAPTURE_VOICE_COMMUNICATION_OUTPUT");
    bool ok = PermissionCache::checkPermission(sCaptureVoiceCommOutput, pid, uid);
    if (!ok) ALOGE("Request requires android.permission.CAPTURE_VOICE_COMMUNICATION_OUTPUT");
    return ok;
}
bool accessUltrasoundAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    uid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerOrRootUid(uid)) return true;
    static const String16 sAccessUltrasound(
        "android.permission.ACCESS_ULTRASOUND");
    bool ok = PermissionCache::checkPermission(sAccessUltrasound, pid, uid);
    if (!ok) ALOGE("Request requires android.permission.ACCESS_ULTRASOUND");
    return ok;
}
bool captureHotwordAllowed(const AttributionSourceState& attributionSource) {
    bool ok = recordingAllowed(attributionSource);
    if (ok) {
        static const String16 sCaptureHotwordAllowed("android.permission.CAPTURE_AUDIO_HOTWORD");
        permission::PermissionChecker permissionChecker;
        ok = (permissionChecker.checkPermissionForPreflight(
                sCaptureHotwordAllowed, attributionSource, String16(),
                AppOpsManager::OP_NONE) != permission::PermissionChecker::PERMISSION_HARD_DENIED);
    }
    if (!ok) ALOGV("android.permission.CAPTURE_AUDIO_HOTWORD");
    return ok;
}
bool settingsAllowed() {
    if (isAudioServerUid(IPCThreadState::self()->getCallingUid())) return true;
    static const String16 sAudioSettings("android.permission.MODIFY_AUDIO_SETTINGS");
    bool ok = PermissionCache::checkCallingPermission(sAudioSettings);
    if (!ok) ALOGE("Request requires android.permission.MODIFY_AUDIO_SETTINGS");
    return ok;
}
bool modifyAudioRoutingAllowed() {
    return modifyAudioRoutingAllowed(getCallingAttributionSource());
}
bool modifyAudioRoutingAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerUid(IPCThreadState::self()->getCallingUid())) return true;
    bool ok = PermissionCache::checkPermission(sModifyAudioRouting, pid, uid);
    if (!ok) ALOGE("%s(): android.permission.MODIFY_AUDIO_ROUTING denied for uid %d",
        __func__, uid);
    return ok;
}
bool modifyDefaultAudioEffectsAllowed() {
    return modifyDefaultAudioEffectsAllowed(getCallingAttributionSource());
}
bool modifyDefaultAudioEffectsAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerUid(IPCThreadState::self()->getCallingUid())) return true;
    static const String16 sModifyDefaultAudioEffectsAllowed(
            "android.permission.MODIFY_DEFAULT_AUDIO_EFFECTS");
    bool ok = PermissionCache::checkPermission(sModifyDefaultAudioEffectsAllowed, pid, uid);
    ALOGE_IF(!ok, "%s(): android.permission.MODIFY_DEFAULT_AUDIO_EFFECTS denied for uid %d",
            __func__, uid);
    return ok;
}
bool dumpAllowed() {
    static const String16 sDump("android.permission.DUMP");
    bool ok = PermissionCache::checkCallingPermission(sDump);
    return ok;
}
bool modifyPhoneStateAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    bool ok = PermissionCache::checkPermission(sModifyPhoneState, pid, uid);
    ALOGE_IF(!ok, "Request requires %s", String8(sModifyPhoneState).c_str());
    return ok;
}
bool bypassInterruptionPolicyAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    static const String16 sWriteSecureSettings("android.permission.WRITE_SECURE_SETTINGS");
    bool ok = PermissionCache::checkPermission(sModifyPhoneState, pid, uid)
        || PermissionCache::checkPermission(sWriteSecureSettings, pid, uid)
        || PermissionCache::checkPermission(sModifyAudioRouting, pid, uid);
    ALOGE_IF(!ok, "Request requires %s or %s",
             String8(sModifyPhoneState).c_str(), String8(sWriteSecureSettings).c_str());
    return ok;
}
bool callAudioInterceptionAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    bool ok = PermissionCache::checkPermission(sCallAudioInterception, pid, uid);
    if (!ok) ALOGV("%s(): android.permission.CALL_AUDIO_INTERCEPTION denied for uid %d",
        __func__, uid);
    return ok;
}
AttributionSourceState getCallingAttributionSource() {
    AttributionSourceState attributionSource = AttributionSourceState();
    attributionSource.pid = VALUE_OR_FATAL(legacy2aidl_pid_t_int32_t(
            IPCThreadState::self()->getCallingPid()));
    attributionSource.uid = VALUE_OR_FATAL(legacy2aidl_uid_t_int32_t(
            IPCThreadState::self()->getCallingUid()));
    attributionSource.token = sp<BBinder>::make();
  return attributionSource;
}
void purgePermissionCache() {
    PermissionCache::purgeCache();
}
status_t checkIMemory(const sp<IMemory>& iMemory)
{
    if (iMemory == 0) {
        ALOGE("%s check failed: NULL IMemory pointer", __FUNCTION__);
        return BAD_VALUE;
    }
    sp<IMemoryHeap> heap = iMemory->getMemory();
    if (heap == 0) {
        ALOGE("%s check failed: NULL heap pointer", __FUNCTION__);
        return BAD_VALUE;
    }
    off_t size = lseek(heap->getHeapID(), 0, SEEK_END);
    lseek(heap->getHeapID(), 0, SEEK_SET);
    if (iMemory->unsecurePointer() == NULL || size < (off_t)iMemory->size()) {
        ALOGE("%s check failed: pointer %p size %zu fd size %u",
              __FUNCTION__, iMemory->unsecurePointer(), iMemory->size(), (uint32_t)size);
        return BAD_VALUE;
    }
    return NO_ERROR;
}
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            bool mustAnonymizeBluetoothAddress( const AttributionSourceState& attributionSource, const String16& caller) { uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid)); if (isAudioServerOrSystemServerUid(uid)) { return false; } const std::optional<AttributionSourceState> resolvedAttributionSource = resolveAttributionSource(attributionSource, DEVICE_ID_DEFAULT); if (!resolvedAttributionSource.has_value()) { return true; } permission::PermissionChecker permissionChecker; return permissionChecker.checkPermissionForPreflightFromDatasource( sAndroidPermissionBluetoothConnect, resolvedAttributionSource.value(), caller, AppOpsManager::OP_BLUETOOTH_CONNECT) != permission::PermissionChecker::PERMISSION_GRANTED; } void anonymizeBluetoothAddress(char *address) { if (address == nullptr || strlen(address) != strlen("AA:BB:CC:
sp<content::pm::IPackageManagerNative> MediaPackageManager::retrievePackageManager() {
    const sp<IServiceManager> sm = defaultServiceManager();
    if (sm == nullptr) {
        ALOGW("%s: failed to retrieve defaultServiceManager", __func__);
        return nullptr;
    }
    sp<IBinder> packageManager = sm->checkService(String16(nativePackageManagerName));
    if (packageManager == nullptr) {
        ALOGW("%s: failed to retrieve native package manager", __func__);
        return nullptr;
    }
    return interface_cast<content::pm::IPackageManagerNative>(packageManager);
}
std::optional<bool> MediaPackageManager::doIsAllowed(uid_t uid) {
    if (mPackageManager == nullptr) {
        mPackageManager = retrievePackageManager();
        if (mPackageManager == nullptr) {
            ALOGW("%s: Playback capture is denied as package manager is not reachable", __func__);
            return std::nullopt;
        }
    }
    Vector<String16> str16PackageNames;
    PermissionController{}.getPackagesForUid(uid, str16PackageNames);
    std::vector<std::string> packageNames;
    for (const auto& str16PackageName : str16PackageNames) {
        packageNames.emplace_back(String8(str16PackageName).c_str());
    }
    if (packageNames.empty()) {
        ALOGW("%s: Playback capture for uid %u is denied as no package name could be retrieved "
              "from the package manager.", __func__, uid);
        return std::nullopt;
    }
    std::vector<bool> isAllowed;
    auto status = mPackageManager->isAudioPlaybackCaptureAllowed(packageNames, &isAllowed);
    if (!status.isOk()) {
        ALOGW("%s: Playback capture is denied for uid %u as the manifest property could not be "
              "retrieved from the package manager: %s", __func__, uid, status.toString8().c_str());
        return std::nullopt;
    }
    if (packageNames.size() != isAllowed.size()) {
        ALOGW("%s: Playback capture is denied for uid %u as the package manager returned incoherent"
              " response size: %zu != %zu", __func__, uid, packageNames.size(), isAllowed.size());
        return std::nullopt;
    }
    Packages& packages = mDebugLog[uid];
    packages.resize(packageNames.size());
    std::transform(begin(packageNames), end(packageNames), begin(isAllowed),
                   begin(packages), [] (auto& name, bool isAllowed) -> Package {
                       return {std::move(name), isAllowed};
                   });
    bool playbackCaptureAllowed = std::all_of(begin(isAllowed), end(isAllowed),
                                                  [](bool b) { return b; });
    return playbackCaptureAllowed;
}
void MediaPackageManager::dump(int fd, int spaces) const {
    dprintf(fd, "%*sAllow playback capture log:\n", spaces, "");
    if (mPackageManager == nullptr) {
        dprintf(fd, "%*sNo package manager\n", spaces + 2, "");
    }
    dprintf(fd, "%*sPackage manager errors: %u\n", spaces + 2, "", mPackageManagerErrors);
    for (const auto& uidCache : mDebugLog) {
        for (const auto& package : std::get<Packages>(uidCache)) {
            dprintf(fd, "%*s- uid=%5u, allowPlaybackCapture=%s, packageName=%s\n", spaces + 2, "",
                    std::get<const uid_t>(uidCache),
                    package.playbackCaptureAllowed ? "true " : "false",
                    package.name.c_str());
        }
    }
}
static constexpr nsecs_t INFO_EXPIRATION_NS = 24 * 60 * 60 * NANOS_PER_SECOND;
static constexpr size_t INFO_CACHE_MAX = 1000;
mediautils::UidInfo::Info mediautils::UidInfo::getInfo(uid_t uid)
{
    const nsecs_t now = systemTime(SYSTEM_TIME_REALTIME);
    struct mediautils::UidInfo::Info info;
    {
        std::lock_guard _l(mLock);
        auto it = mInfoMap.find(uid);
        if (it != mInfoMap.end()) {
            info = it->second;
            ALOGV("%s: uid %d expiration %lld now %lld",
                    __func__, uid, (long long)info.expirationNs, (long long)now);
            if (info.expirationNs <= now) {
                ALOGV("%s: entry for uid %d expired, now %lld",
                        __func__, uid, (long long)now);
                mInfoMap.erase(it);
                info.uid = (uid_t)-1;
            }
        }
    }
    if (info.uid == (uid_t)(-1)) {
        sp<IServiceManager> sm = defaultServiceManager();
        sp<content::pm::IPackageManagerNative> package_mgr;
        if (sm.get() == nullptr) {
            ALOGE("%s: Cannot find service manager", __func__);
        } else {
            sp<IBinder> binder = sm->getService(String16("package_native"));
            if (binder.get() == nullptr) {
                ALOGE("%s: Cannot find package_native", __func__);
            } else {
                package_mgr = interface_cast<content::pm::IPackageManagerNative>(binder);
            }
        }
        std::string pkg;
        if (package_mgr != nullptr) {
            std::vector<std::string> names;
            binder::Status status = package_mgr->getNamesForUids({(int)uid}, &names);
            if (!status.isOk()) {
                ALOGE("%s: getNamesForUids failed: %s",
                        __func__, status.exceptionMessage().c_str());
            } else {
                if (!names[0].empty()) {
                    pkg = names[0].c_str();
                }
            }
        }
        if (pkg.empty()) {
            struct passwd pw{}, *result;
            char buf[8192];
            if (getpwuid_r(uid, &pw, buf, sizeof(buf), &result) == 0
                    && result != nullptr
                    && result->pw_name != nullptr) {
                pkg = result->pw_name;
            }
        }
        if (pkg.compare(0, 7, "shared:") == 0) {
            pkg.erase(0, 7);
        }
        std::string installer;
        int64_t versionCode = 0;
        bool notFound = false;
        if (pkg.empty()) {
            pkg = std::to_string(uid);
            notFound = true;
        } else if (strchr(pkg.c_str(), '.') == nullptr) {
        } else if (strncmp(pkg.c_str(), "android.", 8) == 0) {
        } else if (package_mgr.get() != nullptr) {
            String16 pkgName16(pkg.c_str());
            binder::Status status = package_mgr->getInstallerForPackage(pkgName16, &installer);
            if (!status.isOk()) {
                ALOGE("%s: getInstallerForPackage failed: %s",
                        __func__, status.exceptionMessage().c_str());
            }
            if (status.isOk()) {
                status = package_mgr->getVersionCodeForPackage(pkgName16, &versionCode);
                if (!status.isOk()) {
                    ALOGE("%s: getVersionCodeForPackage failed: %s",
                            __func__, status.exceptionMessage().c_str());
                }
            }
            ALOGV("%s: package '%s' installed by '%s' versioncode %lld",
                    __func__, pkg.c_str(), installer.c_str(), (long long)versionCode);
        }
        std::lock_guard _l(mLock);
        if (mInfoMap.size() >= INFO_CACHE_MAX) mInfoMap.clear();
        info.uid = uid;
        info.package = std::move(pkg);
        info.installer = std::move(installer);
        info.versionCode = versionCode;
        info.expirationNs = now + (notFound ? 0 : INFO_EXPIRATION_NS);
        ALOGV("%s: adding uid %d package '%s' expirationNs: %lld",
                __func__, uid, info.package.c_str(), (long long)info.expirationNs);
        mInfoMap[uid] = info;
    }
    return info;
}
}
