// SPDX-FileCopyrightText: Copyright 2026 satperm
// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: Apple MetalFX temporal upscaling for the Vulkan renderer on MoltenVK (macOS), from the
// Bloodborne macOS port's bbport_metalfx. The Metal objects behind the Vulkan images and queue come
// from VK_EXT_metal_objects. MetalFX encodes into its own Metal command buffer on MoltenVK's queue:
// the caller submits the Vulkan work that writes the inputs first (MoltenVK's synchronous submits
// commit in call order), and Metal runs the command buffers of one queue in commit order.
#pragma once

#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace TlgMetalFx {

struct Frame {
    VkDevice device;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    VkQueue queue;
    VkImage color;  ///< render size
    VkImage depth;  ///< render size
    VkImage motion; ///< render size, RG16F, pixels, current -> previous
    VkImage output; ///< output size
    uint32_t width, height, out_width, out_height;
    float jitter_x, jitter_y; ///< pixels
    float motion_scale_x, motion_scale_y;
    bool reset;          ///< discard history (camera cut, resize)
    bool hdr;            ///< linear HDR color (auto exposure); false for a tonemapped frame
    bool depth_reversed; ///< reversed-Z depth (inFAMOUS)
};

#ifdef __APPLE__
/// Whether this Mac supports the MetalFX temporal scaler.
bool Supported();
/// Encodes and commits one upscale. Call with the graphics queue lock held, right after the
/// Vulkan submission that produced the inputs. False (logged once) when MetalFX cannot run.
bool Encode(const Frame& frame);
/// Drops the scaler (sizes or formats change, or the upscaler is switched off).
void Release();
#else
inline bool Supported() {
    return false;
}
inline bool Encode(const Frame&) {
    return false;
}
inline void Release() {}
#endif

} // namespace TlgMetalFx
