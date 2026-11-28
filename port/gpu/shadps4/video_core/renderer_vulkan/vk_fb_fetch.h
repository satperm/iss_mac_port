// SPDX-License-Identifier: GPL-2.0-or-later
// tlg (macOS): framebuffer fetch for colour feedback (on by default; TLG_MAC_FB_FETCH=0: off).
// The game's gbuffer decals write part of their targets and load the same targets at their own
// pixel; Apple GPUs have no barriers inside a render pass, so each such draw ended the Metal pass
// (store, then load again: ~50 restarts a frame in Seattle). When a fragment shader's only uses
// of an image are own-pixel level-0 loads (ImageResource::fb_fetchable) and the image is one of
// the draw's colour targets, the shader reads it as a subpass input, which MoltenVK turns into a
// [[color(n)]] read (patches/moltenvk-1.4.2-framebuffer-fetch.patch), and the pass stays open.
// Only without the native resolution patch: the game scales FragCoord by 1920 / render width.
// M3 Max, lib_ab 3 x 60 s (2026-11-12): Seattle 55.6 -> 62.1, bridge 81.0 -> 85.0, truck 65.0 -> 65.4
// (Seattle: pass restarts on the same attachments 3690 -> 408 a second); same picture.
#pragma once
#include <array>
#include <cstdlib>
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/regs_color.h"
#include "tlg_settings.h"

namespace Vulkan::FbFetch {

inline bool Enabled() {
#ifdef __APPLE__
    static const bool on = [] {
        const char* env = std::getenv("TLG_MAC_FB_FETCH");
        const char* scale = std::getenv("TLG_RENDER_SCALE");
        const char* half = std::getenv("TLG_BENCH_HALF_SCALE");
        // The launcher's render scale (TLG_RENDER_SCALE), the Detailed benchmark's half pass and a native
        // render scale below 100% move the loads off the own pixel (the game scales FragCoord by its
        // 1920 / render width; 1 at 100%). A native output (1440p, 4K) at 100% keeps them there:
        // Seattle 1440p, 174 pass restarts a frame on the decals' own targets without it.
        const auto& s = TlgSettings::Get();
        const bool native_scaled = s.iss_native_output.load() != 0 && s.iss_native_percent.load() != 100;
        return !(env && *env == '0') && !(scale && *scale) && !(half && *half == '1') && !native_scaled;
    }();
    return on;
#else
    return false;
#endif
}

/// The draw's colour targets that framebuffer fetch may read.
template <typename Regs>
inline void FillTargets(const Regs& regs, u32 mrt_mask,
                        std::array<Shader::HwFragmentRuntimeInfo::FbTarget, Shader::MaxColorBuffers>& out) {
    out = {};
    // Colour output disabled: no attachments are bound (Rasterizer::PrepareRenderState).
    if (!Enabled() || regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable) {
        return;
    }
    for (u32 cb = 0; cb < Shader::MaxColorBuffers; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!(mrt_mask & (1u << cb)) || !col_buf || !regs.color_target_mask.GetMask(cb)) {
            continue;
        }
        out[cb] = {col_buf.Address(), u32(col_buf.GetDataFmt()), u32(col_buf.GetNumberFmt())};
    }
}

} // namespace Vulkan::FbFetch
