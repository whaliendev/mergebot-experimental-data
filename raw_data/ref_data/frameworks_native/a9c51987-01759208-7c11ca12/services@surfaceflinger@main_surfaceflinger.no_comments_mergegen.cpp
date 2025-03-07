#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#include <sys/resource.h>
#include <sched.h>
#include <android/frameworks/displayservice/1.0/IDisplayService.h>
#include <android/hardware/configstore/1.0/ISurfaceFlingerConfigs.h>
#include <android/hardware/graphics/allocator/2.0/IAllocator.h>
#include <android/hardware/graphics/allocator/3.0/IAllocator.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <common/FlagManager.h>
#include <configstore/Utils.h>
#include <displayservice/DisplayService.h>
#include <errno.h>
#include <hidl/LegacySupport.h>
#include <processgroup/sched_policy.h>
#include "SurfaceFlinger.h"
#include "SurfaceFlingerFactory.h"
#include "SurfaceFlingerProperties.h"
using namespace android;
static status_t startGraphicsAllocatorService() {
    using android::hardware::configstore::getBool;
    using android::hardware::configstore::V1_0::ISurfaceFlingerConfigs;
    if (!android::sysprop::start_graphics_allocator_service(false)) {
        return OK;
    }
    status_t result = hardware::registerPassthroughServiceImplementation<
            android::hardware::graphics::allocator::V3_0::IAllocator>();
    if (result == OK) {
        return OK;
    }
    result = hardware::registerPassthroughServiceImplementation<
            android::hardware::graphics::allocator::V2_0::IAllocator>();
    if (result != OK) {
        ALOGE("could not start graphics allocator service");
        return result;
    }
    return OK;
}
static void startDisplayService() {
    using android::frameworks::displayservice::V1_0::implementation::DisplayService;
    using android::frameworks::displayservice::V1_0::IDisplayService;
    sp<IDisplayService> displayservice = sp<DisplayService>::make();
    status_t err = displayservice->registerAsService();
    if (err != OK) {
        ALOGE("Did not register (deprecated) IDisplayService service.");
    }
}
int main(int, char**) {
    signal(SIGPIPE, SIG_IGN);
    hardware::configureRpcThreadpool(1 ,
            false );
    startGraphicsAllocatorService();
    ProcessState::self()->setThreadPoolMaxThreadCount(4);
    if (SurfaceFlinger::setSchedAttr(true) != NO_ERROR) {
        ALOGW("Failed to set uclamp.min during boot: %s", strerror(errno));
    }
    int newPriority = 0;
    int origPolicy = sched_getscheduler(0);
    struct sched_param origSchedParam;
    int errorInPriorityModification = sched_getparam(0, &origSchedParam);
    if (errorInPriorityModification == 0) {
        int policy = SCHED_FIFO;
        newPriority = sched_get_priority_min(policy);
        struct sched_param param;
        param.sched_priority = newPriority;
        errorInPriorityModification = sched_setscheduler(0, policy, &param);
    }
    sp<ProcessState> ps(ProcessState::self());
    ps->startThreadPool();
    if (errorInPriorityModification == 0) {
        errorInPriorityModification = sched_setscheduler(0, origPolicy, &origSchedParam);
    } else {
        ALOGE("Failed to set SurfaceFlinger binder threadpool priority to SCHED_FIFO");
    }
    sp<SurfaceFlinger> flinger = surfaceflinger::createSurfaceFlinger();
    if (errorInPriorityModification == 0) {
        flinger->setMinSchedulerPolicy(SCHED_FIFO, newPriority);
    }
    setpriority(PRIO_PROCESS, 0, PRIORITY_URGENT_DISPLAY);
    set_sched_policy(0, SP_FOREGROUND);
    flinger->init();
    sp<IServiceManager> sm(defaultServiceManager());
    sm->addService(String16(SurfaceFlinger::getServiceName()), flinger, false,
                   IServiceManager::DUMP_FLAG_PRIORITY_CRITICAL | IServiceManager::DUMP_FLAG_PROTO);
    sp<SurfaceComposerAIDL> composerAIDL = sp<SurfaceComposerAIDL>::make(flinger);
if (FlagManager::getInstance().misc1()) { composerAIDL->setMinSchedulerPolicy(SCHED_FIFO, newPriority); }
    sm->addService(String16("SurfaceFlingerAIDL"), composerAIDL, false,
                   IServiceManager::DUMP_FLAG_PRIORITY_CRITICAL | IServiceManager::DUMP_FLAG_PROTO);
    startDisplayService();
    if (SurfaceFlinger::setSchedFifo(true) != NO_ERROR) {
        ALOGW("Failed to set SCHED_FIFO during boot: %s", strerror(errno));
    }
    flinger->run();
    return 0;
}
#pragma clang diagnostic pop
