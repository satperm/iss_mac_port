// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/enum.h"
#include "common/incremental_id.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"

#include <array>
#include <deque>
#include <optional>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

namespace Vulkan {
class Instance;
class Runtime;
} // namespace Vulkan

VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaAllocator)

namespace VideoCore {

enum ImageFlagBits : u32 {
    Empty = 0,
    MaybeCpuDirty = 1 << 0, ///< The page this image is in was touched before the image address
    CpuDirty = 1 << 1,      ///< Contents have been modified from the CPU
    GpuDirty = 1 << 2, ///< Contents have been modified from the GPU (valid data in buffer cache)
    Dirty = MaybeCpuDirty | CpuDirty | GpuDirty,
    GpuModified = 1 << 3, ///< Contents have been modified from the GPU
    Registered = 1 << 6,  ///< True when the image is registered
    Picked = 1 << 7,      ///< Temporary flag to mark the image as picked
    Aliased = 1 << 8,     ///< tlg: another image has the same guest address (alias_states)
};
DECLARE_ENUM_FLAG_OPERATORS(ImageFlagBits)

/// tlg: TLG_LAZY_TEXTURE_USAGE=1 (default off): lazy texture usages (Image::full_usage_flags).
bool LazyTextureUsage();

struct UniqueImage {
    explicit UniqueImage() = default;
    explicit UniqueImage(vk::Device device, VmaAllocator allocator)
        : device{device}, allocator{allocator} {}
    ~UniqueImage();

    UniqueImage(const UniqueImage&) = delete;
    UniqueImage& operator=(const UniqueImage&) = delete;

    UniqueImage(UniqueImage&& other)
        : allocator{std::exchange(other.allocator, VK_NULL_HANDLE)},
          allocation{std::exchange(other.allocation, VK_NULL_HANDLE)},
          image{std::exchange(other.image, VK_NULL_HANDLE)}, image_ci{std::move(other.image_ci)} {}
    UniqueImage& operator=(UniqueImage&& other) {
        if (this == &other) return *this;
        Destroy();
        device = other.device;
        image = std::exchange(other.image, VK_NULL_HANDLE);
        allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        image_ci = std::move(other.image_ci);
        return *this;
    }

    void Create(const vk::ImageCreateInfo& image_ci);

    void Destroy();

    operator vk::Image() const {
        return image;
    }

    operator bool() const {
        return image;
    }

public:
    vk::Device device{};
    VmaAllocator allocator{};
    VmaAllocation allocation{};
    vk::Image image{};
    vk::ImageCreateInfo image_ci{};
    vk::DeviceSize size_bytes{};
};

struct Image {
    u64 perf_generation = 0;
    explicit Image(const Vulkan::Instance& instance, Vulkan::Runtime& runtime,
                   Common::SlotVector<ImageView>& slot_image_views, const ImageInfo& info);
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    Image(Image&&) = default;
    Image& operator=(Image&&) = default;

    bool Overlaps(VAddr overlap_cpu_addr, size_t overlap_size) const noexcept {
        const VAddr overlap_end = overlap_cpu_addr + overlap_size;
        const auto image_addr = info.guest_address;
        const auto image_end = info.guest_address + info.guest_size;
        return image_addr < overlap_end && overlap_cpu_addr < image_end;
    }

    vk::Image GetImage() const {
        return backing->image.image;
    }

    vk::DeviceSize GetHostImageSize() const {
        return backing->image.size_bytes;
    }

    bool IsTracked() {
        return track_addr != 0 && track_addr_end != 0;
    }

    bool SafeToDownload() const {
        return True(flags & ImageFlagBits::GpuModified) && False(flags & (ImageFlagBits::Dirty));
    }

    void AssociateDepth(ImageId depth_image_id, u64 depth_image_uid) {
        depth_id = depth_image_id;
        depth_uid = depth_image_uid;
    }

    void DisassociateDepth() {
        depth_id = {};
        depth_uid = {};
    }

    ImageView& FindView(const ImageViewInfo& view_info, bool ensure_guest_samples = true);

    /// tlg: TLG_LAZY_TEXTURE_USAGE: Metal compresses private textures losslessly only without
    /// MTLTextureUsageShaderWrite and MTLTextureUsagePixelFormatView, which MoltenVK sets for
    /// storage usage and for a mutable format without a list of sRGB twins. Single-sample colour
    /// images (not block-coded) are created without both (a "lazy" backing) and get their full
    /// usage once, when a binding needs storage or a view in another format
    /// (Runtime::UpgradeUsage: new backing, contents copied, the old one released later).
    /// True when the current backing is lazy.
    [[nodiscard]] bool IsLazy() const noexcept;
    /// True when a view `view_info` of the current backing needs the full usage.
    static bool ForceUpgrade();
    [[nodiscard]] bool NeedsFullUsage(const ImageViewInfo& view_info) const;
    /// True when the current backing can be viewed in `format` (a lazy backing: only the formats
    /// of its view format list).
    [[nodiscard]] bool CanViewAs(vk::Format format) const;
    /// A new image (no views, no contents yet) whose creating binding or predecessor needs the
    /// full usage: its backing is made again with it (no copy, no upgrade later).
    void MakeFullUsage();
    /// The create info of a backing with the full usage (no format list), from a backing's.
    [[nodiscard]] vk::ImageCreateInfo FullCreateInfo(const vk::ImageCreateInfo& from) const;

    using Barriers = boost::container::small_vector<vk::ImageMemoryBarrier2, 32>;
    void GetBarriers(Barriers& out_barriers, vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                     vk::PipelineStageFlags2 dst_stage,
                     std::optional<SubresourceRange> subres_range = {});

public:
    struct BackingImage;
    // tlg: fields every binding of the image reads, together ahead of the large ImageInfo
    // (they were spread over several cache lines: one miss each per texture per draw).
    ImageFlagBits flags = ImageFlagBits::Dirty;
    struct {
        u32 is_bound : 1;
        u32 is_target : 1;
        u32 needs_rebind : 1;
        u32 force_general : 1;
        u32 fb_fetch : 1; ///< tlg: a fragment shader reads it through framebuffer fetch
        u32 fb_plain : 1; ///< tlg: also bound as an ordinary texture by this draw
    } binding{};
    struct {
        u32 texture : 1;
        u32 storage : 1;
        u32 render_target : 1;
        u32 depth_target : 1;
        u32 vo_surface : 1;
    } usage{};
    bool scene_proxy = false; ///< tlg: SceneTargets holds a reduced-size proxy of it
    SubresourceRange sampled_range{}; ///< tlg: the range the current draw samples (feedback stats)
    u64 download_frame = ~0ULL; ///< tlg: the frame (flip count) of its last deferred download
    VAddr track_addr = 0;
    VAddr track_addr_end = 0;
    VAddr guest_begin = 0; ///< info.guest_address
    VAddr guest_end = 0;   ///< info.guest_address + info.guest_size
    BackingImage* backing{};
    /// tlg: gc tick of the last LRU touch; skips the LRU list (a cache miss) when current.
    mutable u64 lru_touched_tick = ~0ULL;
    u64 tick_accessed_last{};
    ImageId depth_id{};
    u64 depth_uid{};
    u64 image_uid{};
    /// tlg: order of the last GPU write, to sync images sharing memory (texture_cache.h).
    u64 alias_generation{};

    ImageInfo info;
    Vulkan::Runtime* runtime;
    Common::SlotVector<ImageView>* slot_image_views;
    vk::ImageAspectFlags aspect_mask = vk::ImageAspectFlagBits::eColor;
    vk::SampleCountFlags supported_samples = vk::SampleCountFlagBits::e1;

    vk::ImageUsageFlags usage_flags;
    /// tlg: TLG_LAZY_TEXTURE_USAGE: the usage without it (usage_flags is the current backing's).
    vk::ImageUsageFlags full_usage_flags;
    /// tlg: the view formats of a lazy backing (its VkImageFormatListCreateInfo), eUndefined after.
    std::array<vk::Format, 4> lazy_view_formats{};
    /// tlg: lazy_view_formats entries a view keeps (GetSupportedFormat maps them into the list).
    u8 lazy_direct_ok = 0;
    vk::FormatFeatureFlags2 format_features;
    struct State {
        vk::PipelineStageFlags2 pl_stage = vk::PipelineStageFlagBits2::eAllCommands;
        vk::AccessFlags2 access_mask = vk::AccessFlagBits2::eNone;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };
    struct BackingImage {
        UniqueImage image;
        State state;
        std::vector<State> subresource_states;
        /// tlg: the last partial transition (range and target state). The same request again,
        /// with nothing transitioned in between, needs no barriers: skips the walk over every
        /// level and layer in range (ISS binds partially transitioned arrays thousands of
        /// times per frame).
        struct PartialMemo {
            bool valid = false;
            SubresourceRange range{};
            vk::ImageLayout layout{};
            vk::AccessFlags2 access{};
            vk::PipelineStageFlags2 stage{};
        } partial_memo;
        boost::container::small_vector<ImageViewInfo, 4> image_view_infos;
        boost::container::small_vector<ImageViewId, 4> image_view_ids;
        u32 last_view = 0; ///< tlg: index of the view FindView returned last
        u32 num_samples;
        bool lazy = false; ///< tlg: TLG_LAZY_TEXTURE_USAGE: made without its full usage
    };
    std::deque<BackingImage> backing_images;
    boost::container::static_vector<u64, 16> mip_hashes{};
    u64 lru_id{};
    u64 hash{};
    u64 stale_hash = 0; ///< tlg: TLG_STALE_CHECK: guest memory hash at the last upload
    std::vector<u64> stale_chunks; ///< tlg: the same per 64 KiB, to locate a change
    std::vector<u8> stale_copy; ///< tlg: guest bytes at the upload (images under 16 MiB)

private:
    static Common::IncrementalIdProvider<u64> global_image_uid;
};

} // namespace VideoCore
