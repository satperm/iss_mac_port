// SPDX-FileCopyrightText: Copyright 2026 satperm
// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_metalfx.h"

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace TlgMetalFx {
namespace {

id<MTLFXTemporalScaler> scaler;
MTLPixelFormat scaler_formats[4];
uint32_t scaler_size[4];
bool scaler_hdr;

// TLG_METALFX_TIMING=1: the scaler's GPU time (command buffer start to end), averaged over 300 frames.
std::atomic<uint64_t> timed_ns{0}, timed_frames{0};

void LogOnce(bool& done, const char* message) {
    if (!done) {
        done = true;
        std::printf("MetalFX: %s\n", message);
    }
}

PFN_vkExportMetalObjectsEXT Export(const Frame& f) {
    static PFN_vkExportMetalObjectsEXT export_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
        f.get_device_proc_addr(f.device, "vkExportMetalObjectsEXT"));
    return export_objects;
}

id<MTLTexture> TextureOf(const Frame& f, VkImage image) {
    VkExportMetalTextureInfoEXT texture{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
        .image = image,
        .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
    };
    VkExportMetalObjectsInfoEXT info{.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
                                     .pNext = &texture};
    Export(f)(f.device, &info);
    return texture.mtlTexture;
}

id<MTLCommandQueue> QueueOf(const Frame& f) {
    VkExportMetalCommandQueueInfoEXT queue{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT,
        .queue = f.queue,
    };
    VkExportMetalObjectsInfoEXT info{.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
                                     .pNext = &queue};
    Export(f)(f.device, &info);
    return queue.mtlCommandQueue;
}

} // namespace

bool Supported() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return device && [MTLFXTemporalScalerDescriptor supportsDevice:device];
}

void Release() {
    scaler = nil;
}

bool Encode(const Frame& f) {
    static bool no_export, no_textures, no_scaler, usage_warned, encoded_once;
    if (!Export(f)) {
        LogOnce(no_export, "vkExportMetalObjectsEXT unavailable (VK_EXT_metal_objects)");
        return false;
    }
    @autoreleasepool {
        id<MTLTexture> color = TextureOf(f, f.color);
        id<MTLTexture> depth = TextureOf(f, f.depth);
        id<MTLTexture> motion = TextureOf(f, f.motion);
        id<MTLTexture> output = TextureOf(f, f.output);
        id<MTLCommandQueue> queue = QueueOf(f);
        if (!color || !depth || !motion || !output || !queue) {
            LogOnce(no_textures, "could not get the Metal textures or queue behind the Vulkan objects");
            return false;
        }
        const MTLPixelFormat formats[4] = {color.pixelFormat, depth.pixelFormat, motion.pixelFormat,
                                           output.pixelFormat};
        const uint32_t size[4] = {f.width, f.height, f.out_width, f.out_height};
        bool same = scaler != nil && scaler_hdr == f.hdr;
        for (int i = 0; same && i < 4; ++i) {
            same = formats[i] == scaler_formats[i] && size[i] == scaler_size[i];
        }
        if (!same) {
            scaler = nil;
            MTLFXTemporalScalerDescriptor* d = [[MTLFXTemporalScalerDescriptor alloc] init];
            d.colorTextureFormat = formats[0];
            d.depthTextureFormat = formats[1];
            d.motionTextureFormat = formats[2];
            d.outputTextureFormat = formats[3];
            d.inputWidth = f.width;
            d.inputHeight = f.height;
            d.outputWidth = f.out_width;
            d.outputHeight = f.out_height;
            d.autoExposureEnabled = f.hdr;
            scaler = [d newTemporalScalerWithDevice:queue.device];
            if (!scaler) {
                std::printf("MetalFX: scaler creation failed (color %lu depth %lu motion %lu output "
                            "%lu, %ux%u -> %ux%u)\n",
                            (unsigned long)formats[0], (unsigned long)formats[1],
                            (unsigned long)formats[2], (unsigned long)formats[3], f.width, f.height,
                            f.out_width, f.out_height);
                LogOnce(no_scaler, "temporal scaler unavailable for these inputs");
                return false;
            }
            scaler_hdr = f.hdr;
            for (int i = 0; i < 4; ++i) {
                scaler_formats[i] = formats[i];
                scaler_size[i] = size[i];
            }
            std::printf("MetalFX: temporal scaler %ux%u -> %ux%u (color %lu depth %lu motion %lu output %lu)\n",
                        f.width, f.height, f.out_width, f.out_height, (unsigned long)formats[0],
                        (unsigned long)formats[1], (unsigned long)formats[2], (unsigned long)formats[3]);
        }
        if (!usage_warned && ((color.usage & scaler.colorTextureUsage) != scaler.colorTextureUsage ||
                              (output.usage & scaler.outputTextureUsage) != scaler.outputTextureUsage ||
                              (depth.usage & scaler.depthTextureUsage) != scaler.depthTextureUsage ||
                              (motion.usage & scaler.motionTextureUsage) != scaler.motionTextureUsage)) {
            usage_warned = true;
            std::printf("MetalFX: texture usage short of the scaler's (color %lu/%lu, depth %lu/%lu, "
                        "motion %lu/%lu, output %lu/%lu)\n",
                        (unsigned long)color.usage, (unsigned long)scaler.colorTextureUsage,
                        (unsigned long)depth.usage, (unsigned long)scaler.depthTextureUsage,
                        (unsigned long)motion.usage, (unsigned long)scaler.motionTextureUsage,
                        (unsigned long)output.usage, (unsigned long)scaler.outputTextureUsage);
        }
        scaler.colorTexture = color;
        scaler.depthTexture = depth;
        scaler.motionTexture = motion;
        scaler.outputTexture = output;
        scaler.inputContentWidth = f.width;
        scaler.inputContentHeight = f.height;
        scaler.jitterOffsetX = f.jitter_x;
        scaler.jitterOffsetY = f.jitter_y;
        scaler.motionVectorScaleX = f.motion_scale_x;
        scaler.motionVectorScaleY = f.motion_scale_y;
        scaler.reset = f.reset;
        scaler.depthReversed = f.depth_reversed;
        id<MTLCommandBuffer> cmdbuf = [queue commandBuffer];
        cmdbuf.label = @"tlg MetalFX";
        [scaler encodeToCommandBuffer:cmdbuf];
        static const bool timing = [] {
            const char* env = std::getenv("TLG_METALFX_TIMING");
            return env && *env == '1';
        }();
        if (timing) {
            [cmdbuf addCompletedHandler:^(id<MTLCommandBuffer> done) {
                const double s = done.GPUEndTime - done.GPUStartTime;
                if (s > 0 && s < 0.1) {
                    timed_ns += uint64_t(s * 1e9);
                    if (++timed_frames == 300) {
                        std::printf("MetalFX: GPU %.3f ms a frame (300 frames)\n", double(timed_ns.load()) / 300e6);
                        timed_ns = 0;
                        timed_frames = 0;
                    }
                }
            }];
        }
        [cmdbuf commit];
        if (!encoded_once) {
            encoded_once = true;
            std::printf("MetalFX: first frame encoded\n");
        }
    }
    return true;
}

} // namespace TlgMetalFx
