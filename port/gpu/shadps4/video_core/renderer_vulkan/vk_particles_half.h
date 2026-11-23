// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: TLG_PARTICLES_HALF=1: fire and smoke particle draws at half resolution
// (docs/DESIGN_HALF_RES_PARTICLES.md). Draws of the listed pixel shaders (TLG_PARTICLES_HALF_PS,
// hex, comma separated) that blend over (or add to) a full-size RGBA16F target without depth go
// to a half-size image instead: rgb accumulates their colour, a the scene's transmittance
// (alpha blend Zero / OneMinusSrcAlpha, or Zero / One for additive draws). When that render
// pass ends the image is composited into the target: scene = scene * a + rgb.
#pragma once

#include <array>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;
struct RenderState;
struct GraphicsPipelineKey;

class ParticlesHalf {
public:
    ParticlesHalf(const Instance& instance, Scheduler& scheduler);
    ~ParticlesHalf();

    [[nodiscard]] static bool Enabled();
    /// Whether draws of this pipeline go to the half-size image (blend and shader checks).
    [[nodiscard]] static bool Candidate(const GraphicsPipelineKey& key, u64 ps_hash);
    /// The candidate's blend state for the half-size image (alpha = transmittance).
    static void AdjustKey(GraphicsPipelineKey& key);

    /// A draw of a candidate pipeline: colour 0 of `state` becomes the half-size image and
    /// `target_scale` is halved. False (state unchanged) when the pass does not qualify.
    bool Redirect(RenderState& state, std::array<float, 2>& target_scale, vk::Format format);

    /// Right before a redirected draw begins its pass: a run that ended in between (a barrier
    /// closed the pass and composited it) starts again.
    void EnsureRun();

    /// Any other draw: composites an open run first, before the draw emits its dynamic state
    /// (the composite sets its own viewport and scissor).
    void Close();

    /// Scheduler::EndRendering hook: composites the run when its half-size pass ended.
    void OnEndRendering(const RenderState& ended);

private:
    void CreateResources();
    void StartRun();
    vk::Pipeline CompositePipeline(vk::Format format);

    const Instance& instance;
    Scheduler& scheduler;

    /// One half-size image per target size (passes of two sizes alternate in a frame).
    struct Half {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u32 width = 0, height = 0;
    };
    std::array<Half, 4> halves;
    size_t half_count = 0;
    Half* current = nullptr; ///< of the open run

    bool active = false;
    vk::ImageView target_view{};
    vk::ImageLayout target_layout{};
    vk::Format target_format{};
    u32 target_width = 0, target_height = 0;

    vk::UniqueShaderModule vert, frag;
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    std::array<std::pair<vk::Format, vk::UniquePipeline>, 4> pipelines{};

    u64 runs = 0, draws = 0;
};

} // namespace Vulkan
