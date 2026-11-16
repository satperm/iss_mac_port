// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: ISS draw distance and clip planes (src/view_hook.h). The probe verifies the eboot,
// installs the two code hooks and calls tlg_view_attach; a thread then keeps the game's data
// (LOD table, band distances, default clip planes) equal to the menu's values, so changes are
// live and survive the game rewriting them (debug LOD modes, script resets).
#include "tlg_quality.h"
#include "tlg_settings.h"
#include "tlg_split.h"
#include "tlg_toggles.h"
#include "../tlggpu.h"
#include "tlg_view.h"

#include "../../src/view_hook.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

unsigned char* image = nullptr;
// Read by the hook stubs: {LOD_ADJUST weight, 1.0f} and 1 / FADE_DISTANCE scale.
alignas(16) float region_params[2] = {1.0f, 1.0f};
alignas(16) float fade_inverse = 1.0f;
// Read by the lower-level stub (view_hook.h): the lowest dynamic quality level the game may use.
alignas(16) volatile int32_t quality_floor = 0;
// The level the automatic quality (tlg_quality.h) wants as the lowest, set by the view thread.
std::atomic<int> auto_level{0};

void Put(uint32_t at, float value) {
    float current;
    std::memcpy(&current, image + at, 4);
    if (current != value) {
        std::memcpy(image + at, &value, 4);
    }
}

// Frames shown so far and draws so far. In the GPU process split the renderer's counters live in the host:
// the game process reads the flip and draw counts the host publishes in the shared block; otherwise the
// runtime interface (which both renderers implement; renderer v2 does not update TlgStats).
uint64_t PresentedFrames() {
    if (TlgSplit::Remote()) {
        return __atomic_load_n(&TlgSplit::Shared()->host_flips, __ATOMIC_RELAXED);
    }
    TlgGpuFrameCounters counters{};
    tlggpu_frame_counters(&counters);
    return counters.presented;
}

uint64_t TotalDraws() {
    if (TlgSplit::Remote()) {
        return __atomic_load_n(&TlgSplit::Shared()->host_draws, __ATOMIC_RELAXED);
    }
    return tlggpu_total_draws();
}

void PutInt(uint32_t at, int32_t value) {
    int32_t current;
    std::memcpy(&current, image + at, 4);
    if (current != value) {
        std::memcpy(image + at, &value, 4);
    }
}

void Apply() {
    const auto& s = TlgSettings::Get();
    const float scale = TlgView::ClampLodScale(s.lod_scale);
    for (unsigned r = 0; r < TLG_VIEW_LOD_ROWS; ++r) {
        Put(TLG_VIEW_LOD_TABLE + r * TLG_VIEW_LOD_STRIDE, tlg_view_lod_original[r] * scale);
    }
    const int cull = s.dyn_cull;
    for (unsigned r = 0; r < TLG_VIEW_LOD_ROWS; ++r) {
        PutInt(TLG_VIEW_LOD_TABLE + r * TLG_VIEW_LOD_STRIDE + TLG_VIEW_CULL_OFFSET,
               cull == 1 ? 0 : cull == 2 ? tlg_view_cull_performance[r] : tlg_view_cull_original[r]);
    }
    for (unsigned b = 0; b < TLG_VIEW_BAND_COUNT; ++b) {
        Put(TLG_VIEW_BANDS + 4 * b, TlgView::ClampBand(s.lod_bands[b]));
    }
    Put(TLG_VIEW_NEAR_CLIP, TlgView::ClampNearClip(s.near_clip));
    Put(TLG_VIEW_FAR_CLIP, TlgView::ClampFarClip(s.far_clip));
    region_params[0] = TlgView::ClampRegionLod(s.region_lod);
    fade_inverse = 1.0f / TlgView::ClampRegionFade(s.region_fade);
    // The level the game lowers one step a frame is held at the floor by the stub; a level below
    // the floor (the floor was just raised) is lifted here, once.
    const int32_t floor_level = std::clamp<int32_t>(std::max<int32_t>(s.quality_floor, auto_level.load()), 0, TlgQuality::MaxLevel);
    quality_floor = floor_level;
    if (floor_level > 0) {
        int32_t level;
        std::memcpy(&level, image + TLG_VIEW_LEVEL, 4);
        if (level < floor_level) {
            std::memcpy(image + TLG_VIEW_LEVEL, &floor_level, 4);
        }
    }
}

} // namespace

extern "C" uintptr_t tlg_view_region_params(void) {
    return reinterpret_cast<uintptr_t>(region_params);
}

extern "C" uintptr_t tlg_view_floor_address(void) {
    return reinterpret_cast<uintptr_t>(&quality_floor);
}

extern "C" uintptr_t tlg_view_fade_inverse(void) {
    return reinterpret_cast<uintptr_t>(&fade_inverse);
}

extern "C" void tlg_view_attach(unsigned char* guest_image) {
    image = guest_image;
    Apply(); // before the game's first frame
    if (const int budget = TlgSettings::Get().draw_budget; budget < 100) {
        std::printf("Draw list budget: %d%% of the game's (%s)\n", budget,
                    tlg_view_apply_draw_budget(image, unsigned(budget)) ? "applied" : "unavailable, bytes differ");
    }
    const auto& s = TlgSettings::Get();
    std::printf("Draw distance: LOD scale %.2f, bands %.0f/%.0f/%.0f/%.0f/%.0f/%.0f, region LOD_ADJUST weight %.2f, "
                "FADE_DISTANCE x%.2f, near %.1f, far %.0f, texture LOD bias %+.2f\n",
                s.lod_scale.load(), s.lod_bands[0].load(), s.lod_bands[1].load(), s.lod_bands[2].load(),
                s.lod_bands[3].load(), s.lod_bands[4].load(), s.lod_bands[5].load(), s.region_lod.load(),
                s.region_fade.load(), s.near_clip.load(), s.far_clip.load(), s.mip_lod_bias.load());
    TlgSettings::Get().view_supported = true;
}

// Called by the probe once the image has its final page protections: the thread reads the image,
// so it must not start while the probe still has the whole image unmapped (protect(image, 0)).
extern "C" void tlg_view_start(void) {
    if (!image) {
        return;
    }
    std::thread([] {
        // Every 100 ms the menu's values are written; the level log is for A/B runs only.
        const bool log_levels = std::getenv("TLG_VIEW_LOG") != nullptr || TlgSettings::Get().dyn_cull != 0 ||
                                TlgSettings::Get().quality_target_fps != 0 ||
                                TlgSettings::Get().draw_budget < 100;
        int last_level = -1;
        TlgQuality::Governor governor;
        auto governor_at = std::chrono::steady_clock::now();
        uint64_t last_frames = PresentedFrames(), last_draws = TotalDraws();
        unsigned seen[TLG_VIEW_LOD_ROWS] = {};
        uint32_t max_a = 0, max_b = 0;
        auto report = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // Once a second the last second's frame rate feeds the automatic quality.
            if (const auto now = std::chrono::steady_clock::now(); now - governor_at >= std::chrono::seconds(1)) {
                const double seconds = std::chrono::duration<double>(now - governor_at).count();
                governor_at = now;
                const uint64_t frames = PresentedFrames(), draws = TotalDraws();
                const uint64_t frame_delta = frames - last_frames, draw_delta = draws - last_draws;
                last_frames = frames;
                last_draws = draws;
                auto_level = governor.Update(TlgSettings::Get().quality_target_fps, frame_delta / seconds,
                                             frame_delta ? double(draw_delta) / double(frame_delta) : 0.0);
            }
            Apply();
            if (!log_levels) {
                continue;
            }
            int level;
            std::memcpy(&level, image + TLG_VIEW_LEVEL, 4);
            if (level >= 0 && level < int(TLG_VIEW_LOD_ROWS)) {
                ++seen[level];
                last_level = level;
            }
            // How full the two frame command lists are (the game's budgets: 24 MB and 10 MB); the lists are
            // rewound each frame, so the sample is a random point of the frame: the window's maximum counts.
            auto word = [](uint32_t offset) {
                uint32_t value;
                std::memcpy(&value, image + TLG_VIEW_STATE + offset, 4);
                return value;
            };
            const uint32_t start_a = word(0xb8 + 8 * (word(0xe0) & 1)), start_b = word(0xe8 + 8 * (word(0x110) & 1));
            const uint32_t used_a = word(0xc8) - start_a, used_b = word(0xf8) - start_b;
            if (used_a < (1u << 28)) max_a = std::max(max_a, used_a);
            if (used_b < (1u << 28)) max_b = std::max(max_b, used_b);
            // The game's dynamic quality levels seen in each 5 s window (100 ms samples).
            if (std::chrono::steady_clock::now() - report >= std::chrono::seconds(5)) {
                report = std::chrono::steady_clock::now();
                std::printf("Dynamic quality: level %d, draw lists max %u/%u KB, automatic floor %d, samples per level",
                            last_level, max_a >> 10, max_b >> 10, auto_level.load());
                max_a = max_b = 0;
                for (unsigned r = 0; r < TLG_VIEW_LOD_ROWS; ++r) {
                    if (seen[r]) std::printf(" %u:%u", r, seen[r]);
                    seen[r] = 0;
                }
                std::putchar('\n');
            }
        }
    }).detach();
}
