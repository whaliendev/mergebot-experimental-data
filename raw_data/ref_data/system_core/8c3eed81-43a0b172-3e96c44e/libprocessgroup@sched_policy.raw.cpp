/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <processgroup/sched_policy.h>

#define LOG_TAG "SchedPolicy"

#include <errno.h>
#include <unistd.h>

#include <android-base/logging.h>
#include <android-base/threads.h>
#include <cgroup_map.h>
#include <processgroup/processgroup.h>

using android::base::GetThreadId;

/* Re-map SP_DEFAULT to the system default policy, and leave other values unchanged.
 * Call this any place a SchedPolicy is used as an input parameter.
 * Returns the possibly re-mapped policy.
 */
static inline SchedPolicy _policy(SchedPolicy p) {
    return p == SP_DEFAULT ? SP_SYSTEM_DEFAULT : p;
}

<<<<<<< HEAD
#if defined(__ANDROID__)

int set_cpuset_policy(int tid, SchedPolicy policy) {
||||||| 3e96c44e3
#if defined(__ANDROID__)

#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>

#define POLICY_DEBUG 0

// timer slack value in nS enforced when the thread moves to background
#define TIMER_SLACK_BG 40000000
#define TIMER_SLACK_FG 50000

static pthread_once_t the_once = PTHREAD_ONCE_INIT;

static int __sys_supports_timerslack = -1;

// File descriptors open to /dev/cpuset/../tasks, setup by initialize, or -1 on error
static int system_bg_cpuset_fd = -1;
static int bg_cpuset_fd = -1;
static int fg_cpuset_fd = -1;
static int ta_cpuset_fd = -1; // special cpuset for top app
static int rs_cpuset_fd = -1;  // special cpuset for screen off restrictions

// File descriptors open to /dev/stune/../tasks, setup by initialize, or -1 on error
static int bg_schedboost_fd = -1;
static int fg_schedboost_fd = -1;
static int ta_schedboost_fd = -1;
static int rt_schedboost_fd = -1;

/* Add tid to the scheduling group defined by the policy */
static int add_tid_to_cgroup(int tid, int fd)
{
    if (fd < 0) {
        SLOGE("add_tid_to_cgroup failed; fd=%d\n", fd);
        errno = EINVAL;
        return -1;
    }

    // specialized itoa -- works for tid > 0
    char text[22];
    char *end = text + sizeof(text) - 1;
    char *ptr = end;
    *ptr = '\0';
    while (tid > 0) {
        *--ptr = '0' + (tid % 10);
        tid = tid / 10;
    }

    if (write(fd, ptr, end - ptr) < 0) {
        /*
         * If the thread is in the process of exiting,
         * don't flag an error
         */
        if (errno == ESRCH)
                return 0;
        SLOGW("add_tid_to_cgroup failed to write '%s' (%s); fd=%d\n",
              ptr, strerror(errno), fd);
        errno = EINVAL;
        return -1;
    }

    return 0;
}

/*
    If CONFIG_CPUSETS for Linux kernel is set, "tasks" can be found under
    /dev/cpuset mounted in init.rc; otherwise, that file does not exist
    even though the directory, /dev/cpuset, is still created (by init.rc).

    A couple of other candidates (under cpuset mount directory):
        notify_on_release
        release_agent

    Yet another way to decide if cpuset is enabled is to parse
    /proc/self/status and search for lines begin with "Mems_allowed".

    If CONFIG_PROC_PID_CPUSET is set, the existence "/proc/self/cpuset" can
    be used to decide if CONFIG_CPUSETS is set, so we don't have a dependency
    on where init.rc mounts cpuset. That's why we'd better require this
    configuration be set if CONFIG_CPUSETS is set.

    In older releases, this was controlled by build-time configuration.
 */
bool cpusets_enabled() {
    static bool enabled = (access("/dev/cpuset/tasks", F_OK) == 0);

    return enabled;
}

/*
    Similar to CONFIG_CPUSETS above, but with a different configuration
    CONFIG_CGROUP_SCHEDTUNE that's in Android common Linux kernel and Linaro
    Stable Kernel (LSK), but not in mainline Linux as of v4.9.

    In older releases, this was controlled by build-time configuration.
 */
bool schedboost_enabled() {
    static bool enabled = (access("/dev/stune/tasks", F_OK) == 0);

    return enabled;
}

static void __initialize() {
    const char* filename;

    if (cpusets_enabled()) {
        if (!access("/dev/cpuset/tasks", W_OK)) {

            filename = "/dev/cpuset/foreground/tasks";
            fg_cpuset_fd = open(filename, O_WRONLY | O_CLOEXEC);
            filename = "/dev/cpuset/background/tasks";
            bg_cpuset_fd = open(filename, O_WRONLY | O_CLOEXEC);
            filename = "/dev/cpuset/system-background/tasks";
            system_bg_cpuset_fd = open(filename, O_WRONLY | O_CLOEXEC);
            filename = "/dev/cpuset/top-app/tasks";
            ta_cpuset_fd = open(filename, O_WRONLY | O_CLOEXEC);
            filename = "/dev/cpuset/restricted/tasks";
            rs_cpuset_fd = open(filename, O_WRONLY | O_CLOEXEC);

            if (schedboost_enabled()) {
                filename = "/dev/stune/top-app/tasks";
                ta_schedboost_fd = open(filename, O_WRONLY | O_CLOEXEC);
                filename = "/dev/stune/foreground/tasks";
                fg_schedboost_fd = open(filename, O_WRONLY | O_CLOEXEC);
                filename = "/dev/stune/background/tasks";
                bg_schedboost_fd = open(filename, O_WRONLY | O_CLOEXEC);
                filename = "/dev/stune/rt/tasks";
                rt_schedboost_fd = open(filename, O_WRONLY | O_CLOEXEC);
            }
        }
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "/proc/%d/timerslack_ns", getpid());
    __sys_supports_timerslack = !access(buf, W_OK);
}

/*
 * Returns the path under the requested cgroup subsystem (if it exists)
 *
 * The data from /proc/<pid>/cgroup looks (something) like:
 *  2:cpu:/bg_non_interactive
 *  1:cpuacct:/
 *
 * We return the part after the "/", which will be an empty string for
 * the default cgroup.  If the string is longer than "bufLen", the string
 * will be truncated.
 */
static int getCGroupSubsys(int tid, const char* subsys, char* buf, size_t bufLen)
{
#if defined(__ANDROID__)
    char pathBuf[32];
    char lineBuf[256];
    FILE *fp;

    snprintf(pathBuf, sizeof(pathBuf), "/proc/%d/cgroup", tid);
    if (!(fp = fopen(pathBuf, "re"))) {
        return -1;
    }

    while(fgets(lineBuf, sizeof(lineBuf) -1, fp)) {
        char *next = lineBuf;
        char *found_subsys;
        char *grp;
        size_t len;

        /* Junk the first field */
        if (!strsep(&next, ":")) {
            goto out_bad_data;
        }

        if (!(found_subsys = strsep(&next, ":"))) {
            goto out_bad_data;
        }

        if (strcmp(found_subsys, subsys)) {
            /* Not the subsys we're looking for */
            continue;
        }

        if (!(grp = strsep(&next, ":"))) {
            goto out_bad_data;
        }
        grp++; /* Drop the leading '/' */
        len = strlen(grp);
        grp[len-1] = '\0'; /* Drop the trailing '\n' */

        if (bufLen <= len) {
            len = bufLen - 1;
        }
        strncpy(buf, grp, len);
        buf[len] = '\0';
        fclose(fp);
        return 0;
    }

    SLOGE("Failed to find subsys %s", subsys);
    fclose(fp);
    return -1;
 out_bad_data:
    SLOGE("Bad cgroup data {%s}", lineBuf);
    fclose(fp);
    return -1;
#else
    errno = ENOSYS;
    return -1;
#endif
}

int get_sched_policy(int tid, SchedPolicy *policy)
{
=======
int set_cpuset_policy(int tid, SchedPolicy policy) {
>>>>>>> 43a0b172
    if (tid == 0) {
        tid = GetThreadId();
    }
    policy = _policy(policy);

    switch (policy) {
        case SP_BACKGROUND:
            return SetTaskProfiles(tid,
                                   {"HighEnergySaving", "ProcessCapacityLow", "TimerSlackHigh"})
                           ? 0
                           : -1;
        case SP_FOREGROUND:
        case SP_AUDIO_APP:
        case SP_AUDIO_SYS:
            return SetTaskProfiles(tid,
                                   {"HighPerformance", "ProcessCapacityHigh", "TimerSlackNormal"})
                           ? 0
                           : -1;
        case SP_TOP_APP:
            return SetTaskProfiles(tid,
                                   {"MaxPerformance", "ProcessCapacityMax", "TimerSlackNormal"})
                           ? 0
                           : -1;
        case SP_SYSTEM:
            return SetTaskProfiles(tid, {"ServiceCapacityLow", "TimerSlackNormal"}) ? 0 : -1;
        case SP_RESTRICTED:
            return SetTaskProfiles(tid, {"ServiceCapacityRestricted", "TimerSlackNormal"}) ? 0 : -1;
        default:
            break;
    }

    return 0;
}

int set_sched_policy(int tid, SchedPolicy policy) {
    if (tid == 0) {
        tid = GetThreadId();
    }
    policy = _policy(policy);

#if POLICY_DEBUG
    char statfile[64];
    char statline[1024];
    char thread_name[255];

    snprintf(statfile, sizeof(statfile), "/proc/%d/stat", tid);
    memset(thread_name, 0, sizeof(thread_name));

    unique_fd fd(TEMP_FAILURE_RETRY(open(statfile, O_RDONLY | O_CLOEXEC)));
    if (fd >= 0) {
        int rc = read(fd, statline, 1023);
        statline[rc] = 0;
        char* p = statline;
        char* q;

        for (p = statline; *p != '('; p++)
            ;
        p++;
        for (q = p; *q != ')'; q++)
            ;

        strncpy(thread_name, p, (q - p));
    }
    switch (policy) {
        case SP_BACKGROUND:
            SLOGD("vvv tid %d (%s)", tid, thread_name);
            break;
        case SP_FOREGROUND:
        case SP_AUDIO_APP:
        case SP_AUDIO_SYS:
        case SP_TOP_APP:
            SLOGD("^^^ tid %d (%s)", tid, thread_name);
            break;
        case SP_SYSTEM:
            SLOGD("/// tid %d (%s)", tid, thread_name);
            break;
        case SP_RT_APP:
            SLOGD("RT  tid %d (%s)", tid, thread_name);
            break;
        default:
            SLOGD("??? tid %d (%s)", tid, thread_name);
            break;
    }
#endif

    switch (policy) {
        case SP_BACKGROUND:
            return SetTaskProfiles(tid, {"HighEnergySaving", "TimerSlackHigh"}) ? 0 : -1;
        case SP_FOREGROUND:
        case SP_AUDIO_APP:
        case SP_AUDIO_SYS:
            return SetTaskProfiles(tid, {"HighPerformance", "TimerSlackNormal"}) ? 0 : -1;
        case SP_TOP_APP:
            return SetTaskProfiles(tid, {"MaxPerformance", "TimerSlackNormal"}) ? 0 : -1;
        case SP_RT_APP:
            return SetTaskProfiles(tid, {"RealtimePerformance", "TimerSlackNormal"}) ? 0 : -1;
        default:
            return SetTaskProfiles(tid, {"TimerSlackNormal"}) ? 0 : -1;
    }

<<<<<<< HEAD
    return 0;
}

bool cpusets_enabled() {
    static bool enabled = (CgroupMap::GetInstance().FindController("cpuset") != nullptr);
    return enabled;
}

bool schedboost_enabled() {
    static bool enabled = (CgroupMap::GetInstance().FindController("schedtune") != nullptr);
    return enabled;
}

static int getCGroupSubsys(int tid, const char* subsys, std::string& subgroup) {
    const CgroupController* controller = CgroupMap::GetInstance().FindController(subsys);

    if (!controller) return -1;

    if (!controller->GetTaskGroup(tid, &subgroup)) {
        PLOG(ERROR) << "Failed to find cgroup for tid " << tid;
        return -1;
    }
    return 0;
}

int get_sched_policy(int tid, SchedPolicy* policy) {
    if (tid == 0) {
        tid = GetThreadId();
    }

    std::string group;
    if (schedboost_enabled()) {
        if (getCGroupSubsys(tid, "schedtune", group) < 0) return -1;
    }
    if (group.empty() && cpusets_enabled()) {
        if (getCGroupSubsys(tid, "cpuset", group) < 0) return -1;
    }

    // TODO: replace hardcoded directories
    if (group.empty()) {
        *policy = SP_FOREGROUND;
    } else if (group == "foreground") {
        *policy = SP_FOREGROUND;
    } else if (group == "system-background") {
        *policy = SP_SYSTEM;
    } else if (group == "background") {
        *policy = SP_BACKGROUND;
    } else if (group == "top-app") {
        *policy = SP_TOP_APP;
    } else if (group == "restricted") {
        *policy = SP_RESTRICTED;
    } else {
        errno = ERANGE;
        return -1;
    }
||||||| 3e96c44e3
    set_timerslack_ns(tid, policy == SP_BACKGROUND ? TIMER_SLACK_BG : TIMER_SLACK_FG);

=======
>>>>>>> 43a0b172
    return 0;
}

bool cpusets_enabled() {
    static bool enabled = (CgroupMap::GetInstance().FindController("cpuset") != nullptr);
    return enabled;
}

bool schedboost_enabled() {
    static bool enabled = (CgroupMap::GetInstance().FindController("schedtune") != nullptr);
    return enabled;
}

<<<<<<< HEAD
int set_sched_policy(int, SchedPolicy) {
||||||| 3e96c44e3
int set_sched_policy(int /*tid*/, SchedPolicy /*policy*/) {
=======
static int getCGroupSubsys(int tid, const char* subsys, std::string& subgroup) {
    const CgroupController* controller = CgroupMap::GetInstance().FindController(subsys);

    if (!controller) return -1;

    if (!controller->GetTaskGroup(tid, &subgroup)) {
        PLOG(ERROR) << "Failed to find cgroup for tid " << tid;
        return -1;
    }
>>>>>>> 43a0b172
    return 0;
}

<<<<<<< HEAD
int get_sched_policy(int, SchedPolicy* policy) {
    *policy = SP_SYSTEM_DEFAULT;
||||||| 3e96c44e3
int get_sched_policy(int /*tid*/, SchedPolicy* policy) {
    *policy = SP_SYSTEM_DEFAULT;
=======
int get_sched_policy(int tid, SchedPolicy* policy) {
    if (tid == 0) {
        tid = GetThreadId();
    }

    std::string group;
    if (schedboost_enabled()) {
        if (getCGroupSubsys(tid, "schedtune", group) < 0) return -1;
    }
    if (group.empty() && cpusets_enabled()) {
        if (getCGroupSubsys(tid, "cpuset", group) < 0) return -1;
    }

    // TODO: replace hardcoded directories
    if (group.empty()) {
        *policy = SP_FOREGROUND;
    } else if (group == "foreground") {
        *policy = SP_FOREGROUND;
    } else if (group == "system-background") {
        *policy = SP_SYSTEM;
    } else if (group == "background") {
        *policy = SP_BACKGROUND;
    } else if (group == "top-app") {
        *policy = SP_TOP_APP;
    } else if (group == "restricted") {
        *policy = SP_RESTRICTED;
    } else {
        errno = ERANGE;
        return -1;
    }
>>>>>>> 43a0b172
    return 0;
}

const char* get_sched_policy_name(SchedPolicy policy) {
    policy = _policy(policy);
    static const char* const kSchedPolicyNames[] = {
            [SP_BACKGROUND] = "bg", [SP_FOREGROUND] = "fg", [SP_SYSTEM] = "  ",
            [SP_AUDIO_APP] = "aa",  [SP_AUDIO_SYS] = "as",  [SP_TOP_APP] = "ta",
            [SP_RT_APP] = "rt",     [SP_RESTRICTED] = "rs",
    };
    static_assert(arraysize(kSchedPolicyNames) == SP_CNT, "missing name");
    if (policy < SP_BACKGROUND || policy >= SP_CNT) {
        return "error";
    }
    return kSchedPolicyNames[policy];
}
