// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <array>
#include <span>
#include <thread>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "common/logging/log.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "common/path_util.h"
#include "video_core/renderer_vulkan/vk_wave_stats.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

bool RetryPipelineCreation(vk::Result result, u32 attempt, std::string_view kind,
                           std::string_view name) {
    // With the driver's cache cold, as after a driver update, the compiler threads build hundreds
    // of pipelines at once while the game loads, and the driver failed one of them now and then
    // with an unknown error, which ended the game. Building it again a little later can succeed.
    constexpr u32 MaxAttempts = 5;
    const bool may_recover = result == vk::Result::eErrorUnknown ||
                             result == vk::Result::eErrorOutOfHostMemory ||
                             result == vk::Result::eErrorOutOfDeviceMemory ||
                             result == vk::Result::eErrorInitializationFailed;
    if (!may_recover || attempt >= MaxAttempts) {
        return false;
    }
    LOG_WARNING(Render_Vulkan, "tlg: creating {} pipeline {} failed with {} (attempt {}), trying "
                "again", kind, name, vk::to_string(result), attempt);
    std::this_thread::sleep_for(std::chrono::milliseconds{100} * attempt);
    return true;
}

vk::Pipeline Pipeline::BuiltHandle() const {
    if (const VkPipeline handle = build->handle.load(std::memory_order_acquire)) {
        return vk::Pipeline{handle};
    }
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lock{build->mutex};
    build->cv.wait(lock, [this] {
        return build->handle.load(std::memory_order_acquire) != VK_NULL_HANDLE;
    });
    LOG_INFO(Render_Vulkan, "tlg: waited {:.1f} ms for compute pipeline {} being built",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                 .count(),
             GetDebugString());
    return vk::Pipeline{build->handle.load(std::memory_order_acquire)};
}


namespace {
// tlg (macOS): MoltenVK copies every write of a push (MVKCmdPushDescriptorSet::setContent /
// clearDescriptorWrites, about half of the Vulkan recording thread). Consecutive draws of a
// pipeline repeat ~39% of their bindings (TLG_PUSH_STATS), so only the bindings whose contents
// changed since the previous push on the same command buffer and layout are pushed; the others
// keep their pushed values (push descriptor state persists until an incompatible layout).
// TLG_MAC_PARTIAL_PUSH=0 pushes every binding.
bool PartialPushEnabled() {
#ifdef __APPLE__
    static const bool enabled = [] {
        const char* env = std::getenv("TLG_MAC_PARTIAL_PUSH");
        return !(env && *env == '0');
    }();
    return enabled;
#else
    return false;
#endif
}

u64 HashWrite(const vk::WriteDescriptorSet& w) {
    u64 h = 1469598103934665603ull;
    const auto mix = [&h](const void* p, size_t n) {
        const auto* b = static_cast<const u8*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(&w.descriptorType, sizeof w.descriptorType);
    mix(&w.descriptorCount, sizeof w.descriptorCount);
    if (w.pBufferInfo) mix(w.pBufferInfo, w.descriptorCount * sizeof(*w.pBufferInfo));
    if (w.pImageInfo) mix(w.pImageInfo, w.descriptorCount * sizeof(*w.pImageInfo));
    if (w.pTexelBufferView) mix(w.pTexelBufferView, w.descriptorCount * sizeof(*w.pTexelBufferView));
    return h | 1; // never 0 (0 = not pushed yet)
}

/// Pushes the writes whose bindings changed since the previous push on this command buffer and
/// layout (runs on the Vulkan recording thread).
void PushChanged(vk::CommandBuffer cmdbuf, vk::PipelineBindPoint bind_point, vk::PipelineLayout layout,
                 std::span<const vk::WriteDescriptorSet> writes) {
    if (!PartialPushEnabled()) {
        cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
        return;
    }
    struct State {
        VkCommandBuffer cmdbuf{};
        VkPipelineLayout layout{};
        vk::PipelineBindPoint bind_point{};
        std::array<u64, 128> hashes{};
    };
    static thread_local State state;
    if (state.cmdbuf != VkCommandBuffer(cmdbuf) || state.layout != VkPipelineLayout(layout) ||
        state.bind_point != bind_point) {
        state.cmdbuf = cmdbuf;
        state.layout = layout;
        state.bind_point = bind_point;
        state.hashes.fill(0);
    }
    boost::container::small_vector<vk::WriteDescriptorSet, 32> changed;
    for (const auto& w : writes) {
        const u32 slot = w.dstBinding * 4 + std::min<u32>(w.dstArrayElement, 3);
        if (slot >= state.hashes.size() || w.dstArrayElement > 3) {
            changed.push_back(w); // outside the table: always pushed
            continue;
        }
        const u64 h = HashWrite(w);
        if (state.hashes[slot] != h) {
            state.hashes[slot] = h;
            changed.push_back(w);
        }
    }
    if (!changed.empty()) {
        cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, std::span<const vk::WriteDescriptorSet>{changed.data(), changed.size()});
    }
}
} // namespace

void Pipeline::BindResources(DescriptorWrites& set_writes, u32 count,
                             const Shader::PushData& push_data) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (count == 0) {
        return;
    }
    const std::span<const vk::WriteDescriptorSet> writes{set_writes.data(), count};

    // tlg: TLG_PUSH_STATS=1: how many descriptor pushes repeat the previous one (same layout,
    // same resources), every 5 s (input for skipping them).
    static const bool push_stats = [] {
        const char* env = std::getenv("TLG_PUSH_STATS");
        return env && *env == '1';
    }();
    if (push_stats && uses_push_descriptors) {
        static VkPipelineLayout last_layout{};
        static u64 last_hash{};
        static u64 pushes, repeats, same_layout;
        static auto window = std::chrono::steady_clock::now();
        u64 h = 1469598103934665603ull;
        const auto mix = [&h](const void* p, size_t n) {
            const auto* b = static_cast<const u8*>(p);
            for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
        };
        for (const auto& w : writes) {
            mix(&w.dstBinding, sizeof w.dstBinding);
            mix(&w.descriptorType, sizeof w.descriptorType);
            if (w.pBufferInfo) mix(w.pBufferInfo, w.descriptorCount * sizeof(*w.pBufferInfo));
            if (w.pImageInfo) mix(w.pImageInfo, w.descriptorCount * sizeof(*w.pImageInfo));
            if (w.pTexelBufferView) mix(w.pTexelBufferView, w.descriptorCount * sizeof(*w.pTexelBufferView));
        }
        ++pushes;
        static std::array<u64, 64> binding_hash{};
        static u64 writes_total, writes_changed;
        const bool same = VkPipelineLayout(layout) == last_layout;
        for (const auto& w : writes) {
            u64 wh = 1469598103934665603ull;
            const auto wmix = [&wh](const void* p, size_t n) {
                const auto* b = static_cast<const u8*>(p);
                for (size_t i = 0; i < n; ++i) wh = (wh ^ b[i]) * 1099511628211ull;
            };
            wmix(&w.descriptorType, sizeof w.descriptorType);
            if (w.pBufferInfo) wmix(w.pBufferInfo, w.descriptorCount * sizeof(*w.pBufferInfo));
            if (w.pImageInfo) wmix(w.pImageInfo, w.descriptorCount * sizeof(*w.pImageInfo));
            if (w.pTexelBufferView) wmix(w.pTexelBufferView, w.descriptorCount * sizeof(*w.pTexelBufferView));
            const u32 slot = w.dstBinding % 64;
            ++writes_total;
            if (!same || binding_hash[slot] != wh) ++writes_changed;
            binding_hash[slot] = wh;
        }
        if (same) {
            ++same_layout;
            repeats += h == last_hash;
        }
        last_layout = layout;
        last_hash = h;
        if (std::chrono::steady_clock::now() - window >= std::chrono::seconds(5)) {
            window = std::chrono::steady_clock::now();
            std::printf("Push stats (5 s): %llu pushes, %llu with the previous layout, %llu identical; "
                        "%llu of %llu writes changed\n",
                        (unsigned long long)pushes, (unsigned long long)same_layout,
                        (unsigned long long)repeats, (unsigned long long)writes_changed,
                        (unsigned long long)writes_total);
            pushes = repeats = same_layout = writes_changed = writes_total = 0;
        }
    }

    if (uses_push_descriptors) {
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
                PushChanged(cmdbuf, bind_point, layout, writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        size_t bytes = writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto recorded = scheduler.RecordData(writes);
        auto* patched = const_cast<vk::WriteDescriptorSet*>(recorded.data());
        for (size_t i = 0; i < recorded.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
        scheduler.Record([bind_point, layout, recorded](vk::CommandBuffer cmdbuf) {
            PushChanged(cmdbuf, bind_point, layout, recorded);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread. The pool may hold more
    // writes than this draw's, so drop them before the call (no element is constructed: the
    // vector only grows, see EnsureSetWrites).
    const auto desc_set = desc_heap.Commit(*desc_layout);
    set_writes.resize(count);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

// tlg: one histogram for every pipeline of the process, reported to last_run.log on news and
// every 256 pipelines (vk_wave_stats.h). AMD reports width 0 for graphics executables, so on
// AMD the key also carries the physical VGPR count from VK_AMD_shader_info (it differs between
// wave32 and wave64), and the ISA of the first executables of each stage goes to
// <shaders>/wave_dumps/ (wave32 code manipulates EXEC with the _b32 forms).
void Pipeline::RecordWaveSizes() const {
    if (!instance.IsPipelineExecutableInfoEnabled() || !pipeline) {
        return;
    }
    const auto [result, executables] = instance.GetDevice().getPipelineExecutablePropertiesKHR(
        vk::PipelineInfoKHR{.pipeline = *pipeline});
    if (result != vk::Result::eSuccess) {
        return;
    }
    static std::mutex mutex;
    static WaveSizeHistogram histogram;
    std::scoped_lock lock{mutex};
    bool news = false;
    for (const vk::PipelineExecutablePropertiesKHR& executable : executables) {
        const std::string stages = vk::to_string(executable.stages);
        std::string key = stages;
        // One stage bit per query: the last stage of the executable (vk_wave_stats.h).
        const std::uint32_t stage_bit = AmdShaderInfoStageBit(
            is_compute ? static_cast<std::uint32_t>(vk::ShaderStageFlagBits::eCompute)
                       : static_cast<std::uint32_t>(executable.stages));
        if (instance.IsAmdShaderInfoEnabled() && stage_bit != 0) {
            const auto stage = static_cast<vk::ShaderStageFlagBits>(stage_bit);
            const auto [stats_result, stats] = instance.GetDevice().getShaderInfoAMD(
                *pipeline, stage, vk::ShaderInfoTypeAMD::eStatistics);
            std::string stats_line = "stats unavailable";
            std::optional<std::uint32_t> vgprs;
            if (stats_result == vk::Result::eSuccess &&
                stats.size() >= sizeof(vk::ShaderStatisticsInfoAMD)) {
                vk::ShaderStatisticsInfoAMD info{};
                std::memcpy(&info, stats.data(), sizeof(info));
                vgprs = info.numPhysicalVgprs;
                key += "/vgpr" + std::to_string(info.numPhysicalVgprs);
                stats_line = "physical vgprs " + std::to_string(info.numPhysicalVgprs) +
                             ", available " + std::to_string(info.numAvailableVgprs) +
                             ", used " + std::to_string(info.resourceUsage.numUsedVgprs) +
                             "; physical sgprs " + std::to_string(info.numPhysicalSgprs) +
                             ", used " + std::to_string(info.resourceUsage.numUsedSgprs) +
                             "; lds " + std::to_string(info.resourceUsage.ldsUsageSizeInBytes) +
                             "; workgroup " + std::to_string(info.computeWorkGroupSize[0]) +
                             "x" + std::to_string(info.computeWorkGroupSize[1]) + "x" +
                             std::to_string(info.computeWorkGroupSize[2]);
            }
            if (histogram.ShouldDump(key, 3)) {
                const auto [isa_result, isa] = instance.GetDevice().getShaderInfoAMD(
                    *pipeline, stage, vk::ShaderInfoTypeAMD::eDisassembly);
                if (isa_result == vk::Result::eSuccess && !isa.empty()) {
                    const auto dir =
                        Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) / "wave_dumps";
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    const std::string name = WaveDumpFileName(
                        stages, vgprs, histogram.DumpsOf(key), executable.subgroupSize);
                    std::ofstream out(dir / name, std::ios::binary);
                    out << "key " << key << "\n" << stats_line << "\n";
                    out.write(reinterpret_cast<const char*>(isa.data()),
                              static_cast<std::streamsize>(isa.size()));
                }
            }
        }
        news |= histogram.Add(std::move(key), executable.subgroupSize);
    }
    if (histogram.EndPipeline(news)) {
        std::printf("GPU: wave sizes after %u pipelines: %s\n", histogram.Pipelines(),
                    histogram.Summary().c_str());
    }
}

} // namespace Vulkan
