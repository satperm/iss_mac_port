// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: TLG_EXPOSURE_LOG=1 (diagnostic, 2026-11-17): prints, once per presented frame, the guest
// memory of every tiny float image the game registered (1x1/2x2: its exposure and average
// luminance values), i.e. what the CPU reads back. For the "screen goes dark for 2-3 s" report.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <mutex>
#include "common/types.h"

namespace TlgExposureLog {

inline float Half(u16 h) {
    const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
    float v = exp == 0    ? std::ldexp(float(mant), -24)
              : exp == 31 ? (mant ? NAN : INFINITY)
                          : std::ldexp(float(mant | 1024), int(exp) - 25);
    return sign ? -v : v;
}

inline bool Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("TLG_EXPOSURE_LOG");
        return env && env[0] == '1';
    }();
    return on;
}

struct Entry {
    VAddr address;
    u32 format;
    u32 width, height;
};
inline std::mutex mutex;
inline std::array<Entry, 32> entries{};
inline u32 count = 0;

inline void Note(VAddr address, u32 format, u32 width = 0, u32 height = 0) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{mutex};
    for (u32 i = 0; i < count; ++i) {
        if (entries[i].address == address) {
            entries[i].format = format;
            entries[i].width = width;
            entries[i].height = height;
            return;
        }
    }
    if (count < entries.size()) {
        entries[count++] = {address, format, width, height};
        std::printf("Exposure log: tiny float image at %#llx (format %u, %ux%u)\n",
                    (unsigned long long)address, format, width, height);
    }
}

// vk::Format values: R16Sfloat 76, R16G16Sfloat 83, R16G16B16A16Sfloat 97, R32Sfloat 100,
// R32G32Sfloat 103.
inline void Print(u64 frame) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (!count) {
        return;
    }
    char line[1024];
    int n = std::snprintf(line, sizeof line, "Exposure log %llu:", (unsigned long long)frame);
    for (u32 i = 0; i < count && n < int(sizeof line) - 48; ++i) {
        const u8* p = reinterpret_cast<const u8*>(entries[i].address);
        const u32 f = entries[i].format;
        if (f == 83 && entries[i].width > 2) {
            // Luminance tiles: mean, max and NaN/Inf count of both channels over the image.
            const u32 pixels = entries[i].width * entries[i].height;
            double sum[2]{};
            float top[2]{};
            u32 bad = 0;
            for (u32 k = 0; k < pixels; ++k) {
                u16 h[2];
                std::memcpy(h, p + k * 4, 4);
                for (int c = 0; c < 2; ++c) {
                    const float v = Half(h[c]);
                    if (!std::isfinite(v)) {
                        ++bad;
                        continue;
                    }
                    sum[c] += v;
                    top[c] = std::max(top[c], v);
                }
            }
            n += std::snprintf(line + n, sizeof line - n, " %llx[mean %.4g,%.4g max %.4g,%.4g bad %u]",
                               (unsigned long long)(entries[i].address & 0xffffff), sum[0] / pixels,
                               sum[1] / pixels, top[0], top[1], bad);
            continue;
        }
        if (f == 100 || f == 103) {
            float v[2]{};
            std::memcpy(v, p, f == 103 ? 8 : 4);
            n += std::snprintf(line + n, sizeof line - n, " %llx=%.4g,%.4g",
                               (unsigned long long)(entries[i].address & 0xffffff), v[0], v[1]);
        } else {
            u16 h[2]{};
            std::memcpy(h, p, f == 76 ? 2 : 4);
            n += std::snprintf(line + n, sizeof line - n, " %llx=%.4g,%.4g",
                               (unsigned long long)(entries[i].address & 0xffffff),
                               Half(h[0]), Half(h[1]));
        }
    }
    std::puts(line);
}

} // namespace TlgExposureLog
