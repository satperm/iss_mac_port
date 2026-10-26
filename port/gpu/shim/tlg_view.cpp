// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: ISS draw distance and clip planes (src/view_hook.h). The probe verifies the eboot,
// installs the two code hooks and calls tlg_view_attach; a thread then keeps the game's data
// (LOD table, band distances, default clip planes) equal to the menu's values, so changes are
// live and survive the game rewriting them (debug LOD modes, script resets).
#include "tlg_settings.h"
#include "tlg_view.h"

#include "../../src/view_hook.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

unsigned char* image = nullptr;
// Read by the hook stubs: {LOD_ADJUST weight, 1.0f} and 1 / FADE_DISTANCE scale.
alignas(16) float region_params[2] = {1.0f, 1.0f};
alignas(16) float fade_inverse = 1.0f;

void Put(uint32_t at, float value) {
    float current;
    std::memcpy(&current, image + at, 4);
    if (current != value) {
        std::memcpy(image + at, &value, 4);
    }
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
}

} // namespace

extern "C" uintptr_t tlg_view_region_params(void) {
    return reinterpret_cast<uintptr_t>(region_params);
}

extern "C" uintptr_t tlg_view_fade_inverse(void) {
    return reinterpret_cast<uintptr_t>(&fade_inverse);
}

extern "C" void tlg_view_attach(unsigned char* guest_image) {
    image = guest_image;
    Apply(); // before the game's first frame
    const auto& s = TlgSettings::Get();
    std::printf("Draw distance: LOD scale %.2f, bands %.0f/%.0f/%.0f/%.0f/%.0f/%.0f, region LOD_ADJUST weight %.2f, "
                "FADE_DISTANCE x%.2f, near %.1f, far %.0f, texture LOD bias %+.2f\n",
                s.lod_scale.load(), s.lod_bands[0].load(), s.lod_bands[1].load(), s.lod_bands[2].load(),
                s.lod_bands[3].load(), s.lod_bands[4].load(), s.lod_bands[5].load(), s.region_lod.load(),
                s.region_fade.load(), s.near_clip.load(), s.far_clip.load(), s.mip_lod_bias.load());
    TlgSettings::Get().view_supported = true;
    std::thread([] {
        int last_level = -1;
        unsigned seen[TLG_VIEW_LOD_ROWS] = {};
        auto report = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Apply();
            int level;
            std::memcpy(&level, image + TLG_VIEW_LEVEL, 4);
            if (level >= 0 && level < int(TLG_VIEW_LOD_ROWS)) {
                ++seen[level];
                last_level = level;
            }
            // The game's dynamic quality levels seen in each 5 s window (100 ms samples).
            if (std::chrono::steady_clock::now() - report >= std::chrono::seconds(5)) {
                report = std::chrono::steady_clock::now();
                std::printf("Dynamic quality: level %d, samples per level", last_level);
                for (unsigned r = 0; r < TLG_VIEW_LOD_ROWS; ++r) {
                    if (seen[r]) std::printf(" %u:%u", r, seen[r]);
                    seen[r] = 0;
                }
                std::printf("\n");
            }
        }
    }).detach();
}
