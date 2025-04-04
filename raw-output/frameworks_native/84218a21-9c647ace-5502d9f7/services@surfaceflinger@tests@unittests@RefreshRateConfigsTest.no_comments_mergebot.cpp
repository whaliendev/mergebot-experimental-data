#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra"
#undef LOG_TAG
#define LOG_TAG "SchedulerUnittests"
#include <ftl/enum.h>
#include <gmock/gmock.h>
#include <log/log.h>
#include <thread>
#include "../../Scheduler/RefreshRateConfigs.h"
#include <ui/Size.h>
#include "DisplayHardware/HWC2.h"
#include "FpsOps.h"
#include "Scheduler/RefreshRateConfigs.h"
using namespace std::chrono_literals;
