#define LOG_TAG "AidlLazyServiceRegistrar"
#include <binder/LazyServiceRegistrar.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <android/os/BnClientCallback.h>
#include <android/os/IServiceManager.h>
#include <utils/Log.h>
namespace android {
namespace binder {
namespace internal {
using AidlServiceManager = android::os::IServiceManager;
class ClientCounterCallbackImpl : public ::android::os::BnClientCallback {
public:
    ClientCounterCallbackImpl() : mNumConnectedServices(0), mForcePersist(false) {}
    bool registerService(const sp<IBinder>& service, const std::string& name,
                         bool allowIsolated, int dumpFlags);
    void forcePersist(bool persist);
protected:
    Status onClients(const sp<IBinder>& service, bool clients) override;
private:
    struct Service {
        sp<IBinder> service;
        bool allowIsolated;
        int dumpFlags;
        bool clients = false;
    };
    std::map<std::string, Service>::iterator assertRegisteredService(const sp<IBinder>& service);
    void tryShutdown();
    size_t mNumConnectedServices;
    std::map<std::string, Service> mRegisteredServices;
    bool mForcePersist;
};
class ClientCounterCallback {
public:
    ClientCounterCallback();
    bool registerService(const sp<IBinder>& service, const std::string& name,
                                            bool allowIsolated, int dumpFlags);
    void forcePersist(bool persist);
private:
    sp<ClientCounterCallbackImpl> mImpl;
};
bool ClientCounterCallbackImpl::registerService(const sp<IBinder>& service, const std::string& name,
                                            bool allowIsolated, int dumpFlags) {
    auto manager = interface_cast<AidlServiceManager>(asBinder(defaultServiceManager()));
    bool reRegister = mRegisteredServices.count(name) > 0;
    std::string regStr = (reRegister) ? "Re-registering" : "Registering";
    ALOGI("%s service %s", regStr.c_str(), name.c_str());
    if (!manager->addService(name.c_str(), service, allowIsolated, dumpFlags).isOk()) {
        ALOGE("Failed to register service %s", name.c_str());
        return false;
    }
    if (!reRegister) {
        if (!manager->registerClientCallback(name, service, this).isOk()) {
            ALOGE("Failed to add client callback for service %s", name.c_str());
            return false;
        }
        mRegisteredServices[name] = {
              .service = service,
              .allowIsolated = allowIsolated,
              .dumpFlags = dumpFlags
        };
    }
    return true;
}
<<<<<<< HEAD
std::map<std::string, ClientCounterCallback::Service>::iterator ClientCounterCallback::assertRegisteredService(const sp<IBinder>& service) {
    LOG_ALWAYS_FATAL_IF(service == nullptr, "Got onClients callback for null service");
    for (auto it = mRegisteredServices.begin(); it != mRegisteredServices.end(); ++it) {
        auto const& [name, registered] = *it;
        (void) name;
        if (registered.service != service) continue;
        return it;
    }
    LOG_ALWAYS_FATAL("Got callback on service which we did not register: %s", String8(service->getInterfaceDescriptor()).c_str());
    __builtin_unreachable();
}
void ClientCounterCallback::forcePersist(bool persist) {
||||||| 9243c1057f
void ClientCounterCallback::forcePersist(bool persist) {
=======
void ClientCounterCallbackImpl::forcePersist(bool persist) {
>>>>>>> f76b5eb5
    mForcePersist = persist;
    if(!mForcePersist) {
        tryShutdown();
    }
}
<<<<<<< HEAD
Status ClientCounterCallback::onClients(const sp<IBinder>& service, bool clients) {
    auto & [name, registered] = *assertRegisteredService(service);
    if (registered.clients == clients) {
        LOG_ALWAYS_FATAL("Process already thought %s had clients: %d but servicemanager has "
                         "notified has clients: %d", name.c_str(), registered.clients, clients);
    }
    registered.clients = clients;
    {
         size_t numWithClients = 0;
         for (const auto& [name, registered] : mRegisteredServices) {
             (void) name;
             if (registered.clients) numWithClients++;
         }
         mNumConnectedServices = numWithClients;
||||||| 9243c1057f
Status ClientCounterCallback::onClients(const sp<IBinder>& service, bool clients) {
    if (clients) {
        mNumConnectedServices++;
    } else {
        mNumConnectedServices--;
=======
Status ClientCounterCallbackImpl::onClients(const sp<IBinder>& service, bool clients) {
    if (clients) {
        mNumConnectedServices++;
    } else {
        mNumConnectedServices--;
>>>>>>> f76b5eb5
    }
    ALOGI("Process has %zu (of %zu available) client(s) in use after notification %s has clients: %d",
          mNumConnectedServices, mRegisteredServices.size(), name.c_str(), clients);
    tryShutdown();
    return Status::ok();
}
void ClientCounterCallbackImpl::tryShutdown() {
    if(mNumConnectedServices > 0) {
        return;
    }
    if(mForcePersist) {
        ALOGI("Shutdown prevented by forcePersist override flag.");
        return;
    }
    ALOGI("Trying to shut down the service. No clients in use for any service in process.");
    auto manager = interface_cast<AidlServiceManager>(asBinder(defaultServiceManager()));
    auto unRegisterIt = mRegisteredServices.begin();
    for (; unRegisterIt != mRegisteredServices.end(); ++unRegisterIt) {
        auto& entry = (*unRegisterIt);
        bool success = manager->tryUnregisterService(entry.first, entry.second.service).isOk();
        if (!success) {
            ALOGI("Failed to unregister service %s", entry.first.c_str());
            break;
        }
    }
    if (unRegisterIt == mRegisteredServices.end()) {
        ALOGI("Unregistered all clients and exiting");
        exit(EXIT_SUCCESS);
    }
    for (auto reRegisterIt = mRegisteredServices.begin(); reRegisterIt != unRegisterIt;
         reRegisterIt++) {
        auto& entry = (*reRegisterIt);
        if (!registerService(entry.second.service, entry.first, entry.second.allowIsolated,
                             entry.second.dumpFlags)) {
            ALOGE("Bad state: could not re-register services");
        }
    }
}
ClientCounterCallback::ClientCounterCallback() {
      mImpl = new ClientCounterCallbackImpl();
}
bool ClientCounterCallback::registerService(const sp<IBinder>& service, const std::string& name,
                                            bool allowIsolated, int dumpFlags) {
    return mImpl->registerService(service, name, allowIsolated, dumpFlags);
}
void ClientCounterCallback::forcePersist(bool persist) {
    mImpl->forcePersist(persist);
}
}
LazyServiceRegistrar::LazyServiceRegistrar() {
    mClientCC = std::make_shared<internal::ClientCounterCallback>();
}
LazyServiceRegistrar& LazyServiceRegistrar::getInstance() {
    static auto registrarInstance = new LazyServiceRegistrar();
    return *registrarInstance;
}
status_t LazyServiceRegistrar::registerService(const sp<IBinder>& service, const std::string& name,
                                               bool allowIsolated, int dumpFlags) {
    if (!mClientCC->registerService(service, name, allowIsolated, dumpFlags)) {
        return UNKNOWN_ERROR;
    }
    return OK;
}
void LazyServiceRegistrar::forcePersist(bool persist) {
    mClientCC->forcePersist(persist);
}
}
}
