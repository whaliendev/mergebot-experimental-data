#define LOG_TAG "TranscodingResourcePolicy"
#include <aidl/android/media/BnResourceObserver.h>
#include <aidl/android/media/IResourceObserverService.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <map>
#include <media/TranscodingResourcePolicy.h>
#include <utils/Log.h>
namespace android {
using Status = ::ndk::ScopedAStatus;
using ::aidl::android::media::BnResourceObserver;
using ::aidl::android::media::IResourceObserverService;
using ::aidl::android::media::MediaObservableEvent;
using ::aidl::android::media::MediaObservableFilter;
using ::aidl::android::media::MediaObservableParcel;
using ::aidl::android::media::MediaObservableType;
static std::string toString(const MediaObservableParcel& observable) {
    return "{" + ::aidl::android::media::toString(observable.type) + ", " +
           std::to_string(observable.value) + "}";
}
struct TranscodingResourcePolicy::ResourceObserver : public BnResourceObserver {
    explicit ResourceObserver(TranscodingResourcePolicy* owner) : mOwner(owner) {}
    ::ndk::ScopedAStatus onStatusChanged(
            MediaObservableEvent event, int32_t uid, int32_t pid,
            const std::vector<MediaObservableParcel>& observables) override {
        ALOGD("%s: %s, uid %d, pid %d, %s", __FUNCTION__,
              ::aidl::android::media::toString(event).c_str(), uid, pid,
              toString(observables[0]).c_str());
        if (((uint64_t)event & (uint64_t)MediaObservableEvent::kIdle) != 0) {
            for (auto& observable : observables) {
                if (observable.type == MediaObservableType::kVideoSecureCodec ||
                    observable.type == MediaObservableType::kVideoNonSecureCodec) {
                    mOwner->onResourceAvailable(pid);
                    break;
                }
            }
        }
        return ::ndk::ScopedAStatus::ok();
    }
    TranscodingResourcePolicy* mOwner;
};
class TranscodingResourcePolicyCookie {
 public:
    TranscodingResourcePolicyCookie(TranscodingResourcePolicy* policy) : mPolicy(policy) {}
    TranscodingResourcePolicyCookie() = delete;
    TranscodingResourcePolicy* mPolicy;
};
static std::map<uintptr_t, std::unique_ptr<TranscodingResourcePolicyCookie>> sCookies;
static uintptr_t sCookieKeyCounter;
static std::mutex sCookiesMutex;
void TranscodingResourcePolicy::BinderDiedCallback(void* cookie) {
    std::lock_guard<std::mutex> guard(sCookiesMutex);
    if (auto it = sCookies.find(reinterpret_cast<uintptr_t>(cookie)); it != sCookies.end()) {
        ALOGI("BinderDiedCallback unregistering TranscodingResourcePolicy");
        auto policy = reinterpret_cast<TranscodingResourcePolicy*>(it->second->mPolicy);
        if (policy) {
            policy->unregisterSelf();
        }
        sCookies.erase(it);
    }
}
TranscodingResourcePolicy::TranscodingResourcePolicy()
      : mRegistered(false),
        mResourceLostPid(-1),
        mDeathRecipient(AIBinder_DeathRecipient_new(BinderDiedCallback)) {
    registerSelf();
}
TranscodingResourcePolicy::~TranscodingResourcePolicy() {
<<<<<<< HEAD
    {
        std::lock_guard<std::mutex> guard(sCookiesMutex);
        std::erase_if(sCookies, [this](const auto& cookieEntry) {
            auto const& [key, cookie] = cookieEntry;
            std::lock_guard guard(mCookieKeysLock);
            if (const auto& it = mCookieKeys.find(key); it != mCookieKeys.end()) {
                mCookieKeys.erase(key);
                return true;
            }
            return false;
        });
    }
||||||| b6e500602a
=======
    {
        std::lock_guard<std::mutex> guard(sCookiesMutex);
        for (auto it = sCookies.begin(); it != sCookies.end();) {
            const uintptr_t key = it->first;
            std::lock_guard guard(mCookieKeysLock);
            if (mCookieKeys.find(key) != mCookieKeys.end()) {
                mCookieKeys.erase(key);
                it = sCookies.erase(it);
            } else {
                it++;
            }
        }
    }
>>>>>>> 083f262d
    unregisterSelf();
}
void TranscodingResourcePolicy::registerSelf() {
    ALOGI("TranscodingResourcePolicy: registerSelf");
    ::ndk::SpAIBinder binder(AServiceManager_getService("media.resource_observer"));
    std::scoped_lock lock{mRegisteredLock};
    if (mRegistered) {
        return;
    }
    mService = IResourceObserverService::fromBinder(binder);
    if (mService == nullptr) {
        ALOGE("Failed to get IResourceObserverService");
        return;
    }
    mObserver = ::ndk::SharedRefBase::make<ResourceObserver>(this);
    std::vector<MediaObservableFilter> filters = {
            {MediaObservableType::kVideoSecureCodec, MediaObservableEvent::kIdle},
            {MediaObservableType::kVideoNonSecureCodec, MediaObservableEvent::kIdle}};
    Status status = mService->registerObserver(mObserver, filters);
    if (!status.isOk()) {
        ALOGE("failed to register: error %d", status.getServiceSpecificError());
        mService = nullptr;
        mObserver = nullptr;
        return;
    }
<<<<<<< HEAD
    std::unique_ptr<TranscodingResourcePolicyCookie> cookie =
            std::make_unique<TranscodingResourcePolicyCookie>(this);
    void* cookiePtr = static_cast<void*>(cookie.get());
    uintptr_t cookieKey = sCookieKeyCounter++;
    sCookies.emplace(cookieKey, std::move(cookie));
    {
        std::lock_guard guard(mCookieKeysLock);
        mCookieKeys.insert(cookieKey);
    }
    AIBinder_linkToDeath(binder.get(), mDeathRecipient.get(), reinterpret_cast<void*>(cookieKey));
||||||| b6e500602a
    AIBinder_linkToDeath(binder.get(), mDeathRecipient.get(), reinterpret_cast<void*>(this));
=======
    std::unique_ptr<TranscodingResourcePolicyCookie> cookie =
            std::make_unique<TranscodingResourcePolicyCookie>(this);
    uintptr_t cookieKey = sCookieKeyCounter++;
    sCookies.emplace(cookieKey, std::move(cookie));
    {
        std::lock_guard guard(mCookieKeysLock);
        mCookieKeys.insert(cookieKey);
    }
    AIBinder_linkToDeath(binder.get(), mDeathRecipient.get(), reinterpret_cast<void*>(cookieKey));
>>>>>>> 083f262d
    ALOGD("@@@ registered observer");
    mRegistered = true;
}
void TranscodingResourcePolicy::unregisterSelf() {
    ALOGI("TranscodingResourcePolicy: unregisterSelf");
    std::scoped_lock lock{mRegisteredLock};
    if (!mRegistered) {
        return;
    }
    ::ndk::SpAIBinder binder = mService->asBinder();
    if (binder.get() != nullptr) {
        Status status = mService->unregisterObserver(mObserver);
    }
    mService = nullptr;
    mObserver = nullptr;
    mRegistered = false;
}
void TranscodingResourcePolicy::setCallback(
        const std::shared_ptr<ResourcePolicyCallbackInterface>& cb) {
    std::scoped_lock lock{mCallbackLock};
    mResourcePolicyCallback = cb;
}
void TranscodingResourcePolicy::setPidResourceLost(pid_t pid) {
    std::scoped_lock lock{mCallbackLock};
    mResourceLostPid = pid;
}
void TranscodingResourcePolicy::onResourceAvailable(pid_t pid) {
    std::shared_ptr<ResourcePolicyCallbackInterface> cb;
    {
        std::scoped_lock lock{mCallbackLock};
        if (mResourceLostPid != -1 && mResourceLostPid != pid) {
            cb = mResourcePolicyCallback.lock();
            mResourceLostPid = -1;
        }
    }
    if (cb != nullptr) {
        cb->onResourceAvailable();
    }
}
}
