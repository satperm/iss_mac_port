// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#ifdef __APPLE__
/// macOS has no gettid(): the Mach thread id (unique per thread, small enough for u32 in practice).
static inline pid_t gettid() {
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return static_cast<pid_t>(id);
}
#endif

namespace TlgThreads {
#if   defined(__APPLE__)
/// Hardware threads available to the process (macOS has no affinity masks).
inline unsigned Available() {
    return std::max(1u, std::thread::hardware_concurrency());
}

/// Physical cores (Apple Silicon has no SMT, so the same as Available() there).
inline unsigned PhysicalCores() {
    int cores = 0;
    size_t size = sizeof(cores);
    if (sysctlbyname("hw.physicalcpu", &cores, &size, nullptr, 0) == 0 && cores > 0) {
        return static_cast<unsigned>(cores);
    }
    return Available();
}

/// QoS of the draw preparation helpers (scanner and workers). Background QoS keeps them on the
/// efficiency cores at low clocks: on an M3 Max (10 P + 4 E) 40% of Seattle's draws reached the GPU
/// thread before their worker had prepared them. With 8 or more performance cores they run as
/// user-initiated (99% prepared; lib_ab 2026-11-12: truck 65.4 -> 68.3, bridge 85.5 -> 87.2); with
/// fewer (M1-M4, the Pro chips with 5-6) they stay background so they never take a core from the game.
/// TLG_PREP_QOS=background|utility|default|user overrides.
inline void MakeBackground() {
    static const qos_class_t qos = [] {
        const char* env = std::getenv("TLG_PREP_QOS");
        if (env && *env) {
            if (!std::strcmp(env, "utility")) return QOS_CLASS_UTILITY;
            if (!std::strcmp(env, "default")) return QOS_CLASS_DEFAULT;
            if (!std::strcmp(env, "user")) return QOS_CLASS_USER_INITIATED;
            return QOS_CLASS_BACKGROUND;
        }
        int performance_cores = 0;
        size_t size = sizeof(performance_cores);
        if (sysctlbyname("hw.perflevel0.physicalcpu", &performance_cores, &size, nullptr, 0) == 0 &&
            performance_cores >= 8) {
            return QOS_CLASS_USER_INITIATED;
        }
        return QOS_CLASS_BACKGROUND;
    }();
    pthread_set_qos_class_self_np(qos, 0);
}
#else

/// Hardware threads available to the process.
inline unsigned Available() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// Physical cores (SMT siblings counted once) the process may run on: every allowed CPU's
/// thread_siblings_list starts with the number of its physical core. The hardware threads when
/// sysfs cannot be read.
inline unsigned PhysicalCores() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        return Available();
    }
    std::vector<int> cores;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &set)) {
            continue;
        }
        char path[128];
        std::snprintf(path, sizeof(path),
                      "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
        std::FILE* file = std::fopen(path, "r");
        if (!file) {
            return Available();
        }
        int core = -1;
        const int read = std::fscanf(file, "%d", &core);
        std::fclose(file);
        if (read != 1) {
            return Available();
        }
        if (std::find(cores.begin(), cores.end(), core) == cores.end()) {
            cores.push_back(core);
        }
    }
    return cores.empty() ? Available() : static_cast<unsigned>(cores.size());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
}
#endif

/// TLG_PHYS_CORES=1 (default off): the counts the spin budgets and the helper threads of the
/// draw pipeline are sized with come from the physical cores instead of the hardware threads,
/// so a 6 core / 12 thread machine spins and starts helpers like a 6 thread one. Logged once.
inline bool UsePhysicalCores() {
    static const bool on = [] {
        const char* env = std::getenv("TLG_PHYS_CORES");
        const bool enabled = env && env[0] == '1';
        if (enabled) {
            std::printf("GPU: helper sizing by physical cores (TLG_PHYS_CORES=1)\n");
        }
        return enabled;
    }();
    return on;
}

/// What the long-spin thresholds (DrawPipe, the recorder) and DefaultWorkerCount count:
/// TlgThreads::PhysicalCores() with TLG_PHYS_CORES=1, the hardware threads otherwise. Read
/// once (PhysicalCores() reads sysfs).
inline unsigned SizingCount() {
    static const unsigned count = UsePhysicalCores() ? PhysicalCores() : Available();
    return count;
}

} // namespace TlgThreads
