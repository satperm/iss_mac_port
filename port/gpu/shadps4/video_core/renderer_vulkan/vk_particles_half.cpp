// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_particles_half.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/particle_composite_frag.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {
using Factor = AmdGpu::BlendControl::BlendFactor;
using Func = AmdGpu::BlendControl::BlendFunc;

const std::vector<u32>& Shaders() {
    static const std::vector<u32> list = [] {
        std::vector<u32> out;
        const char* env = std::getenv("TLG_PARTICLES_HALF_PS");
        const char* p = env && *env ? env : "24dad4be";
        while (*p) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 16);
            if (end == p) {
                break;
            }
            out.push_back(u32(v));
            p = *end == ',' ? end + 1 : end;
        }
        std::printf("Particles: half resolution for %zu pixel shaders\n", out.size());
        return out;
    }();
    return list;
}

/// The transmittance factor that matches the colour blend (dst *= it), or false.
bool AlphaDstFor(const AmdGpu::BlendControl& b, Factor& dst) {
    if (!b.enable || b.color_func != Func::Add) {
        return false;
    }
    const bool src_ok = b.color_src_factor == Factor::One || b.color_src_factor == Factor::SrcAlpha ||
                        b.color_src_factor == Factor::Zero;
    if (!src_ok) {
        return false;
    }
    if (b.color_dst_factor == Factor::OneMinusSrcAlpha) {
        dst = Factor::OneMinusSrcAlpha;
        return true;
    }
    if (b.color_dst_factor == Factor::One) {
        dst = Factor::One;
        return true;
    }
    return false;
}
} // namespace

bool ParticlesHalf::Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("TLG_PARTICLES_HALF");
        return env && *env == '1';
    }();
    return enabled;
}

bool ParticlesHalf::Candidate(const GraphicsPipelineKey& key, u64 ps_hash) {
    if (!Enabled() || key.num_color_attachments != 1 ||
        key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid) {
        return false;
    }
    const auto& cb = key.color_buffers[0];
    if (cb.data_format != AmdGpu::DataFormat::Format16_16_16_16 ||
        cb.num_format != AmdGpu::NumberFormat::Float) {
        return false;
    }
    Factor dst;
    if (!AlphaDstFor(key.blend_controls[0], dst)) {
        return false;
    }
    for (const u32 hash : Shaders()) {
        if (hash == ps_hash) {
            return true;
        }
    }
    return false;
}

void ParticlesHalf::AdjustKey(GraphicsPipelineKey& key) {
    auto& b = key.blend_controls[0];
    Factor dst;
    if (!AlphaDstFor(b, dst)) {
        return;
    }
    b.separate_alpha_blend = 1;
    b.alpha_src_factor = Factor::Zero;
    b.alpha_func = Func::Add;
    b.alpha_dst_factor = dst;
    key.write_masks[0] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
}

ParticlesHalf::ParticlesHalf(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    CreateResources();
    std::printf("Particles: half resolution on (TLG_PARTICLES_HALF=1)\n");
}

ParticlesHalf::~ParticlesHalf() {
    if (runs) {
        std::printf("Particles: %llu half-resolution runs, %llu draws, %llu at full size\n",
                    (unsigned long long)runs, (unsigned long long)draws, (unsigned long long)skipped);
    }
}

void ParticlesHalf::CreateResources() {
    const auto device = instance.GetDevice();
    vert = vk::UniqueShaderModule(CompileSPV(FS_TRI_VERT, device), device);
    frag = vk::UniqueShaderModule(CompileSPV(PARTICLE_COMPOSITE_FRAG, device), device);
    sampler = Check(device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = 0.f,
    }));
    const vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eCombinedImageSampler, 1,
                                                 vk::ShaderStageFlagBits::eFragment};
    set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1,
        .pBindings = &binding,
    }));
    const auto sl = *set_layout;
    layout = Check(device.createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &sl}));
}

vk::Pipeline ParticlesHalf::CompositePipeline(vk::Format format) {
    for (const auto& [f, pipeline] : pipelines) {
        if (pipeline && f == format) {
            return *pipeline;
        }
    }
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state{};
    const std::array dynamic_states{vk::DynamicState::eViewportWithCount,
                                    vk::DynamicState::eScissorWithCount};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = u32(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eVertex,
                                          .module = *vert, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eFragment,
                                          .module = *frag, .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    // scene = particles.rgb + scene * particles.a; the scene's alpha stays.
    const vk::PipelineColorBlendAttachmentState blend{
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eOne,
        .dstColorBlendFactor = vk::BlendFactor::eSrcAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eZero,
        .dstAlphaBlendFactor = vk::BlendFactor::eOne,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .attachmentCount = 1,
        .pAttachments = &blend,
    };
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineRasterizationStateCreateInfo raster{.lineWidth = 1.f};
    auto pipeline = Check(instance.GetDevice().createGraphicsPipelineUnique(VK_NULL_HANDLE, {
        .pNext = &rendering,
        .stageCount = u32(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *layout,
    }));
    const auto result = *pipeline;
    for (auto& slot : pipelines) {
        if (!slot.second) {
            slot = {format, std::move(pipeline)};
            return result;
        }
    }
    pipelines[0] = {format, std::move(pipeline)}; // more than four formats: unexpected
    return result;
}

bool ParticlesHalf::Redirect(RenderState& state, std::array<float, 2>& target_scale,
                             vk::Format format) {
    const auto& color = state.color_attachments[0];
    if (state.num_color_attachments != 1 || state.num_layers != 1 ||
        state.depth_stencil_attachment.has_depth || state.depth_stencil_attachment.has_stencil ||
        color.is_clear || format != vk::Format::eR16G16B16A16Sfloat ||
        target_scale[0] != 1.0f || target_scale[1] != 1.0f || state.width < 2 ||
        state.height < 2) {
        if (skipped++ == 0) {
            std::printf("Particles: a candidate draw stays at full size (layers %u, depth %d, "
                        "clear %d, scale %.2f, %ux%u)\n",
                        state.num_layers, int(state.depth_stencil_attachment.has_depth),
                        int(color.is_clear), target_scale[0], state.width, state.height);
        }
        return false;
    }
    if (active && (target_view != color.image_view || target_width != state.width ||
                   target_height != state.height || target_layout != color.image_layout)) {
        Close(); // composites the open run
    }
    if (!active) {
        scheduler.EndRendering();
        const u32 w = (state.width + 1) / 2, h = (state.height + 1) / 2;
        current = nullptr;
        for (size_t i = 0; i < half_count; ++i) {
            if (halves[i].width == w && halves[i].height == h) {
                current = &halves[i];
            }
        }
        if (!current) {
            if (half_count == halves.size()) {
                scheduler.Finish(); // many sizes: start over (the images may be in use)
                for (auto& half : halves) {
                    half.view.reset();
                    half.image = VideoCore::UniqueImage{};
                }
                half_count = 0;
            }
            Half& half = halves[half_count++];
            half.image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
            half.image.Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = vk::Format::eR16G16B16A16Sfloat,
                .extent = {w, h, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eColorAttachment |
                         vk::ImageUsageFlagBits::eSampled |
                         vk::ImageUsageFlagBits::eTransferDst,
                .initialLayout = vk::ImageLayout::eUndefined,
            });
            half.view = Check(instance.GetDevice().createImageViewUnique({
                .image = vk::Image(half.image),
                .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eR16G16B16A16Sfloat,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
            half.width = w;
            half.height = h;
            std::printf("Particles: half-resolution image %ux%u (%zu sizes)\n", w, h, half_count);
            current = &half;
        }
        StartRun();
        ++runs;
        // Every 10 s: runs (each one a clear and a full-screen composite) and draws.
        static auto window = std::chrono::steady_clock::now();
        static u64 window_runs = 0, window_draws = 0;
        const auto now = std::chrono::steady_clock::now();
        if (now - window >= std::chrono::seconds(10)) {
            std::printf("Particles: %llu half-resolution runs, %llu draws in the last 10 s (%llu at full size so far)\n",
                        (unsigned long long)(runs - window_runs),
                        (unsigned long long)(draws - window_draws), (unsigned long long)skipped);
            window = now;
            window_runs = runs;
            window_draws = draws;
        }
        target_view = color.image_view;
        target_layout = color.image_layout;
        target_format = format;
        target_width = state.width;
        target_height = state.height;
    }
    ++draws;
    auto& out = state.color_attachments[0];
    out.image_view = *current->view;
    out.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    out.clear_value = {};
    out.is_clear = 0;
    state.width = u16(current->width);
    state.height = u16(current->height);
    target_scale = {0.5f, 0.5f};
    return true;
}

void ParticlesHalf::StartRun() {
    const vk::Image img{current->image};
    scheduler.Record([img](vk::CommandBuffer cmd) {
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const vk::ImageMemoryBarrier2 to_clear{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = img,
            .subresourceRange = range,
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_clear});
        // No colour yet, all of the scene shows through.
        cmd.clearColorImage(img, vk::ImageLayout::eTransferDstOptimal,
                            vk::ClearColorValue{std::array<float, 4>{0.f, 0.f, 0.f, 1.f}},
                            range);
        const vk::ImageMemoryBarrier2 to_draw{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                             vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = img,
            .subresourceRange = range,
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_draw});
    });
    active = true;
}

void ParticlesHalf::EnsureRun() {
    if (!active) {
        scheduler.EndRendering();
        StartRun();
    }
}

void ParticlesHalf::Close() {
    if (!active) {
        return;
    }
    const auto& open = scheduler.GetRenderState();
    if (scheduler.IsRendering() && open.num_color_attachments == 1 &&
        open.color_attachments[0].image_view == *current->view) {
        scheduler.EndRendering(); // composites
    } else {
        active = false; // cleared, nothing drawn into it
    }
}

void ParticlesHalf::OnEndRendering(const RenderState& ended) {
    if (!active || ended.num_color_attachments != 1 ||
        ended.color_attachments[0].image_view != *current->view) {
        return;
    }
    active = false;
    const vk::Image img{current->image};
    const auto source = *current->view;
    const auto target = target_view;
    const auto target_lay = target_layout;
    const vk::Extent2D size{target_width, target_height};
    const auto pipeline = CompositePipeline(target_format);
    const auto pl = *layout;
    const auto smp = *sampler;
    scheduler.Record([=](vk::CommandBuffer cmd) {
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const vk::MemoryBarrier2 scene{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                             vk::AccessFlagBits2::eColorAttachmentWrite,
        };
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = img,
            .subresourceRange = range,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1,
                              .pMemoryBarriers = &scene,
                              .imageMemoryBarrierCount = 1,
                              .pImageMemoryBarriers = &to_read});
        const vk::RenderingAttachmentInfo attachment{
            .imageView = target,
            .imageLayout = target_lay,
            .loadOp = vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        cmd.beginRendering({
            .renderArea = {{0, 0}, size},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &attachment,
        });
        const vk::DescriptorImageInfo info{smp, source, vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::WriteDescriptorSet write{
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &info,
        };
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, pl, 0, write);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmd.setViewportWithCount(
            vk::Viewport{0.f, 0.f, float(size.width), float(size.height), 0.f, 1.f});
        cmd.setScissorWithCount(vk::Rect2D{{0, 0}, size});
        cmd.draw(3, 1, 0, 0);
        cmd.endRendering();
        ForgetPushedDescriptors();
    });
    // The guest pipelines' dynamic state must be emitted again after this pipeline.
    scheduler.GetDynamicState().Invalidate();
}

} // namespace Vulkan
