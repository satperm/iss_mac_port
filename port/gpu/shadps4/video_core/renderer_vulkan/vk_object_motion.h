// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: object motion vectors for temporal upscaling (docs/motion_vectors.md).
//
// G-buffer shaders preserve clip positions for the exact indexed range of each draw.
// Matching includes vertex streams, index contents and base offsets. A missing match falls
// back to camera motion. See motion_history.h for CPU bookkeeping and its regression test.

#pragma once

#include <array>
#include "video_core/renderer_vulkan/motion_history.h"

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;
struct RenderState;

class ObjectMotion {
public:
    ObjectMotion(const Instance& instance, Scheduler& scheduler);
    ~ObjectMotion();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    using DrawInfo = Motion::Draw;
    u32 PrepareDraw(const DrawInfo& draw);
    /// Small skeletons: whether the bone palette changed (Motion::History::Moving).
    bool Moving(const Motion::History::GateKey& key, u64 palette) {
        return history.Moving(key, palette);
    }
    /// Index range and topology hash of an indexed draw, reused across frames.
    template <class Scan>
    Motion::IndexRangeCache::Result IndexRange(const Motion::IndexRangeCache::Key& key,
                                               Scan&& scan) {
        return index_ranges.Get(key, frame, scan);
    }

    /// Rendering of a motion pipeline: the motion attachment (render-target size).
    void Attach(RenderState& state, u32 width, u32 height);

    /// Start of a frame (display pass).
    void OnFrameStart();
    void InvalidateHistory() { history.NextFrame(); history.NextFrame(); }

    /// The motion image for the compose pass (layout General after this call) and whether it
    /// holds this frame's vectors at `width` x `height`.
    vk::ImageView PrepareRead(vk::CommandBuffer cmdbuf, u32 width, u32 height, bool& valid);
    /// The image of the last PrepareRead (layout General), or null.
    [[nodiscard]] vk::ImageView View() const noexcept {
        return current && current->view ? *current->view : vk::ImageView{};
    }
    [[nodiscard]] vk::Image Image(u32 width, u32 height) const noexcept {
        const Target* target = FindTarget(width, height);
        return target && target->written ? vk::Image(target->image) : vk::Image{};
    }

private:
    /// One motion image per render-target size (Bloodborne port #160): one image that followed
    /// the size waited for the GPU (Scheduler::Finish) at every change and lost the frame's
    /// vectors when passes of two sizes alternated.
    struct Target {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u32 width = 0, height = 0;
        bool written = false; ///< this frame's vectors are in the image
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };
    static constexpr size_t MaxTargets = 8;
    const Target* FindTarget(u32 width, u32 height) const noexcept;
    Target& EnsureTarget(u32 width, u32 height);

    const Instance& instance;
    Scheduler& scheduler;
    bool enabled = false;

    // Parameter ring (host visible): FrameSlots frames of ParamsPerFrame entries (two u32x4),
    // element 0 all zero (motion off).
    static constexpr u32 FrameSlots = 4;
    static constexpr u32 ParamsPerFrame = 8192;
    // Positions (device local): two halves (current/previous frame) of vec4; element 0 is reserved.
    static constexpr u32 PositionsPerFrame = 4u << 20;
    Motion::History history{PositionsPerFrame};
    Motion::IndexRangeCache index_ranges;
    std::array<u64, FrameSlots> params_ticks{};
    vk::Buffer params_buffer{};
    VmaAllocation params_allocation{};
    u32* params_mapped{};
    vk::Buffer positions_buffer{};
    VmaAllocation positions_allocation{};

    u64 frame = 0;
    u32 params_used = 0;
    std::array<Target, MaxTargets> targets;
    size_t target_count = 0;
    Target* current = nullptr; ///< of the last Attach/PrepareRead
};

} // namespace Vulkan
