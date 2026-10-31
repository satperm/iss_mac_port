// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// tlg: spin-wait pause and a cheap cycle counter on x86-64 (Rosetta) and arm64 (the native GPU
// host, docs/macos/GPU_PROCESS_PLAN.md). The counter is for local durations only (units differ
// between the architectures); clocks shared with the guest use common/rdtsc.h.

#include "common/types.h"

#if defined(__x86_64__)
#include "common/cpu_relax.h"
#endif

namespace Common {

inline void CpuRelax() {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __builtin_arm_yield();
#endif
}

inline u64 CpuCycles() {
#if defined(__x86_64__)
    return __rdtsc();
#elif defined(__aarch64__)
    u64 value;
    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

} // namespace Common
