// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// tlg: TLG_UNIFIED_MEMORY=1 (ISS_ too): a range of guest direct memory shared with the GPU as on
// the PS4. From the Castlesea ISS port (gpu/unified_memory.h, 8e167dc), adapted to this
// port's draw pipe.
//
// The range (TLG_UNIFIED_RANGE=base:size; default the 512 MiB heap ISS maps at 0x1048000000,
// its per-frame data: command buffers, constants, rings) is imported host memory: its backing
// view (runtime_memory.c, never protected) bound as one Vulkan buffer (BufferCache::
// EnableUnified). Bindings in it skip uploads, stream copies, page protection and readbacks: the
// GPU reads and writes the guest's own bytes.
//
// What keeps it correct, as on the hardware:
// - end-of-pipe fences (EOP/EOS/RELEASE_MEM) whose label lands in the range are written when the
//   GPU has completed the work before them, after a host barrier (Rasterizer::DeferUnifiedFence),
//   so the guest reuses the memory only when the GPU is done with it;
// - command processor writes into it (WRITE_DATA, DMA) stay immediate, as before;
// - command processor reads of it (WAIT_REG_MEM) wait for the GPU writes recorded before them
//   (BufferCache::SyncUnifiedRead).
#pragma once

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "common/types.h"
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace VideoCore::Unified {

// tlg (macOS): on by default on Macs with 24 GB or less (as bloodborne_mac_port's memory layout for
// small Macs): zero-copy over all of the game's GPU memory halves the footprint (cannery 15.8 ->
// 7.4 GB) at ~15% lower frame rate (fences wait for the GPU). TLG_UNIFIED_MEMORY=0/1 chooses.
inline bool SmallMac() {
#ifdef __APPLE__
    u64 ram = 0;
    size_t len = sizeof(ram);
    return sysctlbyname("hw.memsize", &ram, &len, nullptr, 0) == 0 && ram <= (24ull << 30);
#else
    return false;
#endif
}

inline bool Enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("TLG_UNIFIED_MEMORY");
        if (v && *v) {
            return v[0] == '1';
        }
        return SmallMac();
    }();
    return enabled;
}

struct Range {
#ifdef __APPLE__
    // macOS: the game's direct memory from USER_MIN (runtime_memory.c), as it maps it for the GPU:
    // 0x7000000000 + 1152 MiB, its per-frame heap at 0x7048000000 + 512 MiB, then 3392 MiB
    // (TLG_MAP_LOG=1); 64 KiB short of the retail 5056 MiB.
    VAddr base = 0x7000000000ULL;
    u64 size = 0x13bff0000ULL;
#else
    VAddr base = 0x1048000000ULL;
    u64 size = 0x20000000ULL;
#endif
};

inline const Range& GetRange() {
    static const Range range = [] {
        Range r;
        if (const char* v = std::getenv("TLG_UNIFIED_RANGE")) {
            char* end{};
            const u64 base = std::strtoull(v, &end, 0);
            if (end && *end == ':') {
                if (const u64 size = std::strtoull(end + 1, nullptr, 0)) {
                    r = {base, size};
                }
            }
        }
        return r;
    }();
    return range;
}

enum Stat : u32 {
    Reads,           ///< bindings read in place
    Writes,          ///< bindings the GPU writes
    CpWrites,        ///< immediate command processor writes into the range
    FencesDeferred,  ///< labels written at GPU completion
    CpReadSyncs,     ///< command processor reads that waited for GPU writes
    CpReadWaitUs,    ///< time they waited
    StatCount,
};

inline std::array<std::atomic<u64>, StatCount>& Stats() {
    static std::array<std::atomic<u64>, StatCount> stats{};
    return stats;
}

inline void Add(Stat stat, u64 value = 1) {
    Stats()[stat].fetch_add(value, std::memory_order_relaxed);
}

/// "Unified memory (5 s): ..." once every 5 seconds (the caller's clock, in nanoseconds).
inline void MaybeReport(u64 now_ns) {
    static std::atomic<u64> last{0};
    u64 prev = last.load(std::memory_order_relaxed);
    if (!Enabled() || now_ns - prev < 5'000'000'000ULL ||
        !last.compare_exchange_strong(prev, now_ns)) {
        return;
    }
    auto& s = Stats();
    const auto take = [&](Stat stat) {
        return static_cast<unsigned long long>(s[stat].exchange(0, std::memory_order_relaxed));
    };
    const auto reads = take(Reads), writes = take(Writes), cp_writes = take(CpWrites),
               fences = take(FencesDeferred), syncs = take(CpReadSyncs), wait = take(CpReadWaitUs);
    std::printf("Unified memory (5 s): %llu reads, %llu writes in place, %llu CP writes, %llu fences "
                "deferred, %llu CP read syncs (%.1f ms)\n",
                reads, writes, cp_writes, fences, syncs, wait / 1000.0);
}

} // namespace VideoCore::Unified
