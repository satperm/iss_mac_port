// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef _WIN32
#include <set>
#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <dlfcn.h>
#endif
#include <cmath>
#include <unordered_set>
#include <xxhash.h>
#include "tlg_split.h"
#include "video_core/renderer_vulkan/ui_composition.h"
#include "common/cpu_relax.h"
#include "tlg_toggles.h"
#include "tlg_settings.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_fb_fetch.h"
#include "video_core/renderer_vulkan/vk_lighting_test.h"
#include "video_core/renderer_vulkan/vk_pass_census.h"
#include "video_core/renderer_vulkan/vk_frame_record.h"
#include "common/backports.h"
#include "video_core/cache_storage.h"
#include "common/debug.h"
#include "common/hash.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/host_shaders/quad_indirect_comp.h"
#include "video_core/host_shaders/quad_index_expand_comp.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_occlusion.h"
#include "video_core/buffer_cache/unified_memory.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/host_shaders/acb_copy_comp.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "tlg_threads.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

void StartMark(const char* what);

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, page_manager{this},
      buffer_cache{instance, scheduler, runtime, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, runtime, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool, buffer_cache.GetSparsePageShift()},
      host_markers_enabled{EmulatorSettings.IsVkHostMarkersEnabled()},
      guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    StartMark("rasterizer constructor body");
    if (!instance.IsNullDescriptorSupported()) {
        // Shader writes through a null binding land here and are not discarded, so the contents
        // are only mostly zero (as bloodborne_mac_port).
        null_buffer.emplace(instance, 0, NullBufferSize, VideoCore::MemoryType::HostUncached, "NullBuffer");
        ASSERT_MSG(!null_buffer->mapped_data.empty(), "Null buffer is not host visible");
        std::memset(null_buffer->mapped_data.data(), 0, NullBufferSize);
        if (!null_buffer->is_coherent) {
            null_buffer->Flush(0, NullBufferSize);
        }
    }
    // Before the rasterizer is bound: Liverpool enqueues buffers only once it sees it.
    draw_prep = std::make_unique<DrawPreparation>(pipeline_cache);
    scene_targets = std::make_unique<SceneTargets>(instance, scheduler, runtime, texture_cache);
    // Object motion first: it fixes the buffer addresses the motion shader variants embed.
    object_motion = std::make_unique<ObjectMotion>(instance, scheduler);
    camera_motion = std::make_unique<CameraMotion>(instance, scheduler, texture_cache, runtime);
    camera_motion->SetObjectMotion(object_motion.get());
    upscaler = std::make_unique<TemporalUpscaler>(instance, scheduler, texture_cache, runtime,
                                                  *camera_motion, *scene_targets);
    if (Common::Backports::OcclusionMode() == 4) {
        if (instance.IsHostQueryResetSupported()) {
            occlusion = std::make_unique<OcclusionQueries>(instance, scheduler);
        }
        std::printf("Occlusion: real queries %s (precise %d)\n",
                    occlusion ? "on" : "off: no host query reset, zero counts as mode 3",
                    instance.IsOcclusionQueryPreciseSupported() ? 1 : 0);
    }
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);

    GpuProfiler::Init(instance, scheduler);
    FrameRecord::Attach(instance, scheduler);
    if (DrawPipeWanted()) {
        constant_ring = std::make_unique<ConstantRing>(instance, scheduler);
        draw_pipe = std::make_unique<DrawPipe>(&RunDrawPacket, this);
        scheduler.SetOwnerHook(&SchedulerOwnerHook, this);
    }
    // tlg: this thread joins the texture binding helper before it changes image state.
    runtime.SetImageAccessHook(&JoinBindHelper, this);
    if (draw_pipe) {
        draw_pipe->SetIdleHandler(&Rasterizer::AcbIdle, this);
    }
    if (instance.GetComputeQueue()) {
        acb_scheduler = std::make_unique<Scheduler>(instance, instance.GetComputeQueue(),
                                                    instance.GetComputeQueueFamilyIndex());
        scheduler.SetCompanion(acb_scheduler.get());
        acb_fence_thread = std::jthread(std::bind_front(&Rasterizer::AcbFenceThread, this));

        std::printf("GPU: the game's compute queue runs on a queue of its own (TLG_ACB_QUEUE=1)\n");
    }
    scheduler.SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
        runtime.FlushBarriers();
        buffer_cache.SubmitPendingArenaBinds(info);
    });
}

Rasterizer::~Rasterizer() {
    scheduler.SetCompanion(nullptr);
    if (acb_fence_thread.joinable()) {
        acb_fence_thread.request_stop();
        acb_fence_cv.notify_all();
        acb_fence_thread.join();
    }
}

inline const AmdGpu::Regs& Rasterizer::Regs() const {
    return stage_regs ? *stage_regs : liverpool->regs;
}

inline AmdGpu::CbDbExtent Rasterizer::CbExtent(u32 index) const {
    return stage_regs ? pipe_cb_extent[index] : liverpool->last_cb_extent[index];
}

inline const AmdGpu::ComputeProgram& Rasterizer::CsRegs() const {
    return stage_cs ? *stage_cs : liverpool->GetCsRegs();
}

inline AmdGpu::CbDbExtent Rasterizer::DbExtent() const {
    return stage_regs ? pipe_db_extent : liverpool->last_db_extent;
}

namespace {
/// magic_enum::enum_contains, from a table: it scans every enumerator (per texture per draw).
bool IsKnownFormat(AmdGpu::DataFormat data_fmt, AmdGpu::NumberFormat num_fmt) {
    static const auto tables = [] {
        std::pair<std::array<bool, 256>, std::array<bool, 256>> t{};
        for (const auto value : magic_enum::enum_values<AmdGpu::DataFormat>()) {
            if (static_cast<u32>(value) < 256) {
                t.first[static_cast<u32>(value)] = true;
            }
        }
        for (const auto value : magic_enum::enum_values<AmdGpu::NumberFormat>()) {
            if (static_cast<u32>(value) < 256) {
                t.second[static_cast<u32>(value)] = true;
            }
        }
        return t;
    }();
    const u32 d = static_cast<u32>(data_fmt), n = static_cast<u32>(num_fmt);
    return d < 256 && n < 256 && tables.first[d] && tables.second[n];
}
} // namespace

bool Rasterizer::FilterDraw() {
    const auto& regs = Regs();
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

template <typename... Parts>
VideoCore::ImageId Rasterizer::FindTargetMemoized(VideoCore::TextureCache::ImageDesc& desc,
                                                  LastTarget& last, auto&& make_desc,
                                                  const Parts&... parts) {
    static_assert((sizeof(Parts) + ...) <= 256, "target memo key too large");
    std::array<u8, 256> key{};
    u32 key_size = 0;
    const auto append = [&](const auto& part) {
        std::memcpy(key.data() + key_size, &part, sizeof(part));
        key_size += sizeof(part);
    };
    (append(parts), ...);
    const u64 generation = texture_cache.RegistryGeneration();
    const bool memo_enabled = !TlgToggle::Disabled(TlgToggle::TextureBindingMemo);
    if (memo_enabled && last.generation == generation && last.key_size == key_size &&
        std::memcmp(last.key.data(), key.data(), key_size) == 0) {
        texture_cache.MarkFound(last.image_id);
        return last.image_id;
    }
    const auto remember = [&](VideoCore::ImageId image_id) {
        last.key = key;
        last.key_size = key_size;
        last.generation = generation;
        last.image_id = image_id;
    };
    u64 hash = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < key_size; i += 8) {
        u64 word;
        std::memcpy(&word, key.data() + i, 8);
        hash = (hash ^ word) * 0x100000001B3ull;
        hash ^= hash >> 29;
    }
    auto& memo = target_memo[hash % target_memo.size()];
    if (memo.generation == generation && memo.key_size == key_size &&
        std::memcmp(memo.key.data(), key.data(), key_size) == 0 && memo_enabled) {
        desc = memo.desc;
        texture_cache.MarkFound(memo.image_id);
        remember(memo.image_id);
        return memo.image_id;
    }
    make_desc();
    const VideoCore::ImageId image_id = texture_cache.FindImage(desc);
    if (generation == texture_cache.RegistryGeneration()) {
        memo.key = key;
        memo.key_size = key_size;
        memo.generation = generation;
        memo.image_id = image_id;
        memo.desc = desc;
        remember(image_id);
    } else {
        last.generation = ~0ULL;
    }
    return image_id;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = Regs();
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = CbExtent(cb);
        const u32 tag = 1;
        image_id = bound_images.emplace_back(FindTargetMemoized(
            desc, last_targets[cb], [&] { std::construct_at(&desc, col_buf, hint); }, tag,
            col_buf, hint));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if (GetEffectiveDepthStencilState(regs).needs_attachment) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = DbExtent();
        auto& [image_id, desc] = db_desc;
        const u32 tag = 2;
        image_id = bound_images.emplace_back(FindTargetMemoized(
            desc, last_targets[AmdGpu::NUM_COLOR_BUFFERS],
            [&] {
                std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                                  htile_address, hint);
            },
            tag, regs.depth_buffer, regs.depth_view, regs.depth_control, htile_address, hint));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
    // tlg: the G-buffer pass (5+ color targets) holds the scene depth, and its constants the
    // main camera (shadow passes bind the same layout with the light's camera).
    gbuffer_draw = camera_motion->Enabled() && std::popcount(key.mrt_mask) >= 5 && db_desc.first;
    if (gbuffer_draw) {
        camera_motion->OnGBufferPass(db_desc.first);
    }
    if (upscaler->Enabled() && cb_descs[0].first) {
        upscaler->OnColorTarget(cb_descs[0].first);
    }
    // tlg: scene color: a full-size RGBA16F target drawn with the scene depth.
    if (upscaler->Enabled() && db_desc.first && db_desc.first == camera_motion->Depth() &&
        cb_descs[0].first && std::popcount(key.mrt_mask) <= 2) {
        const auto& color = texture_cache.GetImage(cb_descs[0].first);
        const auto& depth = texture_cache.GetImage(db_desc.first);
        if (color.info.pixel_format == vk::Format::eR16G16B16A16Sfloat &&
            color.info.size.width == depth.info.size.width &&
            color.info.size.height == depth.info.size.height) {
            upscaler->OnSceneColor(cb_descs[0].first);
            // Blended geometry without depth writes (not full-screen passes): transparents and
            // effects start. Blended layers writing depth (decals, wet/blood ground) are
            // surfaces and stay in the history.
            if (regs.blend_control[0].enable && !regs.color_buffers[0].info.blend_bypass &&
                !regs.depth_control.depth_write_enable &&
                (regs.num_indices > 6 || regs.num_instances.NumInstances() > 1)) {
                upscaler->OnBlendedSceneDraw();
            } else if (regs.num_indices <= 6 && regs.num_instances.NumInstances() <= 1) {
                // A full-screen pass over the scene (the fog composite): the transparents are in.
                upscaler->OnSceneComposite();
            }
        }
    }
}

static std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Regs& regs, const Shader::Info& info,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.UserData()[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.UserData()[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = Regs().color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, CbExtent(0));
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    runtime.ClearImage(&image, desc.view_info.range, clear_value);
    ScopeMarkerEnd();
}

// tlg: two-stage draw pipeline (vk_draw_pipe.h). Packet layout, 8-byte aligned parts:
// DrawPacket, u16 block indices, u32 block words (RegDirty::BlockWords each), then per stage a
// PacketStage with its user data and flattened user data, then (verification) the full file.
namespace {
enum class PacketKind : u32 { Draw, Task, Dispatch, Special, Indirect, IndirectDispatch };
struct TaskPacket {
    PacketKind kind;
    u32 size;
    Rasterizer::OrderedTask task;
};
struct DrawPacket {
    PacketKind kind;
    const Pipeline* pipeline;
    const PreparedDraw* prepared;
    u32 index_offset;
    u8 is_indexed;
    u8 regs_reset;
    u16 num_blocks;
    u32 num_stages;
    u32 verify;
    std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> cb_extent;
    AmdGpu::CbDbExtent db_extent;
    u64 ring_end; ///< constant ring position after this packet's copies
};
struct PacketStage {
    const Shader::Info* info;
    VAddr pgm_base;
    u32 ud_size;
    u32 flat_size;
    u32 num_ring;
    u32 pad;
};
constexpr u32 AlignPacket(u32 size) {
    return (size + 7) & ~7u;
}
u32 VerifyInterval() {
    static const u32 interval = [] {
        const char* env = std::getenv("TLG_PIPE_VERIFY");
        return env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 0u;
    }();
    return interval;
}
} // namespace

bool Rasterizer::DrawPipeWanted() {
    const char* env = std::getenv("TLG_DRAW_PIPE");
    if (env && env[0]) {
        return env[0] == '1';
    }
    // Stage B spins while draws flow. Measured ahead with 16 threads (+19%) and with 4 cores /
    // 8 threads (taskset, Steam Deck-like: +18%).
    return TlgThreads::Available() >= 8;
}

bool Rasterizer::UseDrawPipe() const {
    // tlg: off while a frame is recorded: everything then runs in stream order on this thread.
    return draw_pipe && !host_markers_enabled && !TlgToggle::Disabled(TlgToggle::DrawPipeline) &&
           !FrameRecord::Active();
}

bool Rasterizer::OnStageA() const {
#ifdef _WIN32
    // tlg (Windows): libc++ compares std::thread::id through winpthreads' pthread_equal,
    // which asks the kernel (GetThreadId) for both threads: two system calls per check, many
    // per draw. The numeric thread id comes from the TEB.
    return static_cast<u32>(gettid()) == liverpool->GetGpuCommandProcessorThreadId();
#else
    return std::this_thread::get_id() == liverpool->GetGpuCommandProcessorThread();
#endif
}

namespace {
/// Stage A: cycles waited per drain site (TLG_FRAME_STATS), keyed by function name and line.
std::unordered_map<const char*, std::unordered_map<u32, u64>> drain_sites;
} // namespace

void Rasterizer::SchedulerOwnerHook(void* context, void* caller) {
    auto* self = static_cast<Rasterizer*>(context);
    const bool stage_a = self->OnStageA();
    if (!self->draw_pipe || (stage_a && self->draw_pipe->Idle())) {
        return;
    }
    // Each caller is reported once: the site that skipped DrainDrawPipe.
    static std::mutex reported_mutex;
    static std::unordered_set<void*> reported;
    bool report;
    {
        std::scoped_lock lk{reported_mutex};
        report = reported.insert(caller).second;
    }
    if (report) {
        const char* module = "?";
        const char* name = "?";
        uintptr_t offset = reinterpret_cast<uintptr_t>(caller);
#ifndef _WIN32
        Dl_info info{};
        if (dladdr(caller, &info)) {
            module = info.dli_fname ? info.dli_fname : module;
            name = info.dli_sname ? info.dli_sname : name;
            offset -= reinterpret_cast<uintptr_t>(info.dli_fbase);
        }
#endif
        std::fprintf(stderr,
                     "tlg: scheduler used %s while draw packets are in flight, from %s+%#lx "
                     "(%s)%s\n",
                     stage_a ? "on the GPU command thread" : "by another thread", module,
                     static_cast<unsigned long>(offset), name, stage_a ? "; draining first" : "");
    }
    if (stage_a) {
        self->DrainDrawPipe(DrawPipe::ReasonRasterizer, __LINE__, __func__);
        return;
    }
    // tlg: TLG_OWNER_ABORT=1 stops here instead of only logging: a foreign thread (a guest
    // memory hook or the fault handler) reached the scheduler while packets were in flight,
    // which is where a crash in the record chunks comes from. The stack shows the site.
    static const bool abort_on_foreign = [] {
        const char* env = std::getenv("TLG_OWNER_ABORT");
        return env && *env == '1';
    }();
    if (abort_on_foreign) {
        std::fflush(stderr);
        std::abort();
    }
}

void Rasterizer::DrainDrawPipe(u32 reason, u32 line, const char* function) {
    if (!draw_pipe || !OnStageA()) {
        return;
    }
    static const bool stats = std::getenv("TLG_FRAME_STATS") != nullptr;
    if (!stats) {
        draw_pipe->Drain(reason);
        return;
    }
    const u64 before = draw_pipe->drain_cycles;
    draw_pipe->Drain(reason);
    if (const u64 waited = draw_pipe->drain_cycles - before) {
        drain_sites[function][line] += waited;
    }
}

bool Rasterizer::IsGpuSideThread() const {
    return OnStageA() || DrawPipe::OnStageB();
}

void Rasterizer::RunForeign(std::function<void()> work) {
    // tlg: TLG_FOREIGN_SERIALIZE=1: guest threads (host memory hooks) and the userfaultfd thread
    // reach cache state the draw recording thread owns (see UnmapMemory). The work then runs in
    // order with the draws handed over before it -- on the draw recording thread while the pipe
    // is in use, as a readback does -- instead of beside the packet being recorded. Off by
    // default: it adds a wait on the GPU command thread to a path that ran un-queued.
    static const bool serialize = [] {
        const char* env = std::getenv("TLG_FOREIGN_SERIALIZE");
        return env && *env == '1';
    }();
    if (!serialize || IsGpuSideThread()) {
        work();
        return;
    }
    liverpool->SendCommand<true, true>(std::move(work));
}

bool Rasterizer::IsGpuSideThreadId(u32 tid) const {
    // The userfaultfd thread handles the fault while the faulting thread waits: without locks
    // only if that thread owns the caches. The GPU command thread does not while the draw
    // recording thread may be running, so its faults take the locked path of guest threads.
    if (draw_pipe) {
        return tid == draw_pipe->StageBThreadId();
    }
    return tid == liverpool->GetGpuCommandProcessorThreadId();
}

void Rasterizer::NotePendingGpuWrite(VAddr address, u64 size) {
    if (!draw_pipe || !size) {
        return;
    }
    // Draws write the same buffers over and over: an entry for the range only moves on.
    const u64 position = draw_pipe->Head();
    const VAddr end = address + size;
    for (auto& write : pending_writes) {
        if (address <= write.end && write.begin <= end) {
            write.begin = std::min(write.begin, address);
            write.end = std::max(write.end, end);
            write.position = position;
            pending_min = std::min(pending_min, address);
            pending_max = std::max(pending_max, end);
            return;
        }
    }
    pending_writes.push_back({address, end, position});
    pending_min = std::min(pending_min, address);
    pending_max = std::max(pending_max, end);
}

bool Rasterizer::PendingWriteOverlaps(VAddr address, u64 size) {
    if (pending_writes.empty() || address >= pending_max || address + size <= pending_min) {
        return false;
    }
    // Now and then drop entries whose writes the recording thread has done: the bounds shrink.
    const u64 done = draw_pipe->Consumed();
    if ((++pending_checks & 63) == 0) {
        std::erase_if(pending_writes, [&](const PendingWrite& write) { return done >= write.position; });
        pending_min = ~VAddr{0};
        pending_max = 0;
        for (const auto& write : pending_writes) {
            pending_min = std::min(pending_min, write.begin);
            pending_max = std::max(pending_max, write.end);
        }
    }
    const VAddr end = address + size;
    for (const auto& write : pending_writes) {
        if (address < write.end && write.begin < end && done < write.position) {
            return true;
        }
    }
    return false;
}

void Rasterizer::CollectRingBindings(const Shader::Info& stage, const PreparedDraw* prepared,
                           boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>& out) {
    const PreparedStage* prepared_stage = nullptr;
    if (prepared && !TlgToggle::Disabled(TlgToggle::PreparedResources)) {
        for (u32 i = 0; i < prepared->num_stages; ++i) {
            const auto& candidate = prepared->stages[i];
            if (&candidate.program->info == &stage &&
                candidate.num_buffers == stage.buffers.size()) {
                prepared_stage = &candidate;
                break;
            }
        }
    }
    const u64 alignment = std::max<u64>(instance.StorageMinAlignment(),
                                        instance.UniformMinAlignment());
    const auto copy = [&](u32 index, const void* source, VAddr address, u64 size) {
        const auto offset = constant_ring->Allocate(size, alignment);
        if (!offset) {
            return;
        }
        u8* dst = constant_ring->Data(*offset);
        if (source) {
            std::memcpy(dst, source, size);
        } else {
            memory->CopySparseMemory(address, dst, size);
        }
        constant_ring->Flush(*offset, size);
        out.push_back({index, static_cast<u32>(size), *offset, address});
    };
    for (u32 index = 0; index < stage.buffers.size(); ++index) {
        const auto& desc = stage.buffers[index];
        if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::Flatbuf &&
                !stage.flattened_ud_buf.empty()) {
                copy(index, stage.flattened_ud_buf.data(), 0,
                     stage.flattened_ud_buf.size() * sizeof(u32));
            }
            continue;
        }
        const auto vsharp = prepared_stage ? prepared_stage->buffer_sharps[index]
                                           : desc.GetSharp(stage);
        const VAddr address = vsharp.base_address;
        if (address == 0 || vsharp.GetSize() == 0) {
            continue;
        }
        const u64 size = memory->ClampRangeSize(address, vsharp.GetSize());
        if (desc.is_written) {
            NotePendingGpuWrite(address, size);
            continue;
        }
        // The stream path of BufferCache::ObtainBuffer, taken here: small, read-only, not
        // written by the GPU (now or by work still queued for the recording thread).
        if (size == 0 || size > VideoCore::BufferCache::STREAM_THRESHOLD ||
            buffer_cache.IsRegionGpuModified(address, size) || PendingWriteOverlaps(address, size)) {
            continue;
        }
        copy(index, nullptr, address, size);
    }
}

void Rasterizer::PostDraw(const Pipeline* pipeline, const PreparedDraw* used_prepared,
                          bool is_indexed, u32 index_offset, const AmdGpu::ComputeProgram* cs,
                          const IndirectDraw* indirect) {
    auto& dirty = liverpool->pipe_dirty;
    if (!pipe_synced) {
        // Stage B has not run yet: its register copy starts as this one.
        pipe_regs = liverpool->regs;
        dirty.Clear();
        pipe_synced = true;
    }
    std::array<u16, AmdGpu::RegDirty::NumBlocks> blocks;
    u32 num_blocks = 0;
    dirty.blocks.ForEach([&](u32 block) { blocks[num_blocks++] = static_cast<u16>(block); });
    const auto stages =
        pipeline ? pipeline->GetStages() : std::span<const Shader::Info* const>{};
    // Constants: copied here into the ring, the recording thread only binds them.
    thread_local std::array<boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>,
                            Shader::MaxStageTypes>
        rings;
    u32 num_stages = 0;
    u32 size = sizeof(DrawPacket) + AlignPacket(num_blocks * sizeof(u16)) +
               num_blocks * AmdGpu::RegDirty::BlockWords * sizeof(u32);
    for (const auto* stage : stages) {
        if (stage) {
            auto& ring = rings[num_stages];
            ring.clear();
            if (constant_ring && !TlgToggle::Disabled(TlgToggle::ConstantRing)) {
                CollectRingBindings(*stage, used_prepared, ring);
            }
            ++num_stages;
            size += sizeof(PacketStage) +
                    AlignPacket(u32(stage->user_data.size() + stage->flattened_ud_buf.size()) *
                                sizeof(u32)) +
                    u32(ring.size() * sizeof(RingBinding));
        }
    }
    if (cs) {
        size += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    if (indirect) {
        size += AlignPacket(sizeof(IndirectDraw));
    }
    const u32 interval = VerifyInterval();
    const bool verify = interval && (draw_pipe->packets % interval) == 0;
    if (verify) {
        size += sizeof(AmdGpu::Regs);
    }

    u8* out = draw_pipe->Begin(size);
    u8* const start = out;
    auto& packet = *reinterpret_cast<DrawPacket*>(out);
    packet.kind = cs         ? (indirect ? PacketKind::IndirectDispatch : PacketKind::Dispatch)
                  : indirect ? PacketKind::Indirect
                  : pipeline ? PacketKind::Draw
                             : PacketKind::Special;
    packet.pipeline = pipeline;
    packet.prepared = used_prepared;
    packet.index_offset = index_offset;
    packet.is_indexed = is_indexed;
    packet.regs_reset = dirty.reset;
    packet.num_blocks = static_cast<u16>(num_blocks);
    packet.num_stages = num_stages;
    packet.verify = verify;
    packet.cb_extent = liverpool->last_cb_extent;
    packet.db_extent = liverpool->last_db_extent;
    packet.ring_end = constant_ring ? constant_ring->Position() : 0;
    out += sizeof(DrawPacket);
    std::memcpy(out, blocks.data(), num_blocks * sizeof(u16));
    out += AlignPacket(num_blocks * sizeof(u16));
    for (u32 i = 0; i < num_blocks; ++i) {
        std::memcpy(out,
                    liverpool->regs.reg_array.data() + blocks[i] * AmdGpu::RegDirty::BlockWords,
                    AmdGpu::RegDirty::BlockWords * sizeof(u32));
        out += AmdGpu::RegDirty::BlockWords * sizeof(u32);
    }
    u32 stage_index = 0;
    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const auto& ring = rings[stage_index++];
        auto& header = *reinterpret_cast<PacketStage*>(out);
        header = {stage,
                  stage->pgm_base,
                  static_cast<u32>(stage->user_data.size()),
                  static_cast<u32>(stage->flattened_ud_buf.size()),
                  static_cast<u32>(ring.size()),
                  0};
        out += sizeof(PacketStage);
        std::memcpy(out, ring.data(), ring.size() * sizeof(RingBinding));
        out += ring.size() * sizeof(RingBinding);
        std::memcpy(out, stage->user_data.data(), stage->user_data.size_bytes());
        std::memcpy(out + stage->user_data.size_bytes(), stage->flattened_ud_buf.data(),
                    stage->flattened_ud_buf.size() * sizeof(u32));
        out += AlignPacket(u32(stage->user_data.size() + stage->flattened_ud_buf.size()) *
                           sizeof(u32));
    }
    if (cs) {
        std::memcpy(out, cs, sizeof(AmdGpu::ComputeProgram));
        out += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    if (indirect) {
        std::memcpy(out, indirect, sizeof(IndirectDraw));
        out += AlignPacket(sizeof(IndirectDraw));
    }
    if (verify) {
        std::memcpy(out, &liverpool->regs, sizeof(AmdGpu::Regs));
        out += sizeof(AmdGpu::Regs);
    }
    dirty.Clear();
    draw_pipe->Commit(static_cast<u32>(out - start));
    PrintPipeStats();
}

void Rasterizer::RetireSubmission() {
    if (!draw_pipe || !OnStageA()) {
        return;
    }
    while (!pipe_keepalive.empty() && draw_pipe->Reached(pipe_keepalive.front().first)) {
        pipe_keepalive.pop_front();
    }
    if (!draw_pipe->Idle()) {
        pipe_keepalive.emplace_back(draw_pipe->Head(), draw_prep->KeepCurrent());
    }
}

bool Rasterizer::RunInOrder(OrderedTask task, const void* data, u32 size, u64 toggle) {
    if (!UseDrawPipe() || TlgToggle::Disabled(toggle)) {
        DrainDrawPipe(DrawPipe::ReasonRasterizer);
        task(*this, static_cast<const u8*>(data));
        return false;
    }
    const u32 total = sizeof(TaskPacket) + AlignPacket(size);
    u8* out = draw_pipe->Begin(total);
    *reinterpret_cast<TaskPacket*>(out) = {PacketKind::Task, size, task};
    std::memcpy(out + sizeof(TaskPacket), data, size);
    draw_pipe->Commit(total);
    return true;
}

namespace {
void RunCommandTask(Rasterizer&, const u8* data) {
    auto* command = *reinterpret_cast<Common::UniqueFunction<void>* const*>(data);
    (*command)();
    delete command;
}
} // namespace

bool Rasterizer::RunCommandInOrder(Common::UniqueFunction<void>&& command) {
    if (!UseDrawPipe() || !OnStageA() || TlgToggle::Disabled(TlgToggle::PipelinedCommands)) {
        return false;
    }
    auto* heap = new Common::UniqueFunction<void>(std::move(command));
    if (!RunInOrder(&RunCommandTask, &heap, sizeof(heap), TlgToggle::PipelinedCommands)) {
        return true; // ran here after a drain (RunInOrder's fallback), and freed by the task
    }
    return true;
}

void Rasterizer::RunDrawPacket(void* context, const u8* data, u32 size) {
    auto& self = *static_cast<Rasterizer*>(context);
    if (*reinterpret_cast<const PacketKind*>(data) == PacketKind::Task) {
        const auto& task = *reinterpret_cast<const TaskPacket*>(data);
        task.task(self, data + sizeof(TaskPacket));
        return;
    }
    const auto& packet = *reinterpret_cast<const DrawPacket*>(data);
    const u8* in = data + sizeof(DrawPacket);
    const auto* blocks = reinterpret_cast<const u16*>(in);
    in += AlignPacket(packet.num_blocks * sizeof(u16));
    auto& regs = self.pipe_regs;
    if (packet.regs_reset) {
        regs.SetDefaults();
    }
    for (u32 i = 0; i < packet.num_blocks; ++i) {
        std::memcpy(regs.reg_array.data() + blocks[i] * AmdGpu::RegDirty::BlockWords, in,
                    AmdGpu::RegDirty::BlockWords * sizeof(u32));
        in += AmdGpu::RegDirty::BlockWords * sizeof(u32);
    }
    ASSERT(packet.num_stages <= Shader::Info::MaxUdSnapshots);
    for (u32 i = 0; i < packet.num_stages; ++i) {
        const auto& stage = *reinterpret_cast<const PacketStage*>(in);
        in += sizeof(PacketStage);
        self.ring_stages[i] = {stage.info, reinterpret_cast<const RingBinding*>(in),
                               stage.num_ring};
        in += stage.num_ring * sizeof(RingBinding);
        const auto* words = reinterpret_cast<const u32*>(in);
        Shader::Info::ud_snapshots[i] = {stage.info, words, stage.ud_size, words + stage.ud_size,
                                         stage.flat_size, stage.pgm_base};
        in += AlignPacket((stage.ud_size + stage.flat_size) * sizeof(u32));
    }
    Shader::Info::num_ud_snapshots = packet.num_stages;
    self.num_ring_stages = packet.num_stages;
    const AmdGpu::ComputeProgram* cs = nullptr;
    if (packet.kind == PacketKind::Dispatch || packet.kind == PacketKind::IndirectDispatch) {
        cs = reinterpret_cast<const AmdGpu::ComputeProgram*>(in);
        in += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    const IndirectDraw* indirect = nullptr;
    if (packet.kind == PacketKind::Indirect || packet.kind == PacketKind::IndirectDispatch) {
        indirect = reinterpret_cast<const IndirectDraw*>(in);
        in += AlignPacket(sizeof(IndirectDraw));
    }
    if (packet.verify) {
        // The resource tables the GPU thread walked must read the same now: else a write
        // still queued here (WriteData, DMA) changed them after that thread read them.
        static u32 flat_reports = 0;
        for (u32 i = 0; i < packet.num_stages && flat_reports < 32; ++i) {
            const auto& snapshot = Shader::Info::ud_snapshots[i];
            const auto& srt = snapshot.info->srt_info;
            // Stages the pipeline selection did not refresh (no user data) keep their tables.
            if (!srt.walker_func || snapshot.flat_size == 0 || snapshot.user_data_size == 0) {
                continue;
            }
            std::vector<u32> flat(snapshot.flat_size);
            std::memcpy(flat.data(), snapshot.user_data,
                        std::min(snapshot.user_data_size, snapshot.flat_size) * sizeof(u32));
            // Pointers in the tables may be stale by now: a fault only skips the check.
            sigjmp_buf recover;
            if (sigsetjmp(recover, 0)) {
                runtime_fault_recover = nullptr;
                continue;
            }
            runtime_fault_recover = &recover;
            Shader::RunSrtWalker(srt.walker_func, snapshot.user_data, flat.data());
            runtime_fault_recover = nullptr;
            if (std::memcmp(flat.data(), snapshot.flat, snapshot.flat_size * sizeof(u32)) != 0) {
                u32 first = 0, count = 0;
                for (u32 w = 0; w < snapshot.flat_size; ++w) {
                    if (flat[w] != snapshot.flat[w]) {
                        first = count++ ? first : w;
                    }
                }
                std::printf("Draw pipe: resource tables of shader %016llx changed after the GPU "
                            "thread read them (%u of %u words, first %u: %#x -> %#x, user data "
                            "%u words)\n",
                            static_cast<unsigned long long>(snapshot.info->pgm_hash), count,
                            snapshot.flat_size, first, snapshot.flat[first], flat[first],
                            snapshot.user_data_size);
                ++flat_reports;
            }
        }
        const auto* live = reinterpret_cast<const AmdGpu::Regs*>(in);
        in += sizeof(AmdGpu::Regs);
        static u32 reports = 0;
        for (u32 block = 0; block < AmdGpu::RegDirty::NumBlocks && reports < 32; ++block) {
            const u32 at = block * AmdGpu::RegDirty::BlockWords;
            if (std::memcmp(regs.reg_array.data() + at, live->reg_array.data() + at,
                            AmdGpu::RegDirty::BlockWords * sizeof(u32)) != 0) {
                for (u32 w = at; w < at + AmdGpu::RegDirty::BlockWords; ++w) {
                    if (regs.reg_array[w] != live->reg_array[w]) {
                        std::printf("Draw pipe: register copy differs at word %#x (%#x vs %#x)\n",
                                    w, regs.reg_array[w], live->reg_array[w]);
                        ++reports;
                        break;
                    }
                }
            }
        }
    }
    ASSERT(static_cast<u32>(in - data) == size);
    stage_regs = &regs;
    self.pipe_cb_extent = packet.cb_extent;
    self.pipe_db_extent = packet.db_extent;
    if (packet.kind == PacketKind::Special) {
        // A draw FilterDraw handles (fast clear elimination, resolve, depth copy, skip).
        FrameCapture::Poll();
        self.scheduler.PopPendingOperations();
        self.FilterDraw();
    } else if (cs) {
        stage_cs = cs;
        self.dispatch_flags = packet.index_offset;
        const auto* pipeline = static_cast<const ComputePipeline*>(packet.pipeline);
        if (indirect) {
            self.DispatchIndirectRecord(pipeline, indirect->args, indirect->stride);
        } else {
            self.DispatchRecord(pipeline);
        }
        stage_cs = nullptr;
    } else if (indirect) {
        self.DrawIndirectRecord(static_cast<const GraphicsPipeline*>(packet.pipeline),
                                packet.is_indexed, *indirect);
    } else {
        self.DrawRecord(static_cast<const GraphicsPipeline*>(packet.pipeline), packet.prepared,
                        packet.is_indexed, packet.index_offset);
    }
    Shader::Info::num_ud_snapshots = 0;
    self.num_ring_stages = 0;
    if (self.constant_ring) {
        self.constant_ring->Stamp(packet.ring_end);
    }
}

void Rasterizer::PrintPipeStats() {
    static const bool stats = std::getenv("TLG_FRAME_STATS") != nullptr;
    if (!stats || (draw_pipe->packets & 1023) != 0) {
        return;
    }
    static auto window = std::chrono::steady_clock::now();
    static u64 last_packets = 0, last_drains = 0, last_drain_cycles = 0, last_busy = 0;
    static u64 last_ring_waits = 0, last_ring_cycles = 0, last_spin = 0, last_sleeps = 0;
    static u64 last_tsc = Common::CpuCycles();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - window).count();
    if (seconds < 5.0) {
        return;
    }
    const u64 tsc = Common::CpuCycles();
    const double cycles = double(tsc - last_tsc);
    const u64 busy = draw_pipe->busy_cycles.load(std::memory_order_relaxed);
    const u64 spin = draw_pipe->spin_cycles.load(std::memory_order_relaxed);
    const u64 sleeps = draw_pipe->sleeps.load(std::memory_order_relaxed);
    std::printf("Draw pipe: %.0f draws/s pipelined, %.0f drains/s that waited, stage A waited "
                "%.1f%%, stage B busy %.1f%%, stage B idle-spin %.1f%% (%llu sleeps); %llu "
                "ring-full waits, stage A waited %.1f%% for room\n",
                (draw_pipe->packets - last_packets) / seconds,
                (draw_pipe->drains - last_drains) / seconds,
                100.0 * (draw_pipe->drain_cycles - last_drain_cycles) / cycles,
                100.0 * (busy - last_busy) / cycles,
                100.0 * (spin - last_spin) / cycles,
                static_cast<unsigned long long>(sleeps - last_sleeps),
                static_cast<unsigned long long>(draw_pipe->ring_waits - last_ring_waits),
                100.0 * (draw_pipe->ring_wait_cycles - last_ring_cycles) / cycles);
    last_ring_waits = draw_pipe->ring_waits;
    last_ring_cycles = draw_pipe->ring_wait_cycles;
    last_spin = spin;
    last_sleeps = sleeps;
    std::printf("  drains by reason/s:");
    for (u32 r = 0; r < DrawPipe::NumReasons; ++r) {
        if (const u64 n = draw_pipe->drains_by_reason[r]) {
            static constexpr const char* names[] = {"ConstRam", "commands", "compute",
                                                    "submission end", "draw", "rasterizer"};
            const double share = 100.0 * draw_pipe->cycles_by_reason[r] / cycles;
            draw_pipe->cycles_by_reason[r] = 0;
            if (r < 256) {
                std::printf(" %s=%.0f (%.1f%%)",
                            magic_enum::enum_name(AmdGpu::PM4ItOpcode(r)).data(), n / seconds,
                            share);
            } else {
                std::printf(" %s=%.0f (%.1f%%)", names[r - 256], n / seconds, share);
            }
        }
    }
    std::printf("\n  compute queue steps over 0.5%%:");
    for (u32 r = 0; r < DrawPipe::NumReasons; ++r) {
        const u64 step_cycles = draw_pipe->compute_cycles[r];
        if (step_cycles * 200 > cycles) {
            std::printf(" %s=%.0f (%.1f%%)",
                        r < 256 ? magic_enum::enum_name(AmdGpu::PM4ItOpcode(r)).data()
                                : "ProcessCommands",
                        draw_pipe->compute_packets[r] / seconds, 100.0 * step_cycles / cycles);
        }
    }
    std::printf("\n  stage A waits over 0.5%%:");
    for (const auto& [function, lines] : drain_sites) {
        for (const auto& [line, waited] : lines) {
            if (waited * 200 > cycles) {
                std::printf(" %s:%u %.1f%%", function, line, 100.0 * waited / cycles);
            }
        }
    }
    std::printf("\n");
    drain_sites.clear();
    draw_pipe->drains_by_reason = {};
    draw_pipe->compute_cycles = {};
    draw_pipe->compute_packets = {};
    window = now;
    last_tsc = tsc;
    last_packets = draw_pipe->packets;
    last_drains = draw_pipe->drains;
    last_drain_cycles = draw_pipe->drain_cycles;
    last_busy = busy;
}

bool Rasterizer::FilterDrawPasses() const {
    // FilterDraw's checks without its side effects: false when it would skip the draw or run
    // a pass of its own (fast clear elimination, resolve, depth/stencil copy).
    const auto& regs = Regs();
    using Mode = AmdGpu::ColorControl::OperationMode;
    const auto mode = regs.color_control.mode;
    if (mode == Mode::EliminateFastClear || mode == Mode::FmaskDecompress ||
        mode == Mode::Resolve || regs.primitive_type == AmdGpu::PrimitiveType::None) {
        return false;
    }
    const bool depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const bool stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    return !(mode == Mode::Disable && (depth_copy || stencil_copy));
}

namespace {
// tlg: TLG_DISPATCH_LOG=<n> prints dispatches of at least n thread groups and draws of at least
// n vertices (times instances), and the first 64 indirect ones (64 more tessellated) with their
// arguments as the CPU
// sees them (the GPU may write them later), to find work that keeps the GPU busy until the
// watchdog (Metal "GPU Hang").
/// tlg: TLG_TIMING_AFTER_S=N (diagnostic): TLG_DRAW_TIMING / TLG_DISPATCH_TIMING start only N seconds
/// after the first draw, so a state that only appears untimed (Seattle: FPS halves when someone walks
/// past) is timed once it is there.
bool TimingAllowed() {
    static const int after = [] {
        const char* env = std::getenv("TLG_TIMING_AFTER_S");
        return env && *env ? std::atoi(env) : 0;
    }();
    if (after <= 0) {
        return true;
    }
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::steady_clock::now() - start >= std::chrono::seconds(after);
}

u64 DispatchLogThreshold() {
    static const u64 threshold = [] {
        const char* env = std::getenv("TLG_DISPATCH_LOG");
        return env && *env ? std::strtoull(env, nullptr, 10) : 0;
    }();
    return threshold;
}

// TLG_DRAW_LABELS=1: a debug label before each draw (shader hashes and counts). MoltenVK lists them
// as signposts of the encoder that faulted when MVK_CONFIG_DEBUG=1 reports a GPU error.
struct DrawLabel {
    std::array<char, 96> text{};
    explicit operator bool() const {
        return text[0] != 0;
    }
};

DrawLabel MakeDrawLabel(const char* kind, u64 vs, u64 ps, u32 a, u32 b) {
    static const bool enabled = [] {
        const char* env = std::getenv("TLG_DRAW_LABELS");
        return env && *env == '1';
    }();
    DrawLabel label;
    if (enabled) {
        std::snprintf(label.text.data(), label.text.size(), "%s vs %llx ps %llx %u x %u", kind,
                      static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps),
                      a, b);
    }
    return label;
}

void InsertLabel(vk::CommandBuffer cmdbuf, const DrawLabel& label) {
    if (label) {
        cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = label.text.data()});
    }
}

void LogWork(const char* kind, u64 hash, u32 x, u32 y, u32 z) {
    // TLG_DISPATCH_LOG_CS=<hex hash>: the first 40 dispatches and draws of that shader, any size.
    static const u64 only = [] {
        const char* env = std::getenv("TLG_DISPATCH_LOG_CS");
        return env && *env ? std::strtoull(env, nullptr, 16) : 0ull;
    }();
    static std::atomic<u32> only_logged{0};
    if (only != 0 && hash == only && only_logged.fetch_add(1, std::memory_order_relaxed) < 40) {
        std::printf("GPU work: %s %016llx %ux%ux%u\n", kind, static_cast<unsigned long long>(hash),
                    x, y, z);
        return;
    }
    const u64 threshold = DispatchLogThreshold();
    if (threshold == 0) {
        return; // the usual case, before the string searches below (once per draw)
    }
    static std::atomic<u32> indirect_logged{0}, tess_logged{0};
    const bool indirect = std::strstr(kind, "indirect") != nullptr;
    auto& logged = std::strstr(kind, "tess") ? tess_logged : indirect_logged;
    if (threshold == 0 ||
        (u64(x) * y * z < threshold &&
         !(indirect && logged.fetch_add(1, std::memory_order_relaxed) < 64))) {
        return;
    }
    std::printf("GPU work: %s %016llx %ux%ux%u\n", kind,
                static_cast<unsigned long long>(hash), x, y, z);
}
} // namespace

void Rasterizer::Draw(bool is_indexed, u32 index_offset, const PreparedDraw* prepared) {
    RENDERER_TRACE;
    TlgStats::draws.fetch_add(1, std::memory_order_relaxed);

    // tlg: with the draw pipeline this thread only selects the pipeline and hands the draw
    // to the recording thread (DrawRecord there); draws FilterDraw handles itself run here.
    const bool pipelined = UseDrawPipe();
    if (pipelined && !FilterDrawPasses() &&
        !TlgToggle::Disabled(TlgToggle::PipelinedMemoryWrites)) {
        PostDraw(nullptr, nullptr, false, 0);
        return;
    }
    if (!pipelined || !FilterDrawPasses()) {
        DrainDrawPipe(DrawPipe::ReasonDraw);
        FrameCapture::Poll();
        scheduler.PopPendingOperations();
        if (!FilterDraw()) {
            return;
        }
    }
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline({}, prepared);
    const PreparedDraw* used_prepared = pipeline_cache.UsedPrepared();
    if (prepared) {
        const u32 prep_state = prepared->state.load(std::memory_order_acquire);
        const bool checksum_ok = prepared->reg_checksum == liverpool->gfx_reg_checksum;
        draw_prep->Count(pipeline != nullptr && prep_state == PreparedDraw::Ready && checksum_ok,
                         prep_state, checksum_ok);
    }
    if (!pipeline) {
        return;
    }
    // tlg: TLG_SKIP_ADJACENCY=1 drops draws with adjacency topology (geometry-shader outlines),
    // to tell whether a broken effect comes from them.
    static const bool skip_adjacency = std::getenv("TLG_SKIP_ADJACENCY") != nullptr;
    if (skip_adjacency) {
        const auto prim = pipeline->GetGraphicsKey().prim_type;
        if (prim == AmdGpu::PrimitiveType::AdjTriangleList ||
            prim == AmdGpu::PrimitiveType::AdjTriangleStrip) {
            return;
        }
    }
    if (pipelined && FilterDrawPasses()) {
        PostDraw(pipeline, used_prepared, is_indexed, index_offset);
        return;
    }
    DrawRecord(pipeline, used_prepared, is_indexed, index_offset);
}

void Rasterizer::DiagPassBoundary(bool force) {
    static const bool enabled = [] {
        const char* env = std::getenv("TLG_DIAG_IMAGES");
        return env && *env == '1';
    }();
    if (!enabled) {
        return;
    }
    const auto& regs = Regs();
    std::array<VAddr, 8> addresses{};
    for (u32 cb = 0; cb < addresses.size(); ++cb) {
        if (regs.color_buffers[cb]) {
            addresses[cb] = regs.color_buffers[cb].Address();
        }
    }
    if (!force && addresses == diag_addresses) {
        return;
    }
    if (FrameCapture::IsRecording() && !diag_targets.empty()) {
        char tag[16];
        std::snprintf(tag, sizeof(tag), "p%03u", diag_pass++);
        const auto targets = diag_targets;
        for (const auto image_id : targets) {
            texture_cache.DiagDumpImage(image_id, tag);
        }
    }
    diag_targets.clear();
    diag_addresses = force ? std::array<VAddr, 8>{} : addresses;
}

namespace {
// tlg: TLG_PASS_STATS=1: draws drawn through host tessellation (the game's own tessellation and
// rect/quad lists' auxiliary shaders), direct and indirect, per second, every 5 s. MoltenVK
// emulates tessellation with compute, which ends the Metal render pass at each such draw.
void CountTessDraw(const GraphicsPipeline* pipeline, bool indirect) {
    static const bool stats = [] {
        const char* env = std::getenv("TLG_PASS_STATS");
        return env && *env == '1';
    }();
    if (!stats) {
        return;
    }
    const auto prim = pipeline->GetGraphicsKey().prim_type;
    const bool rect = prim == AmdGpu::PrimitiveType::RectList || prim == AmdGpu::PrimitiveType::QuadList;
    const bool tess = pipeline->GetStages()[u32(Shader::SwStage::TessellationControl)] != nullptr;
    static std::atomic<u64> counts[4]{}; // rect direct, rect indirect, tess direct, tess indirect
    static std::atomic<u64> total{0};
    static std::atomic<s64> window{0};
    total.fetch_add(1, std::memory_order_relaxed);
    if (rect || tess) {
        counts[(tess ? 2 : 0) + (indirect ? 1 : 0)].fetch_add(1, std::memory_order_relaxed);
    }
    const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
    s64 start = window.load(std::memory_order_relaxed);
    if (start == 0) {
        window.compare_exchange_strong(start, now);
    } else if (now - start >= 5000 && window.compare_exchange_strong(start, now)) {
        const double secs = (now - start) / 1000.0;
        std::printf("Tess draws per second: rect/quad lists %.0f direct %.0f indirect, game "
                    "tessellation %.0f direct %.0f indirect, of %.0f draws\n",
                    counts[0].exchange(0) / secs, counts[1].exchange(0) / secs,
                    counts[2].exchange(0) / secs, counts[3].exchange(0) / secs,
                    total.exchange(0) / secs);
    }
}
} // namespace

namespace {
// tlg: TLG_DRAW_TIMING=1: every 15 s the next 3000 draws run one at a time (submitted and waited
// for, each in a render pass of its own) and the GPU time per vertex/pixel shader pair is
// printed. Every entry includes a fixed submit + pass overhead (compare with the median);
// the shaders that dominate the frame stand out above it.
struct DrawTiming {
    bool enabled = [] {
        const char* env = std::getenv("TLG_DRAW_TIMING");
        return env && *env == '1';
    }();
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    u32 left = 0;
    std::unordered_map<u64, std::tuple<double, u32, u64, u64>> per_pair;

    bool Begin() {
        if (!enabled) {
            return false;
        }
        if (left == 0) {
            if (!TimingAllowed() || std::chrono::steady_clock::now() < next) {
                return false;
            }
            left = 3000;
            per_pair.clear();
        }
        return true;
    }
    void Add(u64 vs, u64 ps, double ms) {
        auto& [total, count, v, p] = per_pair[vs * 31 + ps];
        total += ms;
        ++count;
        v = vs;
        p = ps;
        if (--left != 0) {
            return;
        }
        next = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        std::vector<std::tuple<double, u32, u64, u64>> sorted;
        double all = 0;
        u32 draws = 0;
        for (const auto& [key, entry] : per_pair) {
            sorted.push_back(entry);
            all += std::get<0>(entry);
            draws += std::get<1>(entry);
        }
        std::ranges::sort(sorted, [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
        std::printf("Draw timing: %u draws, %.1f ms of GPU with overhead (%.3f ms/draw on average)\n",
                    draws, all, all / std::max(draws, 1u));
        for (size_t i = 0; i < std::min<size_t>(sorted.size(), 15); ++i) {
            const auto& [total, count, v, ps_hash] = sorted[i];
            std::printf("  vs %016llx ps %016llx: %8.2f ms in %4u draws (%.3f ms each)\n",
                        static_cast<unsigned long long>(v), static_cast<unsigned long long>(ps_hash),
                        total, count, total / count);
        }
    }
};
DrawTiming& DrawTimer() {
    static DrawTiming timing;
    return timing;
}
} // namespace

void Rasterizer::DrawRecord(const GraphicsPipeline* pipeline, const PreparedDraw* used_prepared,
                            bool is_indexed, u32 index_offset) {
    // tlg: TLG_SKIP_DRAW_PS=<hex pixel shader hashes, comma-separated> (diagnostic): drops the
    // direct draws of those pixel shaders, to see what they draw.
    static const std::vector<u64> skip_ps = [] {
        std::vector<u64> list;
        if (const char* env = std::getenv("TLG_SKIP_DRAW_PS")) {
            for (const char* p = env; *p;) {
                char* end = nullptr;
                const u64 v = std::strtoull(p, &end, 16);
                if (end == p) {
                    break;
                }
                list.push_back(v);
                p = *end == ',' ? end + 1 : end;
            }
        }
        return list;
    }();
    if (!skip_ps.empty()) [[unlikely]] {
        const auto stages = pipeline->GetStages();
        const auto* ps = stages[u32(Shader::SwStage::Fragment)];
        if (ps && std::ranges::find(skip_ps, ps->pgm_hash) != skip_ps.end()) {
            return;
        }
    }
    CountTessDraw(pipeline, false);
    const bool draw_timed = DrawTimer().Begin();
    if (draw_timed) {
        scheduler.EndRendering();
        scheduler.Finish();
    }
    // Read the clock only when timing (TLG_DRAW_TIMING): a clock read per draw is ~2% of the recording thread.
    const auto draw_timed_start =
        draw_timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (DrawPipe::OnStageB()) {
        FrameCapture::Poll();
        scheduler.PopPendingOperations();
    }
    DiagPassBoundary(false);
    const auto& regs = Regs();
    // tlg: the pass copying a finished frame to a display buffer; the previous draw's
    // target is that frame.
    if (camera_motion->Enabled() && regs.color_buffers[0] &&
        FrameCapture::IsDisplayBuffer(regs.color_buffers[0].Address())) {
        scene_started = false;
        camera_motion->OnDisplayPass(cb_descs[0].first);
        if (upscaler->OnFrameStart()) {
            object_motion->InvalidateHistory();
            camera_motion->InvalidateHistory();
        }
        camera_motion->SetJitter(upscaler->Jitter());
        object_motion->OnFrameStart();
        NoteFrameStart();
        static const char* scene_debug = std::getenv("TLG_SCENE_DEBUG");
        scene_debug_frame = scene_debug && std::remove(scene_debug) == 0;
        scene_targets->debug = scene_debug_frame;
    }
    bind_prepared = used_prepared;
    motion_draw = pipeline->GetGraphicsKey().motion_vectors;
    motion_geometry = 0;

    PrepareRenderState(pipeline);
    if (upscaler->Enabled() && std::popcount(pipeline->GetGraphicsKey().mrt_mask) == 1) {
        const auto& viewport = Regs().viewports[0];
        upscaler->OnDraw(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                         cb_descs[0].first, db_desc.first,
                         UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2));
    }
    const PreparedDraw* draw_prepared = bind_prepared;
    // tlg: vertex and index buffers are resolved while the helper binds textures (their
    // commands are recorded after BeginRendering, as before).
    draw_inputs = {pipeline, draw_prepared, index_offset, is_indexed, true, false};
    const bool bound = BindResources(pipeline);
    bind_prepared = nullptr; // indirect draws and dispatches bind without prepared sharps
    const bool inputs_resolved = draw_inputs.resolved;
    draw_inputs.pending = false;
    if (!bound) {
        return;
    }
    const auto state = BeginRendering(pipeline);
    // tlg: before anything of the draw itself is recorded, which has to go with it.
    SubmitIfWorkPiledUp(state);
    scheduler.CountWork();

    if (!inputs_resolved) {
        ResolveVertexBuffers(pipeline, draw_prepared);
        if (is_indexed) {
            ResolveIndexBuffer(index_offset);
        }
    }
    EmitVertexBuffers();
    if (is_indexed) {
        EmitIndexBuffer();
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    // tlg: screen-space (clip disabled) draws into the upscaler's output-size images.
    push_data.xscale *= target_scale[0];
    push_data.xoffset *= target_scale[0];
    push_data.yscale *= target_scale[1];
    push_data.yoffset *= target_scale[1];
    if (motion_draw && motion_geometry) {
        const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
        const auto [base_vertex, first_instance] =
            GetDrawOffsets(regs, vs, pipeline->GetFetchShader());
        const u32 index_size =
            regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2u : 4u;
        const VAddr index_address = is_indexed
            ? regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size : 0;
        // Without a character-size skeleton the draw is gated on its bone palette changing
        // since last frame (model constants follow the camera, the palette does not). The
        // check comes first: most gated draws are static world pieces with long index lists.
        static const bool all_motion = [] {
            const char* value = std::getenv("TLG_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        bool gated = !all_motion;
        u64 palette = 0;
        for (const auto& resource : vs.buffers) {
            if (resource.IsSpecial()) continue;
            const auto buffer = resource.GetSharp(vs);
            const u32 size = buffer.GetSize();
            if (!buffer.Valid() || buffer.GetStride() != 16) continue;
            const auto role = Motion::ClassifyBuffer(size);
            if (role == Motion::BufferRole::Skeleton) {
                gated = false;
                break;
            }
            const VAddr address = buffer.base_address;
            if (role == Motion::BufferRole::SmallSkeleton &&
                memory->IsValidGpuMapping(address, 0) &&
                memory->ClampRangeSize(address, size) == size) {
                palette = XXH3_64bits_withSeed(reinterpret_cast<const void*>(address), size,
                                               palette);
            }
        }
        push_data.motion_param = 0;
        if (!gated || object_motion->Moving({.shader = vs.pgm_hash,
                                             .geometry = motion_geometry,
                                             .indices = index_address,
                                             .index_count = regs.num_indices,
                                             .instances = regs.num_instances.NumInstances(),
                                             .first_instance = first_instance},
                                            palette)) {
            Motion::VertexRange range{base_vertex, regs.num_indices};
            u64 topology = 0;
            if (is_indexed) {
                range = {};
                const u64 bytes = u64(regs.num_indices) * index_size;
                if (index_address && memory->IsValidGpuMapping(index_address, 0) &&
                    memory->ClampRangeSize(index_address, bytes) == bytes) {
                    const bool restart = regs.enable_primitive_restart != 0;
                    const u32 count = regs.num_indices;
                    const auto scanned = object_motion->IndexRange(
                        {index_address, count, index_size, s32(base_vertex), restart}, [&] {
                            const auto* data = reinterpret_cast<const void*>(index_address);
                            const auto scan_range =
                                index_size == 2
                                    ? Motion::IndexedRange(
                                          std::span(static_cast<const u16*>(data), count),
                                          s32(base_vertex), restart)
                                    : Motion::IndexedRange(
                                          std::span(static_cast<const u32*>(data), count),
                                          s32(base_vertex), restart);
                            return Motion::IndexRangeCache::Result{scan_range,
                                                                   XXH3_64bits(data, bytes)};
                        });
                    range = scanned.range;
                    topology = scanned.topology;
                }
            }
            push_data.motion_param = object_motion->PrepareDraw({
                .shader = vs.pgm_hash,
                .geometry = motion_geometry,
                .indices = index_address,
                .topology = topology,
                .index_count = regs.num_indices,
                .instances = regs.num_instances.NumInstances(),
                .first_instance = first_instance,
                .vertices = range,
            });
        }
    }
    pipeline->BindResources(set_writes, set_write_index, push_data);
    // tlg: jitter geometry drawn with the scene depth, not full-screen passes (a shifted
    // full-screen quad leaves an edge column unwritten).
    draw_jitter = {};
    if (upscaler->Enabled() && db_desc.first && db_desc.first == camera_motion->Depth() &&
        (regs.num_indices > 6 || regs.num_instances.NumInstances() > 1)) {
        draw_jitter = upscaler->Jitter();
    }
    UpdateDynamicState(pipeline, is_indexed);
    MarkPass(pipeline, state);
    if (FrameRecord::IsRecording()) [[unlikely]] {
        RecordFrameInputs(pipeline, draw_prepared, &state, is_indexed, index_offset, nullptr);
    }
    scheduler.BeginRendering(state);
    if (!pass_label.empty()) [[unlikely]] {
        scheduler.Record([label = std::move(pass_label)](vk::CommandBuffer cmdbuf) {
            cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = label.c_str()});
        });
        pass_label.clear();
    }
    CountDraw(state, pipeline);

    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const vk::Pipeline handle = pipeline->Handle();
    const vk::Pipeline square = SquarePass(*pipeline);
    const u32 num_indices = regs.num_indices;
    const u32 num_instances = regs.num_instances.NumInstances();
    const u32 first_vertex = vertex_offset;
    const u32 first_instance = instance_offset;
    LogWork(is_indexed ? "indexed draw vs" : "draw vs", vs_info.pgm_hash, num_indices,
            num_instances, 1);
    // tlg: TLG_NEW_SHADERS=1 (diagnostic): the first draw of each vertex/pixel shader pair, with the
    // time since the first one: what joins the scene when the frame rate changes (people walking past).
    static const bool new_shaders = [] {
        const char* env = std::getenv("TLG_NEW_SHADERS");
        return env && *env == '1';
    }();
    if (new_shaders) [[unlikely]] {
        static std::set<std::pair<u64, u64>> seen;
        static const auto start = std::chrono::steady_clock::now();
        const auto* ps_stage = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        const u64 ps_hash = ps_stage ? ps_stage->pgm_hash : 0;
        if (seen.emplace(vs_info.pgm_hash, ps_hash).second) {
            std::printf("New shaders at %.1f s: vs %016llx ps %016llx, %u indices x %u instances\n",
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                        (unsigned long long)vs_info.pgm_hash, (unsigned long long)ps_hash, num_indices,
                        num_instances);
        }
    }
    // tlg: TLG_DRAW_UD_LOG=<hex vs hash>: the vertex stage's user data of that shader's first 20
    // draws (loop bounds and buffer descriptors of a draw that hangs the GPU).
    static const u64 ud_log = [] {
        const char* env = std::getenv("TLG_DRAW_UD_LOG");
        return env && *env ? std::strtoull(env, nullptr, 16) : 0ull;
    }();
    if (ud_log != 0 && vs_info.pgm_hash == ud_log) {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 20) {
            const auto& ud = regs.vs_program.user_data;
            std::printf("Draw user data: vs %016llx %u vertices:", static_cast<unsigned long long>(ud_log),
                        num_indices);
            for (u32 i = 0; i < 16; ++i) {
                std::printf(" %x", ud[i]);
            }
            std::printf("\n");
            // The first records of the buffer whose V# sits in user data 8..11, as the CPU sees them.
            const VAddr base = ud[8] | (u64(ud[9] & 0xffff) << 32);
            const auto* words = reinterpret_cast<const u32*>(base);
            std::printf("  buffer %#llx:", static_cast<unsigned long long>(base));
            for (u32 i = 0; i < 32; ++i) {
                std::printf(" %08x", words[i]);
            }
            std::printf("\n");
        }
    }
    const DrawLabel label = MakeDrawLabel(
        "draw", vs_info.pgm_hash,
        pipeline->GetStages()[u32(Shader::SwStage::Fragment)]
            ? pipeline->GetStages()[u32(Shader::SwStage::Fragment)]->pgm_hash
            : 0,
        num_indices, num_instances);
    // tlg (macOS): TLG_MAC_QUAD_INDEX: a quad list is drawn as triangles through the index pattern
    // (0,1,2, 0,2,3 a quad); the vertex index the shader sees stays the original one.
    const bool quad_index = pipeline->GetGraphicsKey().prim_type == AmdGpu::PrimitiveType::QuadList &&
                            LiverpoolToVK::MacQuadIndex();
    if (quad_index && is_indexed) {
        static std::atomic<u64> dropped{0};
        if ((dropped.fetch_add(1, std::memory_order_relaxed) & 1023) == 0) {
            std::printf("Quad index: indexed quad-list draws dropped so far: %llu\n",
                        static_cast<unsigned long long>(dropped.load()));
        }
        ResetBindings(false);
        return;
    }
    const vk::Buffer quad_indices = quad_index ? QuadIndexBuffer() : vk::Buffer{};
    const u32 num_quads = quad_index ? std::min<u32>(num_indices / 4, MaxPatternQuads) : 0;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        InsertLabel(cmdbuf, label);
        const auto draw = [&] {
            if (quad_indices) {
                cmdbuf.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
                cmdbuf.bindIndexBuffer(quad_indices, 0, vk::IndexType::eUint32);
                cmdbuf.drawIndexed(num_quads * 6, num_instances, 0, s32(first_vertex), first_instance);
            } else if (is_indexed) {
                cmdbuf.drawIndexed(num_indices, num_instances, 0, s32(first_vertex),
                                   first_instance);
            } else {
                cmdbuf.draw(num_indices, num_instances, first_vertex, first_instance);
            }
        };
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, handle);
        draw();
        if (square) [[unlikely]] {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, square);
            draw();
        }
    });
    if (FrameCapture::Active()) {
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        FrameCapture::Draw(vs_info.pgm_hash, ps ? ps->pgm_hash : 0, num_indices, num_instances);
        if (const auto& bc = regs.blend_control[0]; bc.enable && regs.color_buffers[0]) {
            char note[128];
            std::snprintf(note, sizeof(note), "\n  blend ps %08x idx %u: src %u dst %u func %u z %u%u",
                          u32(ps ? ps->pgm_hash : 0), num_indices, u32(bc.color_src_factor),
                          u32(bc.color_dst_factor), u32(bc.color_func),
                          u32(regs.depth_control.depth_enable),
                          u32(regs.depth_control.depth_write_enable));
            FrameCapture::Note(note);
        }
    }
    if (draw_timed) {
        scheduler.EndRendering();
        scheduler.Finish();
        const auto* vs_stage = pipeline->GetStages()[u32(Shader::SwStage::Vertex)];
        const auto* ps_stage = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        DrawTimer().Add(vs_stage ? vs_stage->pgm_hash : 0, ps_stage ? ps_stage->pgm_hash : 0,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                  draw_timed_start).count());
    }
    DebugState.IncDrawCall();

    ResetBindings(false);
    scheduler.KickRecording();
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset) {
    RENDERER_TRACE;
    // tlg: like direct draws, handed to the recording thread after the pipeline selection
    // (else the GPU thread waited here for a whole frame of queued draws).
    const bool pipelined = UseDrawPipe() && FilterDrawPasses() &&
                           !TlgToggle::Disabled(TlgToggle::PipelinedIndirectDraws);
    if (!pipelined) {
        DrainDrawPipe();
        scheduler.PopPendingOperations();
        if (!FilterDraw()) {
            return;
        }
    }

    // tlg (i3-pc 7036810): arguments that no real draw comes near make the GPU hang until the
    // driver resets the device (an instance count in the billions from unrelated float data). Every
    // slot of a multi draw is checked, not just the first, when the arguments live in plain guest
    // memory; slots the GPU wrote (compute) are not in guest memory yet and are left to the GPU.
    // TLG_INDIRECT_CHECK=0 turns the check off. The i3-pc also draws from the checked copy even for
    // GPU-written slots (particle counts a frame old): not ported, it changes what is drawn.
    static const bool check_args = [] {
        const char* value = std::getenv("TLG_INDIRECT_CHECK");
        return !value || value[0] != '0';
    }();
    if (check_args && count_address == 0 && stride >= 16 && max_count != 0 &&
        !buffer_cache.IsRegionGpuModified(arg_address + offset, u64(stride) * max_count)) {
        constexpr u32 max_elements = 0x400000;
        constexpr u32 max_instances = 0x10000;
        constexpr u32 max_slots = 256;
        for (u32 slot = 0; slot < std::min(max_count, max_slots); ++slot) {
            std::array<u32, 2> args{};
            memory->CopySparseMemory(arg_address + offset + u64(slot) * stride,
                                     reinterpret_cast<u8*>(args.data()), sizeof(args));
            if (args[0] > max_elements || args[1] > max_instances) {
                static std::atomic<u32> dropped{0};
                if (dropped.fetch_add(1, std::memory_order_relaxed) < 32) {
                    LOG_WARNING(Render,
                                "tlg: dropped indirect draw at {:#x} slot {}: count {:#x}, "
                                "instances {:#x}",
                                arg_address + offset, slot, args[0], args[1]);
                }
                return;
            }
        }
    }
    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
        .is_indirect = true, // tlg: BP07, BP11
    };
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline(params);
    if (!pipeline) {
        return;
    }
    const IndirectDraw indirect = {arg_address + offset, count_address, stride, max_count};
    if (pipelined) {
        PostDraw(pipeline, nullptr, is_indexed, 0, nullptr, &indirect);
        return;
    }
    DrawIndirectRecord(pipeline, is_indexed, indirect);
}

namespace {
// tlg: TLG_INDIRECT_STATS=1: every 5 s, per vertex shader: indirect draws, and the records' vertex
// and instance counts as guest memory shows them when the draw is recorded (GPU-written counts of
// an earlier frame in unified memory). Compares particle systems between runs, whatever the camera.
void IndirectStats(u64 vs, u32 prim, bool indexed, VAddr args, u32 max_count, bool mapped) {
    static const bool on = std::getenv("TLG_INDIRECT_STATS") != nullptr;
    if (!on) {
        return;
    }
    struct Entry {
        u64 draws = 0, verts = 0, insts = 0, zero = 0;
        u32 prim = 0;
        bool indexed = false;
    };
    static std::mutex mutex;
    static std::map<u64, Entry> entries;
    static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::scoped_lock lk{mutex};
    auto& e = entries[vs];
    ++e.draws;
    e.prim = prim;
    e.indexed = indexed;
    if (mapped && max_count > 0) {
        const auto* w = reinterpret_cast<const u32*>(args);
        e.verts += w[0];
        e.insts += w[1];
        e.zero += (w[0] == 0 || w[1] == 0) ? 1 : 0;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        next = now + std::chrono::seconds(5);
        std::printf("Indirect stats (5 s):\n");
        for (const auto& [hash, x] : entries) {
            std::printf("  vs %08llx prim %u %s: %llu draws, avg %llu verts x %llu inst, %llu empty\n",
                        (unsigned long long)hash, x.prim, x.indexed ? "indexed" : "plain",
                        (unsigned long long)x.draws, (unsigned long long)(x.verts / std::max<u64>(x.draws, 1)),
                        (unsigned long long)(x.insts / std::max<u64>(x.draws, 1)), (unsigned long long)x.zero);
        }
        entries.clear();
    }
}
} // namespace

void Rasterizer::DrawIndirectRecord(const GraphicsPipeline* pipeline, bool is_indexed,
                                    const IndirectDraw& indirect) {
    CountTessDraw(pipeline, true);
#ifdef __APPLE__
    // tlg (macOS, diagnostic): TLG_MAC_SKIP_RECT_INDIRECT=1 drops indirect rect/quad-list draws
    // (GPU particles), to tell whether MoltenVK's tessellation emulation of them breaks frames.
    static const bool skip_rect_indirect = [] {
        const char* env = std::getenv("TLG_MAC_SKIP_RECT_INDIRECT");
        return env && *env == '1';
    }();
    // TLG_MAC_SKIP_VS=<hex>[,<hex>...] (diagnostic): drops indirect draws of these vertex shaders.
    static const std::vector<u64> skip_vs = [] {
        std::vector<u64> list;
        if (const char* env = std::getenv("TLG_MAC_SKIP_VS")) {
            for (const char* p = env; *p;) {
                char* end = nullptr;
                const u64 v = std::strtoull(p, &end, 16);
                if (end == p) {
                    break;
                }
                list.push_back(v);
                p = *end == ',' ? end + 1 : end;
            }
        }
        return list;
    }();
    if (!skip_vs.empty() &&
        std::ranges::find(skip_vs, pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash) != skip_vs.end()) {
        return;
    }
    IndirectStats(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                  u32(pipeline->GetGraphicsKey().prim_type), is_indexed, indirect.args, indirect.max_count,
                  memory->IsValidGpuMapping(indirect.args, 0));
    // TLG_MAC_INDIRECT_TRACE=<file> (diagnostic): one line per indirect draw of TLG_MAC_INDIRECT_LOG's
    // vertex shader: time, fragment shader, args address, the vertex count the CPU sees now, and the
    // previous draw's count read again now (it differs if the GPU was still writing it).
    static FILE* const indirect_trace = [] {
        const char* env = std::getenv("TLG_MAC_INDIRECT_TRACE");
        return env && *env ? std::fopen(env, "w") : nullptr;
    }();
    static const u64 indirect_trace_vs = [] {
        const char* env = std::getenv("TLG_MAC_INDIRECT_LOG");
        return env && *env ? std::strtoull(env, nullptr, 16) : 0ull;
    }();
    if (indirect_trace && pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash == indirect_trace_vs &&
        memory->IsValidGpuMapping(indirect.args, 0)) {
        static VAddr prev_args = 0;
        static u32 prev_seen = 0;
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        const u32 seen = *reinterpret_cast<const u32*>(indirect.args);
        const auto& trace_cb = Regs().color_buffers[0];
        const auto& trace_ud = Regs().vs_program.user_data;
        std::fprintf(indirect_trace, "%lld %llx %llx %u %u %u %u %u %llx %u ud10 %x ud11 %x\n", static_cast<long long>(now_ms),
                     ps ? static_cast<unsigned long long>(ps->pgm_hash) : 0ull,
                     static_cast<unsigned long long>(indirect.args), indirect.max_count, seen,
                     reinterpret_cast<const u32*>(indirect.args)[1], prev_seen,
                     prev_args ? *reinterpret_cast<const u32*>(prev_args) : 0u,
                     trace_cb ? static_cast<unsigned long long>(trace_cb.Address()) : 0ull,
                     trace_cb ? u32(trace_cb.Pitch()) : 0u, trace_ud[10], trace_ud[11]);
        std::fflush(indirect_trace);
        prev_args = indirect.args;
        prev_seen = seen;
    }
    // TLG_MAC_INDIRECT_LOG=<hex vs hash> (diagnostic): the first 40 indirect draws of that vertex
    // shader: count, stride, the records as the CPU sees them, and its user data.
    static const u64 indirect_log = [] {
        const char* env = std::getenv("TLG_MAC_INDIRECT_LOG");
        return env && *env ? std::strtoull(env, nullptr, 16) : 0ull;
    }();
    if (indirect_log != 0 && pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash == indirect_log) {
        static std::atomic<u32> logged{0};
        // TLG_MAC_INDIRECT_LOG_SKIP=<n>: the 40 logged draws start after n draws.
        static const u32 log_skip = [] {
            const char* env = std::getenv("TLG_MAC_INDIRECT_LOG_SKIP");
            return env && *env ? u32(std::atoi(env)) : 0u;
        }();
        const u32 log_index = logged.fetch_add(1, std::memory_order_relaxed);
        if (log_index >= log_skip && log_index < log_skip + 40) {
            std::printf("Indirect log: vs %llx max_count %u stride %u count_addr %#llx args %#llx:",
                        static_cast<unsigned long long>(indirect_log), indirect.max_count, indirect.stride,
                        static_cast<unsigned long long>(indirect.count),
                        static_cast<unsigned long long>(indirect.args));
            if (memory->IsValidGpuMapping(indirect.args, 0)) {
                const auto* w = reinterpret_cast<const u32*>(indirect.args);
                for (u32 r = 0; r < std::min<u32>(indirect.max_count, 4); ++r) {
                    std::printf(" [%u %u %u %u]", w[r * indirect.stride / 4], w[r * indirect.stride / 4 + 1],
                                w[r * indirect.stride / 4 + 2], w[r * indirect.stride / 4 + 3]);
                }
            }
            const auto& r = Regs();
            for (u32 cb = 0; cb < 2; ++cb) {
                const auto& c = r.color_buffers[cb];
                if (c) {
                    std::printf(" cb%u %#llx %ux%u fmt %u/%u blend %u", cb,
                                static_cast<unsigned long long>(c.Address()), c.Pitch(), c.Height(),
                                u32(c.GetDataFmt()), u32(c.GetNumberFmt()), u32(r.blend_control[cb].enable));
                }
            }
            std::printf(" z %#llx zwrite %llx dctl test %u write %u", 
                        static_cast<unsigned long long>(r.depth_buffer.DepthAddress()),
                        static_cast<unsigned long long>(r.depth_buffer.DepthWriteAddress()),
                        u32(r.depth_control.depth_enable), u32(r.depth_control.depth_write_enable));
            if (const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)]) {
                std::printf(" ps %llx tex:", static_cast<unsigned long long>(ps->pgm_hash));
                for (const auto& image_desc : ps->images) {
                    const auto tsharp = image_desc.GetSharp(*ps);
                    std::printf(" %#llx", static_cast<unsigned long long>(tsharp.Address()));
                }
            }
            const auto& ud = Regs().vs_program.user_data;
            std::printf(" ud:");
            for (u32 i = 0; i < 16; ++i) {
                std::printf(" %x", ud[i]);
            }
            // The buffer at user data 12-13 as floats (particle records), at the start and deeper.
            const VAddr data = (VAddr(ud[13] & 0xffff) << 32) | ud[12];
            if (memory->IsValidGpuMapping(data, 0x1100)) {
                for (const u32 at : {0u, 0x1000u}) {
                    std::printf("\n  data+%x:", at);
                    const auto* f = reinterpret_cast<const float*>(data + at);
                    for (u32 i = 0; i < 24; ++i) {
                        std::printf(" %.3g", f[i]);
                    }
                }
            }
            std::printf("\n");
            std::fflush(stdout);
        }
    }
    if (skip_rect_indirect) {
        const auto prim = pipeline->GetGraphicsKey().prim_type;
        if (prim == AmdGpu::PrimitiveType::RectList || prim == AmdGpu::PrimitiveType::QuadList) {
            return;
        }
    }
#endif
    if (DrawPipe::OnStageB()) {
        FrameCapture::Poll();
        scheduler.PopPendingOperations();
    }
    const VAddr count_address = indirect.count;
    const u32 stride = indirect.stride;
    const u32 max_count = indirect.max_count;

    // Indirect arguments may be GPU-written: leave these draws on camera fallback.
    motion_draw = false;
    motion_geometry = 0;
    PrepareRenderState(pipeline);
    if (upscaler->Enabled() && std::popcount(pipeline->GetGraphicsKey().mrt_mask) == 1) {
        const auto& viewport = Regs().viewports[0];
        upscaler->OnDraw(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                         cb_descs[0].first, db_desc.first,
                         UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2));
    }
    if (!BindResources(pipeline)) {
        return;
    }
    // tlg (macOS): TLG_MAC_QUAD_INDEX: the records of an indirect quad list are rewritten on the GPU
    // into indexed ones for the quad index pattern, before the render pass (QuadIndirectArgs).
    const bool quad_index = pipeline->GetGraphicsKey().prim_type == AmdGpu::PrimitiveType::QuadList &&
                            LiverpoolToVK::MacQuadIndex();
    vk::Buffer quad_args{};
    u64 quad_args_offset = 0;
    vk::Buffer quad_indices{};
    u64 quad_indices_offset = 0;
    bool quad_strip = false;
    if (quad_index) {
        if (is_indexed) {
            // Indexed quad lists: their indices are expanded to triangles on the GPU. A single
            // quad (one record, a 4-index buffer: ISS's particles, ~3,000 a second in the fire)
            // draws as a strip a,b,d,c with the game's own record instead: no compute, so the
            // render pass stays open.
            ResolveIndexBuffer(0);
            const bool is16 = index_bind.type == vk::IndexType::eUint16;
            if (max_count == 1 && !indirect.count && index_buffer_bytes == 4u * (is16 ? 2u : 4u)) {
                const VAddr base = Regs().index_base_address.Address<VAddr>();
                u32 q[4];
                for (u32 i = 0; i < 4; ++i) {
                    q[i] = is16 ? reinterpret_cast<const u16*>(base)[i]
                                : reinterpret_cast<const u32*>(base)[i];
                }
                const auto strip = runtime.GetStagingPool().Request(16, VideoCore::MemoryType::HostUncached, 16);
                const u32 order[4] = {q[0], q[1], q[3], q[2]};
                std::memcpy(strip.mapped, order, sizeof(order));
                strip.Flush();
                quad_strip = true;
                quad_indices = strip.buffer->Handle();
                quad_indices_offset = strip.offset;
            } else {
                std::tie(quad_args, quad_args_offset, quad_indices, quad_indices_offset) =
                    QuadIndexedIndirect(indirect.args, stride, max_count);
            }
            if (!quad_args && !quad_strip) {
                static std::atomic<u64> dropped{0};
                if ((dropped.fetch_add(1, std::memory_order_relaxed) & 1023) == 0) {
                    std::printf("Quad index: oversized indexed quad-list draws dropped so far: %llu\n",
                                static_cast<unsigned long long>(dropped.load()));
                }
                ResetBindings(false);
                return;
            }
        } else {
            std::tie(quad_args, quad_args_offset) = QuadIndirectArgs(indirect.args, stride, max_count);
            quad_indices = QuadIndexBuffer();
        }
    }
    const auto state = BeginRendering(pipeline);
    // tlg: before anything of the draw itself is recorded, which has to go with it.
    SubmitIfWorkPiledUp(state);
    scheduler.CountWork();
    // Tessellation on the host: the game's own, or the auxiliary shaders that draw rect and quad
    // lists without geometry shaders (vk_graphics_pipeline.cpp).
    const auto prim_type = pipeline->GetGraphicsKey().prim_type;
    const bool tess_like = pipeline->GetStages()[u32(Shader::SwStage::TessellationControl)] ||
        prim_type == AmdGpu::PrimitiveType::RectList ||
        (prim_type == AmdGpu::PrimitiveType::QuadList && !LiverpoolToVK::MacQuadIndex());
    if (tess_like) {
        scheduler.CountTessIndirect();
#ifdef __APPLE__
        LimitTessIndirectInFlight();
#endif
    }

    BindVertexBuffers(pipeline);
    if (is_indexed) {
#ifdef __APPLE__
        if (tess_like) {
            // tlg (macOS): MoltenVK sizes its tessellation scratch for an indexed indirect draw
            // from the index buffer's remaining length: an index range inside the 5 GiB guest
            // buffer (zero-copy) made it allocate 4 GiB, four times. The range is copied on the
            // GPU into a staging slice (at most a 16 MiB block left) and bound from there.
            ResolveIndexBuffer(0);
            const u64 remaining = index_buffer_obj ? index_buffer_obj->SizeBytes() - index_bind.offset : 0;
            if (remaining > (64ull << 20) && index_buffer_bytes != 0) {
                const auto slice = runtime.GetStagingPool().Request(index_buffer_bytes,
                                                                    VideoCore::MemoryType::DeviceLocal);
                const vk::BufferCopy copy{.srcOffset = index_bind.offset,
                                          .dstOffset = slice.offset,
                                          .size = index_buffer_bytes};
                runtime.CopyBuffer(index_buffer_obj, slice.buffer, std::span{&copy, 1});
                needs_barrier |= runtime.IsBufferAccessed(slice.buffer, slice.offset, index_buffer_bytes);
                index_bind.handle = slice.buffer->Handle();
                index_bind.offset = slice.offset;
            }
            EmitIndexBuffer();
        } else {
            BindIndexBuffer();
        }
#else
        BindIndexBuffer();
#endif
    }

    const auto [buffer, base] =
        buffer_cache.ObtainBuffer(indirect.args, stride * max_count, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, stride * max_count);
    // tlg: BP10 (shadPS4 c09fa46a, #5262) record the fetch so a later writer gets a barrier.
    if (Common::Backports::On(Common::Backports::Id::VertexIndexBarriers)) {
        Common::Backports::Hit(Common::Backports::Id::VertexIndexBarriers);
        bound_buffers.push_back({buffer, base, static_cast<u32>(stride * max_count), vk::AccessFlagBits2::eIndirectCommandRead});
    }

    const VideoCore::Buffer* count_buffer;
    u64 count_offset;
    if (count_address != 0) {
        std::tie(count_buffer, count_offset) = buffer_cache.ObtainBuffer(count_address, 4, false);
        needs_barrier |= runtime.IsBufferAccessed(count_buffer, count_offset, 4);
        // tlg: BP10 (shadPS4 c09fa46a, #5262) record the fetch so a later writer gets a barrier.
        if (Common::Backports::On(Common::Backports::Id::VertexIndexBarriers)) {
            Common::Backports::Hit(Common::Backports::Id::VertexIndexBarriers);
            bound_buffers.push_back({count_buffer, count_offset, static_cast<u32>(4), vk::AccessFlagBits2::eIndirectCommandRead});
        }
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    // tlg: screen-space (clip disabled) draws into the upscaler's output-size images.
    push_data.xscale *= target_scale[0];
    push_data.xoffset *= target_scale[0];
    push_data.yscale *= target_scale[1];
    push_data.yoffset *= target_scale[1];
    pipeline->BindResources(set_writes, set_write_index, push_data);
    draw_jitter = {};
    if (upscaler->Enabled() && db_desc.first && db_desc.first == camera_motion->Depth()) {
        draw_jitter = upscaler->Jitter();
    }
    UpdateDynamicState(pipeline, is_indexed);
    MarkPass(pipeline, state);
    if (FrameRecord::IsRecording()) [[unlikely]] {
        RecordFrameInputs(pipeline, nullptr, &state, is_indexed, 0, &indirect);
    }
    scheduler.BeginRendering(state);
    if (!pass_label.empty()) [[unlikely]] {
        scheduler.Record([label = std::move(pass_label)](vk::CommandBuffer cmdbuf) {
            cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = label.c_str()});
        });
        pass_label.clear();
    }
    CountDraw(state, pipeline);

    ASSERT(stride == (is_indexed ? sizeof(VkDrawIndexedIndirectCommand)
                                 : sizeof(VkDrawIndirectCommand)));
    const vk::Pipeline handle = pipeline->Handle();
    const vk::Pipeline square = SquarePass(*pipeline);
    const vk::Buffer args = buffer->Handle();
    const u64 args_offset = base;
    if (DispatchLogThreshold() != 0) {
        u32 first[2];
        std::memcpy(first, reinterpret_cast<const void*>(indirect.args), sizeof(first));
        if (indirect.max_count > 64) {
            static std::atomic<u32> big_logged{0};
            if (big_logged.fetch_add(1, std::memory_order_relaxed) < 32) {
                std::printf("GPU work: indirect draw with max_count %u (count buffer %s, stride %u)\n",
                            indirect.max_count, indirect.count ? "yes" : "no", indirect.stride);
            }
        }
        const auto prim = pipeline->GetGraphicsKey().prim_type;
        const bool tess = pipeline->GetStages()[u32(Shader::SwStage::TessellationControl)] ||
                          prim == AmdGpu::PrimitiveType::RectList || prim == AmdGpu::PrimitiveType::QuadList;
        const bool gpu_written = buffer_cache.IsRegionGpuModified(indirect.args, 16);
        LogWork(tess ? (gpu_written ? "indirect tess draw (GPU-written) vs" : "indirect tess draw (CPU) vs")
                     : (is_indexed ? "indirect indexed draw vs" : "indirect draw vs"),
                pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash, first[0], first[1], indirect.max_count);
    }
    const vk::Buffer counts = count_address != 0 ? count_buffer->Handle() : vk::Buffer{};
    const u64 counts_offset = count_address != 0 ? count_offset : 0;
    const DrawLabel label = MakeDrawLabel(
        "indirect draw", pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
        pipeline->GetStages()[u32(Shader::SwStage::Fragment)]
            ? pipeline->GetStages()[u32(Shader::SwStage::Fragment)]->pgm_hash
            : 0,
        indirect.max_count, 0);
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        InsertLabel(cmdbuf, label);
        for (const vk::Pipeline bound : {handle, square}) {
            if (!bound) {
                break;
            }
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, bound);
            if (quad_strip) {
                cmdbuf.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleStrip);
                cmdbuf.bindIndexBuffer(quad_indices, quad_indices_offset, vk::IndexType::eUint32);
                cmdbuf.drawIndexedIndirect(args, args_offset, max_count, stride);
            } else if (quad_indices) {
                constexpr u32 record = sizeof(vk::DrawIndexedIndirectCommand);
                cmdbuf.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
                cmdbuf.bindIndexBuffer(quad_indices, quad_indices_offset, vk::IndexType::eUint32);
                if (counts) {
                    cmdbuf.drawIndexedIndirectCount(quad_args, quad_args_offset, counts, counts_offset,
                                                    max_count, record);
                } else {
                    cmdbuf.drawIndexedIndirect(quad_args, quad_args_offset, max_count, record);
                }
            } else if (is_indexed) {
                if (counts) {
                    cmdbuf.drawIndexedIndirectCount(args, args_offset, counts, counts_offset, max_count,
                                                    stride);
                } else {
                    cmdbuf.drawIndexedIndirect(args, args_offset, max_count, stride);
                }
            } else if (counts) {
                cmdbuf.drawIndirectCount(args, args_offset, counts, counts_offset, max_count, stride);
            } else {
                cmdbuf.drawIndirect(args, args_offset, max_count, stride);
            }
        }
    });
    DebugState.IncDrawCall();

    ResetBindings(false);
    scheduler.KickRecording();
}

bool Rasterizer::AcbUnordered() {
    // tlg: the dispatches between two sync packets of the game's compute queue (RELEASE_MEM,
    // WAIT_REG_MEM, ...) are not ordered by buffer barriers, as on GCN: ISS issues ~95 particle
    // dispatches in a row without one, and the hazard barriers serialized them (with MoltenVK's
    // concurrent compute encoders, MVK_TLG_CONCURRENT_DISPATCH, they now overlap). TLG_ACB_UNORDERED=0:
    // as before.
    static const bool on = [] {
        const char* v = std::getenv("TLG_ACB_UNORDERED");
        return !(v && *v == '0');
    }();
    return on;
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;
    TlgStats::dispatches.fetch_add(1, std::memory_order_relaxed);
    const u32 flags = std::exchange(next_dispatch_flags, 0u);
    // tlg: like draws, handed to the draw recording thread after the pipeline selection.
    const bool pipelined = UseDrawPipe() && !TlgToggle::Disabled(TlgToggle::PipelinedDispatch);
    if (!pipelined) {
        DrainDrawPipe();
    }
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    if (pipelined) {
        PostDraw(pipeline, nullptr, false, flags, &CsRegs());
        return;
    }
    dispatch_flags = flags;
    DispatchRecord(pipeline);
}

#ifdef __APPLE__
void Rasterizer::LimitTessIndirectInFlight() {
    // tlg (macOS): each tessellated indirect draw holds MoltenVK scratch buffers (64-128 MiB)
    // until the GPU completes it, and its pool keeps the peak: while the GPU ran ~100 ms frames
    // the recorded backlog grew it to 30 GB. With more than TLG_MAC_TESS_IN_FLIGHT (default 2,
    // 0: off) of them outstanding, this waits for the oldest one's submission to complete.
    static const u32 in_flight = [] {
        const char* env = std::getenv("TLG_MAC_TESS_IN_FLIGHT");
        return env && *env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 2u;
    }();
    if (in_flight == 0) {
        return;
    }
    tess_indirect_ticks.push_back(scheduler.CurrentTick());
    while (tess_indirect_ticks.size() > in_flight) {
        const u64 tick = tess_indirect_ticks.front();
        if (tick >= scheduler.CurrentTick()) {
            break; // still being recorded: SubmitIfWorkPiledUp ends the submission at a pass
        }
        tess_indirect_ticks.pop_front();
        if (!scheduler.IsFree(tick)) {
            scheduler.Wait(tick);
        }
    }
}
#endif

void Rasterizer::SubmitIfWorkPiledUp(const RenderState& next_state) {
    // tlg: commands were submitted only when a guest submission ended or something had to wait,
    // so the GPU idled while thousands of draws were recorded and then had all of them to do,
    // and whatever waited on the GPU (ISS' threads wait for readbacks hundreds of times a
    // second) waited for all of it. Once TLG_SUBMIT_DRAWS (default 1024, 0: off) draws and
    // dispatches are recorded, they are submitted where the next draw starts a render pass
    // anyway, so the GPU works on them while the rest is recorded.
    // macOS: 128 (benchmark, cannery interior idle: 1024 13.9 FPS, 256 21.9, 128 22.0, 32 22.1;
    // cutscene 22 -> 31): with MoltenVK the GPU otherwise waits while most of a frame is recorded.
    // 64 since submits are asynchronous (MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0): 128 27.0,
    // 64 28.0, 32 28.2.
    static const u32 limit = [] {
        const char* env = std::getenv("TLG_SUBMIT_DRAWS");
#ifdef __APPLE__
        constexpr u32 default_limit = 64;
#else
        constexpr u32 default_limit = 1024;
#endif
        return env && *env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : default_limit;
    }();
#ifdef __APPLE__
    // macOS: MoltenVK emulates tessellation with compute and, unable to read indirect
    // arguments, gives every tessellated indirect draw scratch buffers for its maximum vertex
    // count (64-128 MiB each, kMVKMaxDrawIndirectVertexCount) from a pool that keeps its peak:
    // hundreds per submission (GPU-driven rect lists, drawn with auxiliary tessellation shaders)
    // made a 30 GB pool in the graffiti scene. A submission ends once it has
    // TLG_MAC_TESS_PER_SUBMIT (default 1, 0: off) of them, inside a render pass too.
    static const u32 tess_limit = [] {
        const char* env = std::getenv("TLG_MAC_TESS_PER_SUBMIT");
        return env && *env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 1u;
    }();
    const bool tess_piled_up = tess_limit != 0 && scheduler.TessIndirectSinceSubmit() >= tess_limit;
#else
    constexpr bool tess_piled_up = false;
#endif
    if (!tess_piled_up && (limit == 0 || scheduler.WorkSinceSubmit() < limit ||
                           !scheduler.WillBeginRendering(next_state))) {
        return;
    }
    // The submission waits for the recording thread, which may be waiting for a compute
    // pipeline being built (ComputeBind): the draw pipe would stall behind the build.
    if (Pipeline::deferred_binds.load(std::memory_order_relaxed) != 0) {
        return;
    }
    // The frame recorder and capture read back what each submission did: left as they were.
    if (FrameRecord::IsRecording() || FrameCapture::Active()) {
        return;
    }
    if ((++work_submits & 255) == 1) {
        std::printf("GPU: %llu submissions of piled-up draws so far (TLG_SUBMIT_DRAWS=%u)\n",
                    static_cast<unsigned long long>(work_submits), limit);
    }
    scheduler.Flush();
}

Rasterizer::ComputeBinder Rasterizer::ComputeBind(const ComputePipeline* pipeline) {
    if (pipeline->IsBuilt()) {
        return {pipeline->Handle(), nullptr};
    }
    // tlg: still being built on a compiler thread (vk_compute_pipeline.cpp): the thread that
    // records the command into Vulkan waits for it, the GPU thread goes on.
    Pipeline::deferred_binds.fetch_add(1, std::memory_order_relaxed);
    return {vk::Pipeline{}, pipeline};
}

void Rasterizer::ComputeBinder::operator()(vk::CommandBuffer cmdbuf) const {
    if (pending) [[unlikely]] {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pending->Handle());
        Pipeline::deferred_binds.fetch_sub(1, std::memory_order_relaxed);
        return;
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
}

namespace {
// tlg: TLG_DISPATCH_TIMING=1: every 10 s the next 800 dispatches (about a frame) run one at a time,
// each waited for, and the GPU time per compute shader is printed (MoltenVK's timestamps inside
// encoders read 0, and Metal System Trace labels every compute encoder alike).
struct DispatchTiming {
    bool enabled = [] {
        const char* env = std::getenv("TLG_DISPATCH_TIMING");
        return env && *env == '1';
    }();
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();
    u32 left = 0;
    std::unordered_map<u64, std::pair<double, u32>> per_shader;

    bool Begin() {
        if (!enabled) {
            return false;
        }
        if (left == 0) {
            if (!TimingAllowed() || std::chrono::steady_clock::now() < next) {
                return false;
            }
            left = 800;
            per_shader.clear();
        }
        return true;
    }
    void Add(u64 hash, double ms) {
        auto& [total, count] = per_shader[hash];
        total += ms;
        ++count;
        if (--left != 0) {
            return;
        }
        next = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::vector<std::pair<u64, std::pair<double, u32>>> sorted(per_shader.begin(), per_shader.end());
        std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        double all = 0;
        for (const auto& entry : sorted) {
            all += entry.second.first;
        }
        std::printf("Dispatch timing: 800 dispatches, %.1f ms of GPU (with submission overhead)\n", all);
        for (size_t i = 0; i < std::min<size_t>(sorted.size(), 12); ++i) {
            std::printf("  cs %016llx: %7.2f ms in %4u dispatches\n",
                        static_cast<unsigned long long>(sorted[i].first), sorted[i].second.first,
                        sorted[i].second.second);
        }
    }
};
DispatchTiming& Timing() {
    static DispatchTiming timing;
    return timing;
}
} // namespace

Scheduler* Rasterizer::AcbTarget(const Shader::Info& cs, u32 flags, u64 main_records) {
    if (!acb_scheduler || !(flags & 1u)) {
        return nullptr;
    }
    static std::atomic<u64> routed{0}, kept{0}, main_waits{0};
    static const bool stats = [] {
        const char* v = std::getenv("TLG_ACB_PACKETS");
        return v && *v == '1';
    }();
    if (stats) [[unlikely]] {
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(5)) {
            last = now;
            std::printf("ACB queue (5 s): %llu dispatches there, %llu on the main queue, %llu waits for main\n",
                        (unsigned long long)routed.exchange(0), (unsigned long long)kept.exchange(0),
                        (unsigned long long)main_waits.exchange(0));
        }
    }
    static const bool images_there = [] {
        const char* v = std::getenv("TLG_ACB_IMAGES");
        return !v || *v != '0';
    }();
    const bool eligible = buffer_cache.UnifiedAlias() && (images_there || bound_images.empty()) && !cs.uses_dma &&
                          !FrameCapture::Active() && !FrameRecord::IsRecording() &&
                          !Scheduler::FullBarriers();
    if (!eligible) {
        kept.fetch_add(1, std::memory_order_relaxed);
        {
            std::scoped_lock lock{acb_scheduler->CompanionMutex()};
            AcbFlushCopies();
        }
        acb_needs_main = true;
        if (stats) [[unlikely]] {
            static std::set<u64> seen;
            if (seen.insert(cs.pgm_hash).second) {
                std::printf("ACB queue: cs %016llx stays on the main queue: images %zu dma %d\n",
                            (unsigned long long)cs.pgm_hash, bound_images.size(), int(cs.uses_dma));
            }
        }
        return nullptr;
    }
    routed.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock{acb_scheduler->CompanionMutex()};
    AcbFlushCopies();
    // A binding that recorded on the main scheduler (an upload) must run first.
    if (acb_needs_main || scheduler.RecordCount() != main_records) {
        main_waits.fetch_add(1, std::memory_order_relaxed);
        if (stats) [[unlikely]] {
            static std::atomic<u64> by_flag{0}, by_records{0};
            (acb_needs_main ? by_flag : by_records).fetch_add(1, std::memory_order_relaxed);
            if (((by_flag + by_records) & 255) == 0) {
                std::printf("ACB queue: main waits: flag %llu, recorded in binding %llu\n",
                            (unsigned long long)by_flag.load(), (unsigned long long)by_records.load());
            }
        }
        AcbWaitMain();
    }
    return acb_scheduler.get();
}

bool Rasterizer::AcbFillGds(u32 offset, u32 num_bytes, u32 value) {
    if (!acb_scheduler || !buffer_cache.UnifiedAlias()) {
        return false;
    }
    std::scoped_lock lock{acb_scheduler->CompanionMutex()};
    AcbFlushCopies();
    if (acb_needs_main) {
        AcbWaitMain();
    }
    const vk::Buffer gds = buffer_cache.GetGdsBuffer()->Handle();
    acb_scheduler->Record([=](vk::CommandBuffer cmdbuf) {
        cmdbuf.fillBuffer(gds, offset, num_bytes, value);
    });
    // The next dispatches, in another encoder of the same queue: ordered by submission.
    acb_scheduler->Flush();
    return true;
}

bool Rasterizer::AcbCopyGds(VAddr dst, u32 gds_offset, u32 num_bytes) {
    if (!acb_scheduler || !buffer_cache.UnifiedAlias() || !buffer_cache.IsUnified(dst, num_bytes)) {
        return false;
    }
    std::scoped_lock lock{acb_scheduler->CompanionMutex()};
    AcbFlushCopies();
    if (acb_needs_main) {
        AcbWaitMain();
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    VideoCore::written_binding_source = {"acb gds copy", 0};
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(dst, num_bytes, true);
    const vk::Buffer gds = buffer_cache.GetGdsBuffer()->Handle();
    const vk::Buffer alias = buffer_cache.UnifiedAlias();
    const u64 dst_offset = offset;
    // The dispatches that count, then the copy, then the dispatches that read it: submission order.
    acb_scheduler->Flush();
    acb_scheduler->Record([=](vk::CommandBuffer cmdbuf) {
        const vk::BufferCopy region{.srcOffset = gds_offset, .dstOffset = dst_offset, .size = num_bytes};
        cmdbuf.copyBuffer(gds, alias, region);
    });
    acb_scheduler->Flush();
    return true;
}

void Rasterizer::AcbFenceThread(std::stop_token stop) {
    Common::SetCurrentThreadName("tlg:AcbFences");
    const vk::Device device = instance.GetDevice();
    u32 spins = 0;
    while (!stop.stop_requested()) {
        AcbFence fence;
        {
            std::unique_lock lock{acb_fence_mutex};
            acb_fence_cv.wait(lock, stop, [this] { return !acb_fences.empty(); });
            if (stop.stop_requested()) {
                return;
            }
            // The front stays queued until done: the recording side only appends.
            if (device.getEventStatus(acb_fences.front().event) != vk::Result::eEventSet) {
                lock.unlock();
                if (++spins < 64) {
                    std::this_thread::yield();
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(20));
                }
                continue;
            }
            fence = std::move(acb_fences.front());
            acb_fences.pop_front();
        }
        spins = 0;
        fence.write();
        (void)device.resetEvent(fence.event);
        std::scoped_lock lock{acb_fence_mutex};
        acb_free_events.push_back(fence.event);
    }
}

void Rasterizer::AcbDeferFence(Common::UniqueFunction<void>&& write) {
    AcbFlushCopies();
    const vk::Device device = instance.GetDevice();
    vk::Event event{};
    {
        std::scoped_lock lock{acb_fence_mutex};
        if (!acb_free_events.empty()) {
            event = acb_free_events.back();
            acb_free_events.pop_back();
        }
    }
    if (!event) {
        const auto [result, created] = device.createEvent(vk::EventCreateInfo{});
        ASSERT_MSG(result == vk::Result::eSuccess, "createEvent: {}", vk::to_string(result));
        event = created;
    }
    acb_scheduler->Record([event](vk::CommandBuffer cmdbuf) {
        cmdbuf.setEvent(event, vk::PipelineStageFlagBits::eAllCommands);
    });
    acb_scheduler->MarkCompanionWork();
    {
        std::scoped_lock lock{acb_fence_mutex};
        acb_fences.push_back({event, std::move(write)});
    }
    acb_fence_cv.notify_one();
    // Stage B flushes when it runs out of packets (AcbIdle); under a steady stream of packets, every few
    // fences. TLG_ACB_FLUSH_FENCES (default 8), 1: a command buffer per fence.
    static const u32 flush_fences = [] {
        const char* v = std::getenv("TLG_ACB_FLUSH_FENCES");
        return v && *v ? u32(std::atoi(v)) : 8u;
    }();
    if (++acb_fences_since_flush >= flush_fences || !DrawPipe::OnStageB()) {
        acb_fences_since_flush = 0;
        acb_scheduler->Flush();
    }
}

void Rasterizer::AcbIdle(void* context) {
    auto* self = static_cast<Rasterizer*>(context);
    // TLG_GFX_FENCE_BATCH: graphics fences still in the open submission go out now that nothing
    // more is being recorded (the game may be waiting for one of them).
    if (self->gfx_fences_unflushed && self->gfx_fence_tick == self->scheduler.CurrentTick()) {
        self->gfx_fences_unflushed = 0;
        self->scheduler.Flush();
    }
    // TLG_IDLE_FLUSH=N (0, the default: off): with nothing more to record, the main queue's work
    // goes to the GPU once N draws/dispatches wait; TLG_IDLE_FLUSH_PASS=0: only outside a render
    // pass. The GPU otherwise idles until 64 draws or a fence end the submission.
    static const u32 idle_flush = [] {
        const char* v = std::getenv("TLG_IDLE_FLUSH");
        return v && *v ? u32(std::atoi(v)) : 0u;
    }();
    static const bool idle_in_pass = [] {
        const char* v = std::getenv("TLG_IDLE_FLUSH_PASS");
        return !v || *v != '0';
    }();
    if (idle_flush && self->scheduler.WorkSinceSubmit() >= idle_flush &&
        (idle_in_pass || !self->scheduler.IsRendering()) &&
        Pipeline::deferred_binds.load(std::memory_order_relaxed) == 0 && !FrameRecord::IsRecording() &&
        !FrameCapture::Active()) {
        self->scheduler.Flush();
    }
    if (!self->acb_scheduler || (self->acb_fences_since_flush == 0 && self->acb_copies.empty())) {
        return;
    }
    std::scoped_lock lock{self->acb_scheduler->CompanionMutex()};
    self->AcbFlushCopies();
    self->acb_fences_since_flush = 0;
    self->acb_scheduler->Flush();
}

bool Rasterizer::AcbCopyMemory(VAddr dst, VAddr src, u32 num_bytes) {
    if (!acb_scheduler || !buffer_cache.UnifiedAlias() || !buffer_cache.IsUnified(dst, num_bytes) ||
        !buffer_cache.IsUnified(src, num_bytes) || ((dst | src | num_bytes) & 3) != 0 || num_bytes == 0) {
        return false;
    }
    // As CopyBuffer: neither side was written by the GPU, so the copy needs no GPU work at all.
    if (!buffer_cache.IsRegionGpuModified(dst, num_bytes) &&
        !buffer_cache.IsRegionGpuModified(src, num_bytes) &&
        !texture_cache.FindImageFromRange(src, num_bytes)) {
        if (TlgSplit::Host()) {
            InvalidateMemory(dst, num_bytes);
        }
        std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
        return true;
    }
    const vk::Device device = instance.GetDevice();
    if (!acb_copy_pipeline) {
        const std::array<vk::DescriptorSetLayoutBinding, 1> bindings = {{
            {.binding = 0,
             .descriptorType = vk::DescriptorType::eStorageBuffer,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        acb_copy_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = 16 + 32 * sizeof(AcbCopy)};
        acb_copy_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*acb_copy_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto module = CompileSPV(ACB_COPY_COMP, device);
        acb_copy_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *acb_copy_layout,
                }));
        device.destroyShaderModule(module);
    }
    std::scoped_lock lock{acb_scheduler->CompanionMutex()};
    if (acb_needs_main) {
        AcbWaitMain();
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    VideoCore::written_binding_source = {"acb copy", 0};
    const auto [dst_buffer, dst_at] = buffer_cache.ObtainBuffer(dst, num_bytes, true);
    const auto [src_buffer, src_at] = buffer_cache.ObtainBuffer(src, num_bytes, false);
    if (((dst_at + num_bytes) >> 34) != 0 || ((src_at + num_bytes) >> 34) != 0) {
        return false; // beyond what a 32-bit dword index reaches
    }
    acb_copies.push_back({u32(src_at / 4), u32(dst_at / 4), num_bytes / 4, 0});
    if (acb_copies.size() == acb_copies.capacity()) {
        AcbFlushCopies();
    }
    return true;
}

void Rasterizer::AcbFlushCopies() {
    if (acb_copies.empty()) {
        return;
    }
    std::scoped_lock lock{acb_scheduler->CompanionMutex()};
    static std::atomic<u64> batches{0}, copies{0};
    static const bool stats = std::getenv("TLG_ACB_PACKETS") != nullptr;
    copies.fetch_add(acb_copies.size(), std::memory_order_relaxed);
    if (stats && (batches.fetch_add(1, std::memory_order_relaxed) & 8191) == 8191) [[unlikely]] {
        std::printf("ACB copies: %llu in %llu dispatches\n", (unsigned long long)copies.load(),
                    (unsigned long long)batches.load());
    }
    struct Constants {
        u32 num_copies;
        u32 pad[3];
        std::array<AcbCopy, 32> copies;
    } constants{};
    constants.num_copies = u32(acb_copies.size());
    std::copy(acb_copies.begin(), acb_copies.end(), constants.copies.begin());
    acb_copies.clear();
    const vk::Buffer alias = buffer_cache.UnifiedAlias();
    const vk::Pipeline pipeline = *acb_copy_pipeline;
    const vk::PipelineLayout layout = *acb_copy_layout;
    acb_scheduler->Record([=](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 mb{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &mb});
        const vk::DescriptorBufferInfo info{alias, 0, VK_WHOLE_SIZE};
        const vk::WriteDescriptorSet write{.dstBinding = 0,
                                           .descriptorCount = 1,
                                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                                           .pBufferInfo = &info};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, write);
        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0,
                             u32(16 + constants.num_copies * sizeof(AcbCopy)), &constants);
        cmdbuf.dispatch(1, 1, 1);
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &mb});
    });
    acb_scheduler->MarkCompanionWork();
}

void Rasterizer::AcbWaitMain() {
    acb_needs_main = false;
    scheduler.Flush();
    acb_scheduler->WaitNextSubmit(scheduler.GetWorkSemaphore()->Handle(), scheduler.CurrentTick() - 1);
}

void Rasterizer::AcbBind(Scheduler& acb, const ComputePipeline* pipeline, bool barrier) {
    needs_barrier = false;
    if (barrier) {
        acb.Record([](vk::CommandBuffer cmdbuf) {
            const vk::MemoryBarrier2 mb{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader |
                                vk::PipelineStageFlagBits2::eDrawIndirect,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
                                 vk::AccessFlagBits2::eIndirectCommandRead,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &mb});
        });
    }
    const vk::Buffer tracked = buffer_cache.UnifiedHandle();
    const vk::Buffer alias = buffer_cache.UnifiedAlias();
    for (auto& info : buffer_infos) {
        if (info.buffer == tracked) {
            info.buffer = alias;
        }
    }
    pipeline->BindResources(acb, set_writes, set_write_index, push_data);
}

void Rasterizer::DispatchRecord(const ComputePipeline* pipeline) {
    const bool timed = Timing().Begin();
    if (timed) {
        scheduler.EndRendering();
        scheduler.Finish();
    }
    const auto timed_start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    FrameCapture::Poll();
    gbuffer_draw = false;

    scheduler.PopPendingOperations();
    DiagPassBoundary(true);

    const auto& cs_program = CsRegs();

    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    // TLG_MAC_SKIP_CS=<hex> (diagnostic, breaks the picture): drops direct dispatches of that shader,
    // to bound what its dispatches cost.
    static const u64 skip_cs = [] {
        const char* env = std::getenv("TLG_MAC_SKIP_CS");
        return env && *env ? std::strtoull(env, nullptr, 16) : 0ull;
    }();
    if (skip_cs != 0 && cs.pgm_hash == skip_cs) [[unlikely]] {
        ResetBindings(true);
        return;
    }
    // TLG_CS_WATCH=<hex address>[,<hex address>...] (diagnostic): the first dispatch of each compute
    // shader whose buffers cover one of these addresses, and if it may write them.
    static const std::vector<VAddr> cs_watch = [] {
        std::vector<VAddr> list;
        if (const char* env = std::getenv("TLG_CS_WATCH")) {
            for (const char* p = env; *p;) {
                char* end = nullptr;
                const VAddr v = std::strtoull(p, &end, 16);
                if (end == p) {
                    break;
                }
                list.push_back(v);
                p = *end == ',' ? end + 1 : end;
            }
        }
        return list;
    }();
    if (!cs_watch.empty()) {
        static std::set<u64> watched;
        for (const auto& desc : cs.buffers) {
            const auto sharp = desc.GetSharp(cs);
            for (const VAddr at : cs_watch) {
                const VAddr base = sharp.base_address;
                if (at >= base && at < base + sharp.GetSize() &&
                    watched.insert(cs.pgm_hash ^ (u64(desc.is_written) << 63)).second) {
                    std::printf("CS watch: shader %016llx %s %#llx (buffer %#llx + %#llx, %ux%ux%u, %ux%ux%u threads)\n",
                                static_cast<unsigned long long>(cs.pgm_hash),
                                desc.is_written ? "writes" : "reads",
                                static_cast<unsigned long long>(at), static_cast<unsigned long long>(base),
                                static_cast<unsigned long long>(sharp.GetSize()), cs_program.dim_x,
                                cs_program.dim_y, cs_program.dim_z, cs_program.num_thread_x.full,
                                cs_program.num_thread_y.full, cs_program.num_thread_z.full);
                    for (const auto& other : cs.buffers) {
                        const auto o = other.GetSharp(cs);
                        std::printf("CS watch:   buffer %#llx + %#llx stride %u %s\n",
                                    static_cast<unsigned long long>(o.base_address),
                                    static_cast<unsigned long long>(o.GetSize()), u32(o.GetStride()),
                                    other.is_written ? "written" : "read");
                    }
                }
            }
        }
    }
    // tlg: TLG_CS_DUMP=<hex hash> (diagnostic): every 256th dispatch of that compute shader: its user
    // data, and each buffer with its first 24 dwords when it is read-only and mapped (constants such
    // as counts: what changes when the same dispatch gets slower).
    static const u64 cs_dump = [] {
        const char* env = std::getenv("TLG_CS_DUMP");
        return env && *env ? std::strtoull(env, nullptr, 16) : 0ull;
    }();
    if (cs_dump && cs.pgm_hash == cs_dump) [[unlikely]] {
        static u64 dumps = 0;
        if ((dumps++ & 255) == 0) {
            std::printf("CS dump %016llx #%llu: %ux%ux%u; user data:", (unsigned long long)cs.pgm_hash,
                        (unsigned long long)dumps, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
            for (size_t i = 0; i < cs.user_data.size() && i < 16; ++i) {
                std::printf(" %08x", cs.user_data[i]);
            }
            std::printf("\n");
            u32 index = 0;
            for (const auto& desc : cs.buffers) {
                if (desc.IsSpecial()) {
                    ++index;
                    continue;
                }
                const auto sharp = desc.GetSharp(cs);
                const VAddr base = sharp.base_address;
                const u64 size = sharp.GetSize();
                std::printf("CS dump   buffer %u %#llx + %#llx stride %u %s:", index++, (unsigned long long)base,
                            (unsigned long long)size, u32(sharp.GetStride()), desc.is_written ? "written" : "read");
                if (!desc.is_written && base && size >= 4 && IsMapped(base, std::min<u64>(size, 96))) {
                    const u32* words = std::bit_cast<const u32*>(base);
                    for (u64 i = 0; i < std::min<u64>(size / 4, 24); ++i) {
                        std::printf(" %08x", words[i]);
                    }
                }
                std::printf("\n");
            }
        }
    }
    // After the resource binding: its transfers (image uploads, downloads) mark themselves.
    const auto mark = [&] {
        if (auto* profiler = GpuProfiler::Get()) {
            profiler->Mark(cs.pgm_hash ^ 0xD15Aull, [&] {
                return fmt::format("dispatch cs {:016x} ({}x{}x{} groups)", cs.pgm_hash,
                                   cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
            });
        }
    };
    if (upscaler->Enabled()) {
        upscaler->OnDispatch(cs.pgm_hash);
    }
    if (FrameRecord::IsRecording()) [[unlikely]] {
        RecordFrameInputs(pipeline, nullptr, nullptr, false, 0, nullptr);
    }
    if (ExecuteShaderHLE(cs, Regs(), cs_program, *this)) {
        if (FrameRecord::Open()) [[unlikely]] {
            FrameRecord::Append(" hle");
        }
        if (dispatch_flags & 1u) {
            AcbAfterMainWork();
        }
        dispatch_flags = 0;
        return;
    }

    const u64 main_records = scheduler.RecordCount();
    if (!BindResources(pipeline)) {
        dispatch_flags = 0;
        return;
    }
    // tlg: TLG_ACB_PACKETS=1: compute-queue dispatches that bind images, every 5 s.
    static const bool acb_images = [] {
        const char* v = std::getenv("TLG_ACB_PACKETS");
        return v && *v == '1';
    }();
    if (acb_images && (dispatch_flags & 1u)) [[unlikely]] {
        static u64 total = 0, with_images = 0, with_gds = 0;
        static auto last = std::chrono::steady_clock::now();
        ++total;
        with_images += !bound_images.empty();
        for (const auto& b : cs.buffers) {
            if (b.buffer_type == Shader::BufferType::GdsBuffer) {
                ++with_gds;
                break;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(5)) {
            last = now;
            std::printf("ACB dispatches (5 s): %llu, with images %llu, with GDS %llu\n",
                        (unsigned long long)total, (unsigned long long)with_images,
                        (unsigned long long)with_gds);
            total = with_images = with_gds = 0;
        }
    }
    if (Scheduler* acb = AcbTarget(cs, dispatch_flags, main_records)) {
        std::scoped_lock acb_lock{acb->CompanionMutex()};
        AcbBind(*acb, pipeline, (dispatch_flags & 2u) != 0);
        dispatch_flags = 0;
        const auto acb_bind = ComputeBind(pipeline);
        const u32 x = cs_program.dim_x, y = cs_program.dim_y, z = cs_program.dim_z;
        LogWork("dispatch cs", cs.pgm_hash, x, y, z);
        const DrawLabel label = MakeDrawLabel("acb cs", cs.pgm_hash, 0, x, y);
        acb->Record([=](vk::CommandBuffer cmdbuf) {
            acb_bind(cmdbuf);
            InsertLabel(cmdbuf, label);
            cmdbuf.dispatch(x, y, z);
        });
        acb->MarkCompanionWork();
        DebugState.IncDispatch();
        ResetBindings(true);
        return;
    }
    if ((dispatch_flags & 2u) && AcbUnordered()) {
        needs_barrier = true; // the game synced before this run: everything earlier is ordered
    }
    dispatch_flags = 0;

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    mark();
    pipeline->BindResources(set_writes, set_write_index, push_data);

    const auto bind = ComputeBind(pipeline);
    const u32 dim_x = cs_program.dim_x, dim_y = cs_program.dim_y, dim_z = cs_program.dim_z;
    LogWork("dispatch cs", cs.pgm_hash, dim_x, dim_y, dim_z);
    if (FrameCapture::Active()) {
        FrameCapture::Dispatch(cs.pgm_hash, dim_x, dim_y, dim_z);
    }
    const DrawLabel label = MakeDrawLabel("cs", cs.pgm_hash, 0, dim_x, dim_y);
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        if (Scheduler::FullBarriers()) {
            Scheduler::RecordFullBarrier(cmdbuf);
        }
        bind(cmdbuf);
        InsertLabel(cmdbuf, label);
        cmdbuf.dispatch(dim_x, dim_y, dim_z);
        if (Scheduler::FullBarriers()) {
            Scheduler::RecordFullBarrier(cmdbuf);
        }
    });
    DebugState.IncDispatch();
    scheduler.CountWork();
    if (timed) {
        scheduler.Finish();
        Timing().Add(cs.pgm_hash, std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - timed_start)
                                      .count());
    }

    // tlg diagnostics: TLG_DIAG_IMAGES=1 writes the images a dispatch read and wrote, after
    // it, while a frame capture records (TLG_CAPTURE_TRIGGER).
    static const bool diag_images = [] {
        const char* env = std::getenv("TLG_DIAG_IMAGES");
        return env && *env == '1';
    }();
    if (diag_images && FrameCapture::IsRecording()) {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "cs%08x", static_cast<u32>(cs.pgm_hash));
        const auto images = bound_images;
        for (const auto image_id : images) {
            texture_cache.DiagDumpImage(image_id, tag);
        }
    }

    ResetBindings(true);
    scheduler.KickRecording();
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;
    const u32 flags = std::exchange(next_dispatch_flags, 0u);
    // tlg: handed to the recording thread like direct dispatches.
    const bool pipelined = UseDrawPipe() && !TlgToggle::Disabled(TlgToggle::PipelinedIndirectDraws);
    if (!pipelined) {
        DrainDrawPipe();
    }
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    if (pipelined) {
        const IndirectDraw indirect = {address + offset, 0, size, 1};
        PostDraw(pipeline, nullptr, false, flags, &CsRegs(), &indirect);
        return;
    }
    dispatch_flags = flags;
    DispatchIndirectRecord(pipeline, address + offset, size);
}

void Rasterizer::DispatchIndirectRecord(const ComputePipeline* pipeline, VAddr address,
                                        u32 size) {
    scheduler.PopPendingOperations();
    const u32 offset = 0;

    const u64 main_records = scheduler.RecordCount();
    if (!BindResources(pipeline)) {
        dispatch_flags = 0;
        return;
    }
    if (Scheduler* acb = AcbTarget(pipeline->GetStage(Shader::SwStage::Compute), dispatch_flags, main_records)) {
        const auto [args_buffer, args_base] = buffer_cache.ObtainBuffer(address + offset, size, false);
        if (scheduler.RecordCount() != main_records) {
            AcbWaitMain();
        }
        std::scoped_lock acb_lock{acb->CompanionMutex()};
        // The arguments come from earlier dispatches: ordered with a barrier if any wrote them.
        AcbBind(*acb, pipeline, (dispatch_flags & 2u) != 0 ||
                                    runtime.IsBufferAccessed(args_buffer, args_base, size));
        dispatch_flags = 0;
        const auto acb_bind = ComputeBind(pipeline);
        const vk::Buffer args_handle = args_buffer->Handle() == buffer_cache.UnifiedHandle()
                                           ? buffer_cache.UnifiedAlias()
                                           : args_buffer->Handle();
        const u64 offset_in_args = args_base;
        const DrawLabel label =
            MakeDrawLabel("acb indirect cs", pipeline->GetStage(Shader::SwStage::Compute).pgm_hash, 0, 0, 0);
        acb->Record([=](vk::CommandBuffer cmdbuf) {
            acb_bind(cmdbuf);
            InsertLabel(cmdbuf, label);
            cmdbuf.dispatchIndirect(args_handle, offset_in_args);
        });
        acb->MarkCompanionWork();
        DebugState.IncDispatch();
        ResetBindings(true);
        return;
    }
    if ((dispatch_flags & 2u) && AcbUnordered()) {
        needs_barrier = true; // the game synced before this run
    }
    dispatch_flags = 0;

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, size);
    // tlg: BP10 (shadPS4 c09fa46a, #5262) record the fetch so a later writer gets a barrier.
    if (Common::Backports::On(Common::Backports::Id::VertexIndexBarriers)) {
        Common::Backports::Hit(Common::Backports::Id::VertexIndexBarriers);
        bound_buffers.push_back({buffer, base, static_cast<u32>(size), vk::AccessFlagBits2::eIndirectCommandRead});
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    if (FrameRecord::IsRecording()) [[unlikely]] {
        const IndirectDraw indirect = {address + offset, 0, size, 1};
        RecordFrameInputs(pipeline, nullptr, nullptr, false, 0, &indirect);
    }
    scheduler.EndRendering();
    pipeline->BindResources(set_writes, set_write_index, push_data);

    if (DispatchLogThreshold() != 0) {
        u32 dims[3];
        std::memcpy(dims, reinterpret_cast<const void*>(address + offset), sizeof(dims));
        LogWork("indirect dispatch cs", pipeline->GetStage(Shader::SwStage::Compute).pgm_hash, dims[0],
                    dims[1], dims[2]);
    }
    const auto bind = ComputeBind(pipeline);
    const vk::Buffer args = buffer->Handle();
    const u64 args_offset = base;
    const DrawLabel label =
        MakeDrawLabel("indirect cs", pipeline->GetStage(Shader::SwStage::Compute).pgm_hash, 0, 0, 0);
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        if (Scheduler::FullBarriers()) {
            Scheduler::RecordFullBarrier(cmdbuf);
        }
        bind(cmdbuf);
        InsertLabel(cmdbuf, label);
        cmdbuf.dispatchIndirect(args, args_offset);
        if (Scheduler::FullBarriers()) {
            Scheduler::RecordFullBarrier(cmdbuf);
        }
    });
    DebugState.IncDispatch();
    scheduler.CountWork();

    ResetBindings(true);
    scheduler.KickRecording();
}

namespace {
// tlg: frame recorder helpers. Registers as 32-bit words, '.'-separated.
template <typename T>
std::string RecordWords(const T& value) {
    static_assert(sizeof(T) % 4 == 0);
    std::array<u32, sizeof(T) / 4> words;
    std::memcpy(words.data(), &value, sizeof(T));
    std::string out;
    for (size_t i = 0; i < words.size(); ++i) {
        out += fmt::format("{}{:x}", i ? "." : "", words[i]);
    }
    return out;
}
// Indexed by Shader::SwStage.
constexpr std::array<const char*, Shader::MaxStageTypes> RecordStageNames = {"ps", "hs", "ds",
                                                                            "vs", "gs", "cs"};
} // namespace

void Rasterizer::RecordFrameInputs(const Pipeline* pipeline, const PreparedDraw* prepared,
                                   const RenderState* state, bool is_indexed, u32 index_offset,
                                   const IndirectDraw* indirect) {
    if (!FrameRecord::Open() || DrawPipe::OnStageB()) {
        return;
    }
    const auto add = [](const std::string& text) { FrameRecord::Append(text); };
    // The copies end the render pass; a draw that clears its targets and continues the pass
    // in progress would clear them again when it begins a new one.
    bool gpu_ok = FrameRecord::GpuCopiesAvailable();
    if (state && scheduler.IsRenderingWith(*state)) {
        bool clears = state->depth_stencil_attachment.depth_clear ||
                      state->depth_stencil_attachment.stencil_clear;
        for (u32 i = 0; i < state->num_color_attachments; ++i) {
            clears |= state->color_attachments[i].is_clear != 0;
        }
        if (clears) {
            gpu_ok = false;
            add(" gpu=skipped_clear");
        }
    }
    // Memory the GPU wrote is read from the GPU's copy (the guest copy may be stale or
    // read-protected); anything else from guest memory, which the cache mirrors.
    const auto memory_input = [&](const std::string& key, VAddr address, u64 size) {
        add(fmt::format(" {}={:#x}+{}:", key, address, size));
        if (address == 0 || size == 0) {
            add("null");
            return;
        }
        size = memory->ClampRangeSize(address, size);
        if (gpu_ok && size && size <= std::numeric_limits<u32>::max() &&
            buffer_cache.IsRegionGpuModified(address, size)) {
            const auto [buffer, offset] = buffer_cache.ObtainBuffer(address, u32(size), false);
            FrameRecord::GpuHash("", buffer->Handle(), offset, size);
        } else {
            add(FrameRecord::GuestHash(address, size));
        }
    };

    const auto* saved_prepared = bind_prepared;
    bind_prepared = prepared;
    const auto& regs = Regs();
    for (const Shader::Info* info : pipeline->GetStages()) {
        if (!info) {
            continue;
        }
        const u32 s = static_cast<u32>(info->sw_stage);
        const char* name = s < RecordStageNames.size() ? RecordStageNames[s] : "xs";
        // Permutation: the pipeline key holds HashCombine(program hash, permutation index).
        const u64 key_hash =
            pipeline->IsCompute()
                ? static_cast<const ComputePipeline*>(pipeline)->GetComputeKey().value
                : static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey().stage_hashes[s];
        std::string perm = "?";
        for (u64 p = 0; p < 4096; ++p) {
            if (HashCombine(info->pgm_hash, p) == key_hash) {
                perm = std::to_string(p);
                break;
            }
        }
        const auto ud = info->FlatUserData();
        add(fmt::format(" {}={:016x}/p{} {}.ud={:016x}", name, info->pgm_hash, perm, name,
                        XXH3_64bits(ud.data(), ud.size() * sizeof(u32))));
        const PreparedStage* ps = FindPreparedStage(*info);
        for (u32 i = 0; i < info->buffers.size(); ++i) {
            const auto& desc = info->buffers[i];
            if (desc.IsSpecial()) {
                continue;
            }
            const auto sharp = ps ? ps->buffer_sharps[i] : desc.GetSharp(*info);
            memory_input(fmt::format("{}.b{}{}", name, i, desc.is_written ? "w" : ""),
                         sharp.base_address, sharp.GetSize());
        }
        for (u32 i = 0; i < info->images.size(); ++i) {
            const auto& desc = info->images[i];
            const auto t = ps ? ps->image_sharps[i] : desc.GetSharp(*info);
            add(fmt::format(" {}.t{}{}=", name, i, desc.is_written ? "w" : ""));
            if (t.Address() == 0) {
                add("null");
                continue;
            }
            add(fmt::format("{:#x}:{}/{}:{}x{}x{}:{}:m{}-{}", t.Address(),
                            magic_enum::enum_name(t.GetDataFmt()),
                            magic_enum::enum_name(t.GetNumberFmt()), u32(t.width) + 1,
                            u32(t.height) + 1, u32(t.depth) + 1,
                            magic_enum::enum_name(t.GetType()), u32(t.base_level),
                            u32(t.last_level)));
        }
        for (u32 i = 0; i < info->samplers.size(); ++i) {
            const auto ss = ps ? ps->sampler_sharps[i] : info->samplers[i].GetSharp(*info);
            add(fmt::format(" {}.s{}={:08x}", name, i, u32(XXH3_64bits(&ss, sizeof(ss)))));
        }
    }

    if (pipeline->IsCompute()) {
        if (!indirect) {
            const auto& cs = CsRegs();
            add(fmt::format(" groups={}x{}x{}", cs.dim_x, cs.dim_y, cs.dim_z));
        }
    } else {
        const auto* graphics = static_cast<const GraphicsPipeline*>(pipeline);
        const auto& vs = graphics->GetStage(Shader::SwStage::Vertex);
        if (const auto& fetch = graphics->GetFetchShader()) {
            for (u32 i = 0; i < fetch->attributes.size(); ++i) {
                const auto sharp = fetch->attributes[i].GetSharp(vs);
                memory_input(fmt::format("vb{}", i), sharp.base_address, sharp.GetSize());
            }
        }
        if (is_indexed) {
            const u32 index_size =
                regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2u : 4u;
            const u64 count = indirect ? regs.index_size : regs.num_indices;
            add(fmt::format(" ioff={}", index_offset));
            memory_input("ib",
                         regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size,
                         count * index_size);
        }
        add(fmt::format(" prim={}", magic_enum::enum_name(regs.primitive_type)));
        const auto& vp = regs.viewports[0];
        add(fmt::format(" vp={:g},{:g},{:g},{:g},{:g},{:g} scis={}", vp.xscale, vp.xoffset,
                        vp.yscale, vp.yoffset, vp.zscale, vp.zoffset,
                        RecordWords(regs.screen_scissor)));
        const u32 num_colors = state ? state->num_color_attachments : 0;
        for (u32 cb = 0; cb < num_colors; ++cb) {
            add(fmt::format(" rt{}=", cb));
            if (!cb_descs[cb].first) {
                add("null");
            } else {
                const auto& image = texture_cache.GetImage(cb_descs[cb].first).info;
                add(fmt::format("{:#x}:{}:{}x{}", image.guest_address,
                                vk::to_string(image.pixel_format), image.size.width,
                                image.size.height));
            }
            add(fmt::format(" bl{}={}", cb, RecordWords(regs.blend_control[cb])));
        }
        if (db_desc.first) {
            const auto& image = texture_cache.GetImage(db_desc.first).info;
            add(fmt::format(" z={:#x}:{}:{}x{}", image.guest_address,
                            vk::to_string(image.pixel_format), image.size.width,
                            image.size.height));
        }
        add(fmt::format(" cm={} csm={} cc={} dc={} st={} sref={}.{} drc={} pc={} bc={}",
                        RecordWords(regs.color_target_mask), RecordWords(regs.color_shader_mask),
                        RecordWords(regs.color_control), RecordWords(regs.depth_control),
                        RecordWords(regs.stencil_control), RecordWords(regs.stencil_ref_front),
                        RecordWords(regs.stencil_ref_back), RecordWords(regs.depth_render_control),
                        RecordWords(regs.polygon_control), RecordWords(regs.blend_constants)));
        if (state) {
            u32 clears = 0;
            for (u32 i = 0; i < state->num_color_attachments; ++i) {
                clears |= u32(state->color_attachments[i].is_clear != 0) << i;
            }
            clears |= u32(state->depth_stencil_attachment.depth_clear) << 8 |
                      u32(state->depth_stencil_attachment.stencil_clear) << 9;
            add(fmt::format(" clr={:x}", clears));
        }
    }

    if (indirect) {
        // Arguments as the guest wrote them and as the GPU reads them at this draw.
        const u64 bytes = u64(indirect->stride) * indirect->max_count;
        add(fmt::format(" max={} stride={} args.cpu={}", indirect->max_count, indirect->stride,
                        FrameRecord::GuestWords(indirect->args, bytes)));
        if (gpu_ok && bytes && bytes <= std::numeric_limits<u32>::max()) {
            const auto [buffer, offset] = buffer_cache.ObtainBuffer(indirect->args, u32(bytes), false);
            FrameRecord::GpuWords(" args=", buffer->Handle(), offset, bytes);
        }
        if (indirect->count) {
            add(fmt::format(" count.cpu={}", FrameRecord::GuestWords(indirect->count, 4)));
            if (gpu_ok) {
                const auto [buffer, offset] = buffer_cache.ObtainBuffer(indirect->count, 4, false);
                FrameRecord::GpuWords(" count=", buffer->Handle(), offset, 4);
            }
        }
    }
    bind_prepared = saved_prepared;
    if (gpu_ok) {
        FrameRecord::FlushGpuCopies();
    }
}

u64 Rasterizer::Flush() {
    DrainDrawPipe();
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    DrainDrawPipe();
    scheduler.Finish();
}

// tlg (i3-pc 25f247c): a game that binds a heap whole and read-only (ISS: 0.5 and 2.5 GB
// geometry heaps, indexed by a per-instance offset, so no part can be left out) uploaded all of it
// in the frame of the first bind (a freeze of 2 to 3 seconds). The ranges are remembered per title
// (heap_binds.txt in the game cache) and, in later sessions, uploaded a slice per submission until
// the game binds them, so the first bind finds only what was written since. Uploading early is
// sound: a later write marks the pages again. On; TLG_HEAP_WARM=0 turns it off.
namespace {
constexpr u64 HugeBindSize = 256ull << 20;
constexpr u64 HugeWarmSlice = 16ull << 20;

struct HugeBind {
    VAddr base;
    u64 size;
    u64 cursor{};
    bool bound{};
};

bool HugeWarmEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("TLG_HEAP_WARM");
        return !value || value[0] != '0';
    }();
    return enabled;
}

std::filesystem::path HugeBindFile() {
    return Storage::GameCacheDir() / "heap_binds.txt";
}

std::mutex huge_binds_mutex;

std::vector<HugeBind>& HugeBinds() {
    static std::vector<HugeBind> binds = [] {
        std::vector<HugeBind> loaded;
        if (FILE* f = std::fopen(HugeBindFile().string().c_str(), "r")) {
            unsigned long long base, size;
            while (loaded.size() < 16 && std::fscanf(f, "%llx %llx", &base, &size) == 2) {
                if (size >= HugeBindSize && size <= (4ull << 30)) {
                    loaded.push_back({VAddr(base), u64(size)});
                }
            }
            std::fclose(f);
        }
        return loaded;
    }();
    return binds;
}

void NoteHugeBind(VAddr base, u64 size) {
    if (!HugeWarmEnabled()) {
        return;
    }
    std::scoped_lock lock{huge_binds_mutex};
    auto& binds = HugeBinds();
    for (auto& bind : binds) {
        if (bind.base == base && bind.size == size) {
            bind.bound = true;
            return;
        }
    }
    if (binds.size() >= 16) {
        return;
    }
    binds.push_back({base, size, 0, true});
    std::error_code ec;
    std::filesystem::create_directories(HugeBindFile().parent_path(), ec);
    if (FILE* f = std::fopen(HugeBindFile().string().c_str(), "a")) {
        std::fprintf(f, "%llx %llx\n", static_cast<unsigned long long>(base),
                     static_cast<unsigned long long>(size));
        std::fclose(f);
    }
}
} // namespace

void Rasterizer::OnSubmit() {
    DrainDrawPipe();
    if (occlusion) {
        occlusion->OnSubmit();
    }
    if (fault_process_pending) {
        fault_process_pending = false;
        buffer_cache.ProcessFaultBuffer();
    }
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    if (HugeWarmEnabled()) {
        // One slice of each remembered heap this session has not bound yet. The draw pipe was
        // drained above, so no draw is binding buffers meanwhile.
        std::scoped_lock lock{huge_binds_mutex};
        for (auto& bind : HugeBinds()) {
            if (bind.bound) {
                continue;
            }
            const VAddr start = bind.base + bind.cursor;
            const u64 length = std::min(HugeWarmSlice, bind.size - bind.cursor);
            bind.cursor = bind.cursor + length >= bind.size ? 0 : bind.cursor + length;
            boost::container::small_vector<std::pair<VAddr, u64>, 8> parts;
            ForEachMappedRangeInRange(start, length, [&](const auto& range) {
                parts.emplace_back(range.lower(), range.upper() - range.lower());
            });
            for (const auto& [addr, part_size] : parts) {
                if (part_size > 64_KB) {
                    (void)buffer_cache.ObtainBuffer(addr, u32(part_size), false);
                }
            }
        }
    }
    runtime.TickFrame();
}

const PreparedStage* Rasterizer::FindPreparedStage(const Shader::Info& stage) const {
    // Sharps a draw-preparation worker read from the same flattened user data.
    if (!bind_prepared || TlgToggle::Disabled(TlgToggle::PreparedResources)) {
        return nullptr;
    }
    for (u32 i = 0; i < bind_prepared->num_stages; ++i) {
        const auto& candidate = bind_prepared->stages[i];
        if (&candidate.program->info == &stage &&
            candidate.num_images == stage.images.size() &&
            candidate.num_samplers == stage.samplers.size() &&
            candidate.num_buffers == stage.buffers.size()) {
            return &candidate;
        }
    }
    return nullptr;
}

bool Rasterizer::BindHelperWanted() {
    // A spinning helper pays off only with cores to spare (the recording thread spins too).
    const char* env = std::getenv("TLG_TEXTURE_HELPER");
    if (env && env[0]) {
        return env[0] == '1';
    }
    // Measured neutral on 16 threads (docs/parallel_gpu.md): opt-in.
    return false;
}

bool Rasterizer::HelperEligible(const Pipeline* pipeline) const {
    if (pipeline->IsCompute() || !bind_helper.Available() || FrameCapture::Active() ||
        TlgToggle::Disabled(TlgToggle::TextureBindHelper) ||
        TlgToggle::Disabled(TlgToggle::TextureBindingMemo) ||
        TlgToggle::Disabled(TlgToggle::UpdateImageFastPath)) {
        return false;
    }
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        // Mip arrays make the descriptor layout depend on the T#s.
        for (const auto& image : stage->images) {
            if (image.mip_fallback_mode != Shader::MipStorageFallbackMode::None) {
                return false;
            }
        }
        // Storage writes invalidate cached images from the buffer side.
        for (const auto& buffer : stage->buffers) {
            if (!buffer.IsSpecial() && buffer.is_written) {
                return false;
            }
        }
    }
    return true;
}

bool Rasterizer::TexturesBindableOnHelper(const Shader::Info& stage,
                                          const PreparedStage* prepared) {
    const u64 generation = texture_cache.RegistryGeneration();
    for (u32 image_index = 0; image_index < stage.images.size(); ++image_index) {
        const auto& image_desc = stage.images[image_index];
        if (image_desc.is_written) {
            return false;
        }
        const auto tsharp =
            prepared ? prepared->image_sharps[image_index] : image_desc.GetSharp(stage);
        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid ||
            !memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !IsKnownFormat(data_fmt, num_fmt)) {
            continue; // null descriptor
        }
        const auto& entry =
            prepared ? CachedImageDescEntry(tsharp, image_desc, prepared->image_hashes[image_index])
                     : CachedImageDescEntry(tsharp, image_desc);
        if (entry.found_generation != generation) {
            return false;
        }
        VideoCore::ImageId image_id = entry.found_id;
        auto* image = &texture_cache.GetImage(image_id);
        if (const auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
            image_id = depth_image_id;
            image = &texture_cache.GetImage(image_id);
        }
        if (image->binding.needs_rebind || !texture_cache.IsUpToDate(image_id) ||
            scene_targets->Tracks(image->image_uid) ||
            (upscaler->Enabled() && upscaler->RedirectsSampled(image_id))) {
            return false;
        }
    }
    return true;
}

void Rasterizer::RunTextureTask(void* context) {
    auto& self = *static_cast<Rasterizer*>(context);
    auto& task = self.texture_task;
    for (u32 i = 0; i < task.count; ++i) {
        auto& stage = task.stages[i];
        if (!self.TexturesBindableOnHelper(*stage.info, stage.prepared)) {
            return;
        }
        auto binding = stage.binding;
        u32 write_index = stage.write_index;
        self.BindTextures(*stage.info, stage.prepared, binding, write_index, task.barrier, true);
        task.completed = i + 1;
    }
}

void Rasterizer::JoinBindHelper(void* context) {
    if (!BindHelper::OnHelper()) {
        static_cast<Rasterizer*>(context)->bind_helper.Join();
    }
}

bool Rasterizer::SetWritePool() {
    // tlg: TLG_SET_WRITE_POOL=0 restores the per-draw clear and resize of the descriptor writes.
    static const bool enabled = [] {
        const char* env = std::getenv("TLG_SET_WRITE_POOL");
        const bool off = env && env[0] == '0';
        if (off) {
            std::printf("tlg: per-draw descriptor write pool off (TLG_SET_WRITE_POOL=0)\n");
        }
        return !off;
    }();
    return enabled;
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        return false;
    }

    set_write_index = 0;
    if (!SetWritePool()) {
        set_writes.clear();
    }
    fb_mrt_mask = pipeline->IsCompute()
                      ? 0u
                      : static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey().mrt_mask;
    buffer_infos.clear();
    image_infos.clear();
    // tlg: G-buffer passes sample material textures for a scene rendered below the output
    // size; the temporal upscaler restores the detail of the output's mip level (FSR guide:
    // log2(render / output)). Shadows, post-processing and UI keep the guest's bias.
    // The menu's texture LOD bias (mip_lod_bias) adds to every draw: textures without mips
    // (UI, full-screen passes) are unaffected. Steps of 1/8: dragging the slider creates few samplers.
    sampler_lod_bias = pipeline->IsCompute() ? 0.0f
                                             : std::round(TlgSettings::Get().mip_lod_bias.load() * 8.0f) / 8.0f;
    if (!pipeline->IsCompute() && !TlgToggle::Disabled(TlgToggle::SceneMipBias) &&
        std::popcount(static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey().mrt_mask &
                      0xff) >= 5) {
        sampler_lod_bias += upscaler->SceneMipBias();
        static float reported = 0.0f;
        if (sampler_lod_bias != reported) {
            reported = sampler_lod_bias;
            std::printf("Upscaler: scene texture LOD bias %.2f\n", sampler_lod_bias);
        }
    }

    bool uses_dma = false;

    static const bool stats = std::getenv("TLG_FRAME_STATS") != nullptr;
    if (stats && ((helper_full + helper_partial + helper_serial) & 1023) == 0) {
        static auto window = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - window >= std::chrono::seconds(5)) {
            window = now;
            const double forks = std::max<double>(1, helper_full + helper_partial);
            std::printf("Texture helper: %llu binds in parallel, %llu partly, %llu serial; "
                        "per fork %.0f cycles helper task, %.0f cycles GPU thread wait\n",
                        static_cast<unsigned long long>(helper_full),
                        static_cast<unsigned long long>(helper_partial),
                        static_cast<unsigned long long>(helper_serial),
                        bind_helper.task_cycles.exchange(0) / forks,
                        bind_helper.wait_cycles / forks);
            bind_helper.wait_cycles = 0;
            helper_full = helper_partial = helper_serial = 0;
        }
    }

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(Regs());
    if (!HelperEligible(pipeline)) {
        ++helper_serial;
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            EnsureSetWrites(set_write_index + static_cast<u32>(stage->buffers.size() +
                                                              stage->images.size() +
                                                              stage->samplers.size()));
            stage->PushUd(binding, push_data);
            const PreparedStage* prepared = FindPreparedStage(*stage);
            BindBuffers(*stage, prepared, binding, push_data, set_write_index);
            BindTextures(*stage, prepared, binding, set_write_index, needs_barrier);
            uses_dma |= stage->uses_dma;
        }
    } else {
        // tlg: textures on the helper, buffers here. Without mip arrays every stage's
        // descriptors are [buffers][images][samplers], one each, so both sides know their
        // binding numbers and descriptor write slots up front.
        struct BufferStage {
            const Shader::Info* info;
            const PreparedStage* prepared;
            Shader::Backend::Bindings binding;
            u32 write_index;
        };
        std::array<BufferStage, Shader::MaxStageTypes> buffer_stages;
        auto& task = texture_task;
        task.count = 0;
        task.completed = 0;
        task.barrier = false;
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            const u32 num_buffers = static_cast<u32>(stage->buffers.size());
            const u32 num_descriptors =
                num_buffers + static_cast<u32>(stage->images.size() + stage->samplers.size());
            EnsureSetWrites(set_write_index + num_descriptors);
            stage->PushUd(binding, push_data);
            const PreparedStage* prepared = FindPreparedStage(*stage);
            buffer_stages[task.count] = {stage, prepared, binding, set_write_index};
            auto texture_binding = binding;
            texture_binding.buffer += num_buffers;
            texture_binding.unified += num_buffers;
            task.stages[task.count] = {stage, prepared, texture_binding,
                                       set_write_index + num_buffers};
            ++task.count;
            binding.buffer += num_buffers;
            binding.unified += num_descriptors;
            set_write_index += num_descriptors;
            uses_dma |= stage->uses_dma;
        }
        bind_helper.Fork(&RunTextureTask, this);
        for (u32 i = 0; i < task.count; ++i) {
            auto& stage = buffer_stages[i];
            BindBuffers(*stage.info, stage.prepared, stage.binding, push_data, stage.write_index);
        }
        if (draw_inputs.pending && !TlgToggle::Disabled(TlgToggle::EarlyDrawInputs)) {
            ResolveVertexBuffers(draw_inputs.pipeline, draw_inputs.prepared);
            if (draw_inputs.is_indexed) {
                ResolveIndexBuffer(draw_inputs.index_offset);
            }
            draw_inputs.resolved = true;
        }
        bind_helper.Join();
        needs_barrier |= task.barrier;
        ++(task.completed == task.count ? helper_full
                                        : task.completed ? helper_partial : helper_serial);
        // Stages the helper left (an image needing work that records commands).
        for (u32 i = task.completed; i < task.count; ++i) {
            auto& stage = task.stages[i];
            u32 write_index = stage.write_index;
            BindTextures(*stage.info, stage.prepared, stage.binding, write_index, needs_barrier);
        }
    }

    if (uses_dma) {
        buffer_cache.SynchronizeDmaBuffers();
        fault_process_pending = true;
    }

    return true;
}

void Rasterizer::BindVertexBuffers(const GraphicsPipeline* pipeline, const PreparedDraw* prepared) {
    ResolveVertexBuffers(pipeline, prepared);
    EmitVertexBuffers();
}

void Rasterizer::EmitVertexBuffers() {
    // tlg: only the used entries go into the recording chunk (the static vectors hold 32 of
    // each: ~3 KiB copied per draw).
    auto& v = vertex_binds;
    const bool dynamic_input = instance.IsVertexInputDynamicState();
    const u32 num_buffers = v.num_buffers;
    scheduler.ReserveRecordData(
        v.bindings.size() * sizeof(v.bindings[0]) + v.attributes.size() * sizeof(v.attributes[0]) +
        num_buffers * (sizeof(vk::Buffer) + 3 * sizeof(vk::DeviceSize)) + 128);
    if (dynamic_input) {
        scheduler.Record([bindings = scheduler.RecordData(std::span<const vk::VertexInputBindingDescription2EXT>{v.bindings.data(), v.bindings.size()}),
                          attributes = scheduler.RecordData(std::span<const vk::VertexInputAttributeDescription2EXT>{
                              v.attributes.data(), v.attributes.size()})](
                             vk::CommandBuffer cmdbuf) {
            cmdbuf.setVertexInputEXT(bindings, attributes);
        });
    }
    if (num_buffers == 0) {
        return;
    }
    const auto buffers = scheduler.RecordData(std::span<const vk::Buffer>{v.host_buffers.data(), num_buffers});
    const auto offsets = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_offsets.data(), num_buffers});
    if (dynamic_input) {
        scheduler.Record([num_buffers, buffers, offsets](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindVertexBuffers(0, num_buffers, buffers.data(), offsets.data());
        });
        return;
    }
    const auto sizes = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_sizes.data(), num_buffers});
    const auto strides = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_strides.data(), num_buffers});
    scheduler.Record([num_buffers, buffers, offsets, sizes, strides](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindVertexBuffers2(0, num_buffers, buffers.data(), offsets.data(), sizes.data(),
                                  strides.data());
    });
}

void Rasterizer::ResolveVertexBuffers(const GraphicsPipeline* pipeline,
                                      const PreparedDraw* prepared) {
    const auto& regs = Regs();
    auto& v = vertex_binds;
    v.num_buffers = 0;
    v.host_buffers.clear();
    v.host_offsets.clear();
    v.host_sizes.clear();
    v.host_strides.clear();
    auto& attributes = v.attributes;
    auto& bindings = v.bindings;
    attributes.clear();
    bindings.clear();
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    // Inputs a draw-preparation worker read from the same registers and (verified) user data.
    const auto& fetch = pipeline->GetFetchShader();
    const PreparedVertexInputs* ready =
        prepared && prepared->vertex.valid && instance.IsVertexInputDynamicState() && fetch &&
                fetch->attributes.size() == prepared->vertex.count && !FrameCapture::Active() &&
                !TlgToggle::Disabled(TlgToggle::PreparedResources)
            ? &prepared->vertex
            : nullptr;
    if (ready) {
        attributes.assign(ready->attributes, ready->attributes + ready->count);
        bindings.assign(ready->bindings, ready->bindings + ready->count);
        guest_buffers.assign(ready->buffers, ready->buffers + ready->count);
    } else {
        pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                                  regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);
    }

    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }
    if (motion_draw && !guest_buffers.empty()) {
        // Include every stream: identical position buffers can be paired with different
        // skinning/instance data. Index topology and base offsets are matched separately.
        motion_geometry = ready ? ready->buffers_hash
                                : XXH3_64bits(guest_buffers.data(),
                                              guest_buffers.size() * sizeof(AmdGpu::Buffer));
    }
    // Object motion research: vertex streams of G-buffer draws.
    if (FrameCapture::Active() && gbuffer_draw) {
        for (const auto& vb : guest_buffers) {
            char note[128];
            std::snprintf(note, sizeof(note), "\n  gb vb at %#llx size %u stride %u",
                          (unsigned long long)vb.base_address, u32(vb.GetSize()),
                          u32(vb.GetStride()));
            FrameCapture::Note(note);
        }
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        const VideoCore::Buffer* buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    // Build list of ranges covering the requested buffers
    VertexInputs<BufferRange> ranges{};
    VertexInputs<BufferRange> ranges_merged{};
    if (ready) {
        for (u32 i = 0; i < ready->num_ranges; ++i) {
            ranges_merged.emplace_back(ready->ranges[i].base, ready->ranges[i].end);
        }
    } else {
        for (const auto& buffer : guest_buffers) {
            if (buffer.base_address != 0 && buffer.GetSize() > 0) {
                ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
            }
        }
    }

    // Merge connecting ranges together
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        std::tie(range.buffer, range.offset) =
            buffer_cache.ObtainBuffer(range.base_address, size, false);
        needs_barrier |= runtime.IsBufferAccessed(range.buffer, range.offset, size);
        // tlg: BP10 (shadPS4 c09fa46a, #5262) record the fetch so a later writer gets a barrier.
        if (Common::Backports::On(Common::Backports::Id::VertexIndexBarriers)) {
            Common::Backports::Hit(Common::Backports::Id::VertexIndexBarriers);
            bound_buffers.push_back({range.buffer, range.offset, static_cast<u32>(size), vk::AccessFlagBits2::eVertexAttributeRead});
        }
    }

    // Bind vertex buffers
    auto& host_buffers = v.host_buffers;
    auto& host_offsets = v.host_offsets;
    auto& host_sizes = v.host_sizes;
    auto& host_strides = v.host_strides;
    u32 stream_with_memory = 0;
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto host_buffer_info =
                ready ? ranges_merged.begin() + ready->range_index[stream_with_memory++]
                      : std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                            return buffer.base_address >= range.base_address &&
                                   buffer.base_address < range.end_address;
                        });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->buffer->Handle());
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(NullBuffer());
            host_offsets.push_back(0);
            if (null_buffer) { // a size past the end of the stand-in would be invalid
                host_sizes.push_back(VK_WHOLE_SIZE);
                host_strides.push_back(buffer.GetStride());
                continue;
            }
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    v.num_buffers = static_cast<u32>(guest_buffers.size());
}

void Rasterizer::BindIndexBuffer(u32 index_offset) {
    ResolveIndexBuffer(index_offset);
    EmitIndexBuffer();
}

void Rasterizer::EmitIndexBuffer() {
    scheduler.Record([handle = index_bind.handle, offset = index_bind.offset,
                      type = index_bind.type](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindIndexBuffer(handle, offset, type);
    });
}

void Rasterizer::ResolveIndexBuffer(u32 index_offset) {
    const auto& regs = Regs();

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    // Bind index buffer.
    const u32 index_buffer_size = regs.num_indices * index_size;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(index_address, index_buffer_size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, offset, index_buffer_size);
    // tlg: BP10 (shadPS4 c09fa46a, #5262) record the fetch so a later writer gets a barrier.
    if (Common::Backports::On(Common::Backports::Id::VertexIndexBarriers)) {
        Common::Backports::Hit(Common::Backports::Id::VertexIndexBarriers);
        bound_buffers.push_back({buffer, offset, static_cast<u32>(index_buffer_size), vk::AccessFlagBits2::eIndexRead});
    }
    index_bind = {buffer->Handle(), offset, index_type};
    index_buffer_obj = buffer;
    index_buffer_bytes = index_buffer_size;
}

void Rasterizer::ResetBindings(bool is_compute) {
    for (auto& image_id : bound_images) {
        texture_cache.GetImage(image_id).binding = {};
    }
    for (const auto [buffer, offset, size, src_access] : bound_buffers) {
        const auto dst_stage = is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                          : vk::PipelineStageFlagBits2::eAllGraphics;
        runtime.AccessBuffer(buffer, offset, size, dst_stage, src_access);
    }
    bound_images.clear();
    bound_buffers.clear();
    needs_barrier = false;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits ||
        image0.info.props.is_bc_decoded || image1.info.props.is_bc_decoded) { // tlg: TLG_BC_TINY
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    runtime.CopyColorAndDepth(&src_image, &dst_image);
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth || image1.info.props.is_bc_decoded) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    runtime.ClearImage(&image1, range, clear);
    return true;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, const PreparedStage* prepared,
                             Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data, u32& write_index) {
    const u64 alignment = instance.StorageMinAlignment();
    for (u32 buffer_index = 0; buffer_index < stage.buffers.size(); ++buffer_index) {
        const auto& desc = stage.buffers[buffer_index];
        const RingBinding* ring = num_ring_stages ? FindRingBinding(stage, buffer_index) : nullptr;
        if (ring) {
            // Copied by the GPU command thread into the constant ring (read-only).
            // tlg: dump the ring's own copy, not the guest address. A binding whose bytes came
            // from the host keeps `address` as a label only, and it can be zero; IsValidGpuMapping
            // merely range checks against the 40 bit GPU limit, so zero passed through and the
            // dump read address zero (crash on the frame capture key).
            if (FrameCapture::Active()) {
                FrameCapture::Buffer(stage.pgm_hash, buffer_index, ring->address,
                                     constant_ring->Data(ring->offset), ring->size);
            }
            if (!desc.IsSpecial()) {
                if (ring->size == 864 && gbuffer_draw &&
                    memory->IsValidGpuMapping(ring->address, 0)) {
                    camera_motion->OnConstants(reinterpret_cast<const float*>(ring->address));
                }
                push_data.AddOffset(binding.buffer, 0);
#ifdef __APPLE__
                push_data.SetSize(binding.buffer, ring->size);
#endif
            }
            buffer_infos.emplace_back(constant_ring->Handle(), ring->offset, ring->size);
        } else if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                static const bool gds_log = std::getenv("TLG_GDS_LOG") != nullptr;
                if (gds_log) {
                    static std::set<u64> seen;
                    if (seen.insert(stage.pgm_hash).second) {
                        std::printf("GDS log: shader %016llx binds GDS\n",
                                    static_cast<unsigned long long>(stage.pgm_hash));
                    }
                }
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                needs_barrier |= runtime.IsBufferAccessed(gds_buf, 0, gds_buf->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetStreamBuffer();
                const u32 ubo_size = stage.FlatUserData().size() * sizeof(u32);
                if (FrameCapture::Active()) {
                    FrameCapture::Buffer(stage.pgm_hash, binding.buffer, 0,
                                         stage.FlatUserData().data(), ubo_size);
                }
                const u64 offset =
                    vk_buffer.Copy(stage.FlatUserData().data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (Regs().clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(NullBuffer(), 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetStreamBuffer();
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = Regs().clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetStreamBuffer();
                const auto& cs_program = CsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                lds_buffer.Commit();
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else {
                UNREACHABLE_MSG("Unexpected buffer type {}", u32(desc.buffer_type));
            }
        } else {
            const auto vsharp =
                prepared ? prepared->buffer_sharps[buffer_index] : desc.GetSharp(stage);
            if (vsharp.GetSize() == 864 && gbuffer_draw &&
                memory->IsValidGpuMapping(vsharp.base_address, 0)) {
                camera_motion->OnConstants(reinterpret_cast<const float*>(vsharp.base_address));
            }
            // Object motion research: the vertex shader buffers of G-buffer draws.
            if (FrameCapture::Active() && gbuffer_draw && stage.sw_stage == Shader::SwStage::Vertex &&
                vsharp.base_address != 0 && memory->IsValidGpuMapping(vsharp.base_address, 0)) {
                const auto* f = reinterpret_cast<const float*>(vsharp.base_address);
                char note[192];
                std::snprintf(note, sizeof(note),
                              "\n  gb vs %08x slot %u at %#llx size %u stride %u: %g %g %g %g",
                              u32(stage.pgm_hash), binding.buffer,
                              (unsigned long long)vsharp.base_address, u32(vsharp.GetSize()),
                              u32(vsharp.GetStride()), f[0], f[1], f[2], f[3]);
                FrameCapture::Note(note);
            }
            if (FrameCapture::Active() && vsharp.base_address != 0 && vsharp.GetSize() != 0 &&
                memory->IsValidGpuMapping(vsharp.base_address, 0)) {
                FrameCapture::Buffer(stage.pgm_hash, binding.buffer, vsharp.base_address,
                                     reinterpret_cast<const void*>(vsharp.base_address),
                                     memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize()));
            }
            if (vsharp.base_address == 0 || vsharp.GetSize() == 0) {
                buffer_infos.emplace_back(NullBuffer(), 0, VK_WHOLE_SIZE);
            } else {
                const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
                if (size != vsharp.GetSize()) {
                    LOG_ERROR(Render, "Clamped size from {} to {} for stage {:#x}",
                              vsharp.GetSize(), size, stage.pgm_hash);
                }
                if (!desc.is_written && size >= HugeBindSize) {
                    NoteHugeBind(vsharp.base_address, size);
                }
                VideoCore::written_binding_source = {"shader", stage.pgm_hash};
                const auto [buffer, offset] = buffer_cache.ObtainBuffer(
                    vsharp.base_address, size, desc.is_written, desc.is_formatted);
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                if (adjust % 4 != 0) {
                    LOG_WARNING(Render_Vulkan, "Buffer binding in shader {:#x} isn't dword aligned",
                                stage.pgm_hash);
                }
                push_data.AddOffset(binding.buffer, adjust);
#ifdef __APPLE__
                push_data.SetSize(binding.buffer, size + adjust);
#endif
                buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                bound_buffers.emplace_back(
                    buffer, offset, size,
                    vk::AccessFlagBits2::eShaderRead |
                        (desc.is_written ? vk::AccessFlagBits2::eShaderWrite
                                         : vk::AccessFlagBits2::eNone));
                if (desc.is_written) {
                    // Raw storage-buffer writes can also make an aliased cached image stale.
                    texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
                }
                // tlg: TLG_ACB_UNORDERED: a compute-queue dispatch orders its buffers only where
                // the game syncs (the first dispatch after its sync packet), as GCN runs the
                // dispatches of one queue without waiting for each other.
                if ((!(dispatch_flags & 1u) || !AcbUnordered()) &&
                    runtime.IsBufferAccessed(buffer, offset, size, desc.is_written)) {
                    needs_barrier = true;
                }
            }
        }

        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        set_write.pImageInfo = nullptr;
        set_write.pTexelBufferView = nullptr;
        ++binding.buffer;
    }
}

Rasterizer::ImageDescCacheEntry& Rasterizer::CachedImageDescEntry(const AmdGpu::Image& sharp,
                                                                   const Shader::ImageResource& res,
                                                                   u64 hash) {
    std::array<u64, 4> key;
    std::memcpy(key.data(), &sharp, sizeof(key));
    // tlg (gow3_pc #348): needs_native too, since FindImage resolves partial height views only for it.
    const u32 flags = u32(res.is_written) | u32(res.is_depth) << 1 | u32(res.is_array) << 2 |
                      u32(res.needs_native) << 3;
    // Two-way set associative: a frame binds a few thousand distinct T#s.
    auto* set = &image_desc_cache[(hash % (image_desc_cache.size() / 2)) * 2];
    const auto matches = [&](const ImageDescCacheEntry& e) {
        return e.flags == flags && e.sharp == key;
    };
    const bool disabled = TlgToggle::Disabled(TlgToggle::ImageDescCache);
    ImageDescCacheEntry* slot = nullptr;
    if (!disabled && matches(set[0])) {
        slot = &set[0];
    } else if (!disabled && matches(set[1])) {
        slot = &set[1];
    } else {
        // Replace the least recently used way that no binding of this call points at.
        slot = set[0].last_use <= set[1].last_use ? &set[0] : &set[1];
        if (slot->pinned == bind_epoch) {
            slot = slot == &set[0] ? &set[1] : &set[0];
        }
        if (slot->pinned == bind_epoch) {
            slot = &image_desc_overflow.emplace_back();
        }
    }
    slot->last_use = ++desc_use_counter;
    auto& entry = *slot;
    if (entry.flags != flags || entry.sharp != key ||
        TlgToggle::Disabled(TlgToggle::ImageDescCache)) {
        entry.desc = VideoCore::TextureCache::ImageDesc{sharp, res};
        entry.sharp = key;
        entry.flags = flags;
        entry.found_generation = ~0ULL;
        entry.view_memo = {};
    }
    return entry;
}

namespace {
// tlg: TLG_ADDR_TEX_LOG=<hex guest address>: each time a shader binding that samples an image
// overlapping that address gets a different image (id, size, format), one line with the time.
// For aliased memory (one guest range used as several images): shows which one a pass reads.
// Bindings resolved through the texture-set memo are not seen: disable it (toggle bit 1<<45).
struct AddrTexLog {
    VAddr addr = [] {
        const char* env = std::getenv("TLG_ADDR_TEX_LOG");
        return env && *env ? VAddr(std::strtoull(env, nullptr, 16)) : VAddr(0);
    }();
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::mutex mutex;
    std::unordered_map<u64, u32> last; // (hash ^ binding) -> image id
    void Note(u64 hash, u32 binding, VideoCore::ImageId id, const VideoCore::Image& image,
              bool target) {
        const auto& info = image.info;
        if (addr < info.guest_address || addr >= info.guest_address + info.guest_size) {
            return;
        }
        std::scoped_lock lk{mutex};
        auto [it, fresh] = last.try_emplace(hash * 64 + binding, id.index);
        if (!fresh && it->second == id.index) {
            return;
        }
        it->second = id.index;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start).count();
        std::printf("GPU addrtex: %lld ms %s %016llx b%u -> image %u 0x%llx+0x%x %ux%u fmt %d mips %u\n",
                    (long long)ms, target ? "rt" : "tex", (unsigned long long)hash, binding,
                    id.index, (unsigned long long)info.guest_address, info.guest_size,
                    info.size.width, info.size.height, int(info.pixel_format),
                    info.resources.levels);
    }
};
AddrTexLog& AddrTex() {
    static AddrTexLog log;
    return log;
}
} // namespace

void Rasterizer::BindTextures(const Shader::Info& stage, const PreparedStage* prepared,
                              Shader::Backend::Bindings& binding, u32& write_index,
                              bool& barrier, bool on_helper) {
    const u32 first_image_idx = image_infos.size();
    TextureSet* set_slot = nullptr;
    if (!on_helper && BindTexturesFromSet(stage, prepared, first_image_idx, barrier, set_slot)) {
        // Descriptor writes: one per image (no mip arrays in a memoized set).
        for (u32 i = 0; i < stage.images.size(); ++i) {
            auto& set_write = set_writes[write_index++];
            set_write.dstSet = VK_NULL_HANDLE;
            set_write.dstBinding = binding.unified++;
            set_write.dstArrayElement = 0;
            set_write.descriptorCount = 1;
            set_write.descriptorType = vk::DescriptorType::eSampledImage;
            set_write.pImageInfo = &image_infos[first_image_idx + i];
            set_write.pBufferInfo = nullptr;
            set_write.pTexelBufferView = nullptr;
        }
        BindSamplers(stage, prepared, binding, write_index, set_slot);
        return;
    }
    image_bindings.clear();
    image_binding_entries.clear();
    image_desc_overflow.clear();
    image_desc_storage.clear();
    ++bind_epoch;
    // To emulate storing to explicit mip levels, build a descriptor array with each mip level.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;
    // TextureSetMemo: what this call resolves, to remember when the set qualifies.
    std::array<TextureSetEntry, TextureSet::MaxImages> resolved{};
    bool set_ok = set_slot != nullptr;
    // SampleSceneProxies: bindings that may read a reduced scene proxy instead of the native
    // image (normalized sampling only; see Shader::ImageResource::needs_native).
    boost::container::small_vector<bool, 16> binding_proxy_ok;
    const bool sample_proxies = !TlgToggle::Disabled(TlgToggle::SampleSceneProxies) &&
                                !on_helper && scene_targets->Reduced();

    boost::container::small_vector<u8, 16> binding_fb;
    Shader::RuntimeInfo fb_runtime{};
    bool fb_runtime_ready = false;
    for (u32 image_index = 0; image_index < stage.images.size(); ++image_index) {
        const auto& image_desc = stage.images[image_index];
        const auto tsharp =
            prepared ? prepared->image_sharps[image_index] : image_desc.GetSharp(stage);
        // tlg: a hash lookup per texture per draw for a diagnostic only.
        static const bool warn_meta = std::getenv("TLG_WARN_META_TEXTURE") != nullptr;
        if (warn_meta && texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings.emplace_back(VideoCore::ImageId{}, &image_desc_storage.emplace_back());
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(false);
            binding_fb.push_back(0);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        if (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !IsKnownFormat(data_fmt, num_fmt)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                        "data_format={}, num_format={}",
                        tsharp.Address(), tsharp.pitch, tsharp.width, static_cast<u32>(data_fmt),
                        static_cast<u32>(num_fmt));
            image_bindings.emplace_back(VideoCore::ImageId{}, &image_desc_storage.emplace_back());
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(false);
            binding_fb.push_back(0);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        // tlg (macOS): an own-pixel load of one of the draw's colour targets reads it through
        // framebuffer fetch (vk_fb_fetch.h); the same rule chose the shader's permutation.
        u8 fb_cb = 0;
        if (stage.hw_stage == Shader::HwStage::Fragment && image_desc.fb_fetchable && fb_mrt_mask &&
            FbFetch::Enabled()) {
            if (!fb_runtime_ready) {
                fb_runtime.hw_stage = Shader::HwStage::Fragment;
                FbFetch::FillTargets(Regs(), fb_mrt_mask, fb_runtime.hw.fs.fb_targets);
                fb_runtime_ready = true;
            }
            fb_cb = Shader::FbFetchTarget(image_desc, tsharp, fb_runtime);
        }
        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        // Same as image_desc.NumBindings(stage), without fetching the T# again.
        const u32 num_bindings =
            mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex
                ? static_cast<u32>(tsharp.last_level - tsharp.base_level + 1)
                : 1u;
        const bool proxy_candidate = sample_proxies && num_bindings == 1 &&
                                     !image_desc.needs_native && !image_desc.is_written &&
                                     mip_fallback_mode == Shader::MipStorageFallbackMode::None;

        auto& desc_entry =
            prepared ? CachedImageDescEntry(tsharp, image_desc, prepared->image_hashes[image_index])
                     : CachedImageDescEntry(tsharp, image_desc);
        // TLG_SCENE_DEBUG: proxied scene targets this binding reads at the native size, and why.
        const auto debug_native = [&](const VideoCore::Image& image) {
            if (!scene_debug_frame || proxy_candidate || !image.scene_proxy) return;
            std::printf("Scene native read: %s %016llx %s %ux%u needs_native %d written %d "
                        "bindings %u mip_fallback %d\n",
                        stage.sw_stage == Shader::SwStage::Compute ? "cs" : "gfx",
                        (unsigned long long)stage.pgm_hash,
                        vk::to_string(image.info.pixel_format).c_str(), image.info.size.width,
                        image.info.size.height, int(image_desc.needs_native),
                        int(image_desc.is_written), num_bindings, int(mip_fallback_mode));
        };
        for (auto i = 0; i < num_bindings; i++) {
            // tlg: a plain binding (no mip override) of the same T# resolves to the same image
            // while no image was registered or unregistered.
            if (mip_fallback_mode == Shader::MipStorageFallbackMode::None &&
                desc_entry.found_generation == texture_cache.RegistryGeneration() &&
                !TlgToggle::Disabled(TlgToggle::TextureBindingMemo)) {
                desc_entry.pinned = bind_epoch;
                auto& [image_id, _] =
                    image_bindings.emplace_back(desc_entry.found_id, &desc_entry.found_desc);
                image_binding_entries.push_back(&desc_entry);
                binding_proxy_ok.push_back(proxy_candidate);
                binding_fb.push_back(fb_cb);
                texture_cache.MarkFound(image_id);
                auto* image = &texture_cache.GetImage(image_id);
                if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                    image_id = depth_image_id;
                    image = &texture_cache.GetImage(image_id);
                }
                debug_native(*image);
                if (image->binding.is_bound) {
                    image->binding.force_general |= image_desc.is_written;
                }
                image->binding.is_bound = 1u;
                if (fb_cb) { image->binding.fb_fetch = 1u; } else { image->binding.fb_plain = 1u; }
                continue;
            }
            auto& desc = image_desc_storage.emplace_back(desc_entry.desc);
            auto& [image_id, _] = image_bindings.emplace_back(VideoCore::ImageId{}, &desc);
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(proxy_candidate);
            binding_fb.push_back(fb_cb);

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            const u64 generation = texture_cache.RegistryGeneration();
            image_id = texture_cache.FindImage(desc);
            if (mip_fallback_mode == Shader::MipStorageFallbackMode::None &&
                generation == texture_cache.RegistryGeneration() &&
                desc_entry.pinned != bind_epoch) {
                desc_entry.found_generation = generation;
                desc_entry.found_id = image_id;
                desc_entry.found_desc = desc;
                desc_entry.view_memo = {};
            }
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            debug_native(*image);
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
            if (fb_cb) { image->binding.fb_fetch = 1u; } else { image->binding.fb_plain = 1u; }
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // Second pass to re-bind images that were updated after binding
    for (u32 binding_index = 0; binding_index < image_bindings.size(); ++binding_index) {
        auto& [image_id, desc_ptr] = image_bindings[binding_index];
        auto* memo_entry = image_binding_entries[binding_index];
        const auto& desc = *desc_ptr;
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                auto& rebind_desc = image_desc_storage.emplace_back(desc);
                desc_ptr = &rebind_desc;
                image_id = texture_cache.FindImage(rebind_desc);
                memo_entry = nullptr;
                set_ok = false;
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            if (AddrTex().addr) [[unlikely]] {
                AddrTex().Note(stage.pgm_hash, binding_index, image_id, image, false);
            }
            // tlg: TLG_STALE_CHECK=1 (diagnostic): an image taken as clean whose guest memory no
            // longer hashes as at its last upload is stale: the write was not noticed.
            static const bool stale_check = std::getenv("TLG_STALE_CHECK") != nullptr;
            if (stale_check && !is_storage && image.stale_hash != 0 &&
                False(image.flags & VideoCore::ImageFlagBits::Dirty) &&
                False(image.flags & VideoCore::ImageFlagBits::GpuModified) &&
                !image.info.props.is_depth) {
                const u64 now = XXH3_64bits(std::bit_cast<const u8*>(image.info.guest_address),
                                            image.info.guest_size);
                if (now != image.stale_hash) {
                    static std::atomic<u32> found{0};
                    const u32 n = found.fetch_add(1, std::memory_order_relaxed);
                    if (n < 40 || (n & 1023) == 0) {
                        std::printf("Stale texture #%u: stage %llx binding %u: 0x%llx+0x%x %ux%u fmt %d tracked %d flags %x\n",
                                    n, (unsigned long long)stage.pgm_hash, binding_index,
                                    (unsigned long long)image.info.guest_address, image.info.guest_size,
                                    image.info.size.width, image.info.size.height,
                                    int(image.info.pixel_format), image.IsTracked() ? 1 : 0,
                                    u32(image.flags));
                        u32 changed = 0;
                        u64 first = ~0ull, last = 0;
                        for (u64 c = 0; c < image.stale_chunks.size(); ++c) {
                            const u64 at = c * 0x10000;
                            const u64 h = XXH3_64bits(std::bit_cast<const u8*>(image.info.guest_address + at),
                                                      std::min<u64>(0x10000, image.info.guest_size - at));
                            if (h != image.stale_chunks[c]) {
                                ++changed;
                                first = std::min(first, at);
                                last = at;
                                image.stale_chunks[c] = h;
                            }
                        }
                        if (!image.stale_copy.empty()) {
                            // What changed: the first differing dwords, and how much in all.
                            const auto* was = reinterpret_cast<const u32*>(image.stale_copy.data());
                            const auto* is = std::bit_cast<const u32*>(image.info.guest_address);
                            const u64 dwords = image.stale_copy.size() / 4;
                            u64 diffs = 0;
                            std::printf("  old->new:");
                            for (u64 d = 0; d < dwords; ++d) {
                                if (was[d] != is[d]) {
                                    if (diffs++ < 8) {
                                        std::printf(" +%llx:%08x->%08x", (unsigned long long)d * 4, was[d], is[d]);
                                    }
                                }
                            }
                            std::printf(" (%llu dwords differ)\n", (unsigned long long)diffs);
                            std::memcpy(image.stale_copy.data(), is, dwords * 4);
                        }
                        std::printf("  changed %u of %zu chunks, first +0x%llx last +0x%llx; tracked +0x%llx..+0x%llx; mips %u layers %u\n",
                                    changed, image.stale_chunks.size(), (unsigned long long)first,
                                    (unsigned long long)last,
                                    (unsigned long long)(image.track_addr - image.info.guest_address),
                                    (unsigned long long)(image.track_addr_end - image.info.guest_address),
                                    image.info.resources.levels, image.info.resources.layers);
                    }
                    image.stale_hash = now;
                    // TLG_STALE_CHECK=2: also repair (a test of the diagnosis; hashing every bind
                    // is slow): the image is marked modified and uploaded again now.
                    static const bool repair = std::getenv("TLG_STALE_CHECK")[0] == '2';
                    if (repair) {
                        image.flags |= VideoCore::ImageFlagBits::CpuDirty;
                        texture_cache.UpdateImage(image_id);
                    }
                }
            }
            auto& image_view = texture_cache.FindTexture(
                image_id, desc, memo_entry ? &memo_entry->view_memo : nullptr, !on_helper);
            const auto binding = image.binding;

            // tlg: a proxied scene image sampled with normalized coordinates reads the proxy:
            // no resample to the native size (the native image is not touched).
            if (binding_proxy_ok[binding_index] && !is_storage && !binding.force_general &&
                !binding.is_target &&
                !(upscaler->Enabled() && upscaler->RedirectsSampled(image_id))) {
                if (const auto proxy = scene_targets->SampleProxy(image, desc.view_info)) {
                    image.usage.texture = 1u;
                    image_infos.emplace_back(VK_NULL_HANDLE, proxy->view, proxy->layout);
                    if (set_ok && binding_index < resolved.size()) {
                        resolved[binding_index] = {image_id, proxy->view, image.backing,
                                                   desc.view_info.range, true};
                    }
                    ++proxy_samples;
                    continue;
                }
            }
            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            if (binding_index < binding_fb.size() && binding_fb[binding_index] && binding.is_target &&
                !binding.force_general) {
                // tlg (macOS): read in the pass through framebuffer fetch: no layout change, no
                // barrier (the attachment keeps its layout; the descriptor is not read).
            } else if ((binding.force_general || binding.is_target) && !image.info.props.is_depth) {
                if (instance.IsAttachmentFeedbackLoopLayoutSupported() && image.binding.is_target) {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT,
                        vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
                } else {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
                }
            } else {
                if (is_storage) {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    barrier |= runtime.Transit(
                        &image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead, desc.view_info.range);
                }
            }
            image.sampled_range = desc.view_info.range;
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;
            if (FrameCapture::Active()) {
                FrameCapture::Sampled(image.info, is_storage);
            }

            vk::ImageView view = *image_view.image_view;
            vk::ImageLayout layout = image.backing->state.layout;
            if (upscaler->Enabled()) {
                // tlg: the display pass reads the upscaled frame (scaled presets).
                upscaler->RedirectSampled(image_id, image_view.info, view, layout);
            }
            image_infos.emplace_back(VK_NULL_HANDLE, view, layout);
            if (set_ok && binding_index < resolved.size()) {
                resolved[binding_index] = {image_id, *image_view.image_view, image.backing,
                                           desc.view_info.range};
                set_ok = !is_storage && !binding.force_general && !binding.is_target;
            }
        }
    }
    if (set_ok && image_bindings.size() == stage.images.size()) {
        // Remember the set: the same T#s resolve the same way while no image is (un)registered.
        set_slot->generation = texture_cache.RegistryGeneration();
        set_slot->scene_generation = scene_targets->Generation();
        set_slot->count = static_cast<u32>(stage.images.size());
        std::copy_n(resolved.begin(), set_slot->count, set_slot->entries.begin());
    } else if (set_slot) {
        set_slot->key = 0;
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& desc = *image_bindings[image_binding_idx].second;
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];
        set_write.pBufferInfo = nullptr;
        set_write.pTexelBufferView = nullptr;

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    BindSamplers(stage, prepared, binding, write_index, set_slot);
}

u64 Rasterizer::SamplerSetKey(const Shader::Info& stage, const PreparedStage* prepared) const {
    // GetSampler's inputs: the S#s, the border color base, the per-sampler depth flags and the
    // LOD bias of this draw.
    const u32 count = static_cast<u32>(stage.samplers.size());
    u64 key =
        count ? XXH3_64bits(prepared->sampler_sharps, count * sizeof(AmdGpu::Sampler)) : 0;
    const u64 border_color_base = Regs().ta_bc_base.base_addr;
    key = HashCombine(key, border_color_base);
    key = HashCombine(key, u64(std::bit_cast<u32>(sampler_lod_bias)));
    u32 depth_flags = 0;
    for (u32 i = 0; i < count; ++i) {
        depth_flags |= u32(stage.samplers[i].is_depth) << i;
    }
    return HashCombine(key, u64(depth_flags));
}

void Rasterizer::BindSamplers(const Shader::Info& stage, const PreparedStage* prepared,
                              Shader::Backend::Bindings& binding, u32& write_index,
                              TextureSet* set_slot) {
    const u32 count = static_cast<u32>(stage.samplers.size());
    // tlg (SamplerSetMemo): the set these textures were bound from resolved these samplers
    // already, so reuse their handles instead of a hash and a map lookup per sampler. The LRU
    // entries are still marked used, under TouchSamplers' one lock: the collector evicts by age,
    // and a command recorded with a sampler is still in flight. TouchSamplers also drops the
    // memo when a sampler was destroyed (its LRU ids may be gone or reused).
    bool memo = false;
    bool store = false;
    if (set_slot != nullptr && set_slot->key != 0 && prepared != nullptr &&
        !TlgToggle::Disabled(TlgToggle::SamplerSetMemo)) {
        const u64 generation = texture_cache.SamplerGeneration();
        const u64 key = SamplerSetKey(stage, prepared);
        memo = set_slot->sampler_count == count && set_slot->sampler_key == key &&
               set_slot->sampler_generation == generation &&
               texture_cache.TouchSamplers(set_slot->sampler_lru_ids.data(), count, generation);
        store = !memo;
        if (store) {
            // Resolve below and remember this set's samplers for the next hit.
            set_slot->sampler_key = key;
            set_slot->sampler_generation = generation;
            set_slot->sampler_count = count;
        }
    }
    for (u32 sampler_index = 0; sampler_index < count; ++sampler_index) {
        const auto& sampler = stage.samplers[sampler_index];
        vk::Sampler vk_sampler;
        if (memo) {
            vk_sampler = set_slot->samplers[sampler_index];
        } else {
            const auto ssharp =
                prepared ? prepared->sampler_sharps[sampler_index] : sampler.GetSharp(stage);
            vk_sampler = texture_cache.GetSampler(
                ssharp, Regs().ta_bc_base, sampler.is_depth,
                sampler.is_depth ? 0.0f : sampler_lod_bias,
                store ? &set_slot->sampler_lru_ids[sampler_index] : nullptr);
            if (store) {
                set_slot->samplers[sampler_index] = vk_sampler;
            }
        }
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
        set_write.pBufferInfo = nullptr;
        set_write.pTexelBufferView = nullptr;
    }
}

Rasterizer::BeginSignature Rasterizer::MakeBeginSignature(const GraphicsPipeline* pipeline) const {
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = Regs();
    BeginSignature sig;
    const u32 num_color = std::bit_width(key.mrt_mask);
    for (u32 cb = 0; cb < num_color && cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        sig.ids[cb] = cb_descs[cb].first;
        if (cb_descs[cb].first) {
            sig.views[cb] = cb_descs[cb].second.view_info;
        }
        sig.color_samples[cb] = key.color_samples[cb];
    }
    sig.ids[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.first;
    if (db_desc.first) {
        sig.views[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.second.view_info;
    }
    sig.mrt_mask = key.mrt_mask;
    sig.num_samples = key.num_samples;
    sig.motion = key.motion_vectors;
    sig.scene_started = scene_started;
    sig.raster_scaling = upscaler->RasterScaling();
    sig.upscaler_state = upscaler->RedirectState();
    sig.generation = texture_cache.RegistryGeneration();
    std::memcpy(&sig.depth_control, &regs.depth_control, sizeof(u32));
    sig.depth_valid = regs.depth_buffer.DepthValid();
    sig.stencil_valid = regs.depth_buffer.StencilValid();
    return sig;
}

bool Rasterizer::BindTexturesFromSet(const Shader::Info& stage, const PreparedStage* prepared,
                                     u32 first_image_idx, bool& barrier, TextureSet*& slot) {
    const u32 count = static_cast<u32>(stage.images.size());
    if (!prepared || count == 0 || count > TextureSet::MaxImages ||
        TlgToggle::Disabled(TlgToggle::TextureSetMemo) || FrameCapture::Active()) {
        return false;
    }
    for (const auto& image : stage.images) {
        if (image.is_written || image.mip_fallback_mode != Shader::MipStorageFallbackMode::None) {
            return false;
        }
    }
    u64 key = XXH3_64bits_withSeed(prepared->image_hashes, count * sizeof(u64),
                                   reinterpret_cast<u64>(&stage));
    key |= 1; // 0 marks an empty slot
    auto& set = texture_sets[key % texture_sets.size()];
    slot = &set;
    const u64 generation = texture_cache.RegistryGeneration();
    const bool match = set.key == key && set.stage == &stage && set.count == count &&
                       set.generation == generation &&
                       set.scene_generation == scene_targets->Generation() &&
                       std::equal(set.hashes.begin(), set.hashes.begin() + count,
                                  prepared->image_hashes);
    if (!match) {
        ++texture_set_why[set.key != key ? 0 : set.generation != generation ? 1 : 3];
        ++texture_set_misses;
        set.key = key;
        set.stage = &stage;
        set.count = 0;
        set.generation = ~0ull;
        std::copy_n(prepared->image_hashes, count, set.hashes.begin());
        return false;
    }
    // Every image still has the backing its view belongs to and needs no refresh.
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = set.entries[i];
        if (!entry.id) {
            continue;
        }
        const auto& image = texture_cache.GetImage(entry.id);
        // Proxy entries need a current proxy; native entries of a proxied image go through
        // BindTextures, which may sample the proxy instead.
        const bool proxies_on = !TlgToggle::Disabled(TlgToggle::SampleSceneProxies);
        if (image.backing != entry.backing || image.binding.needs_rebind ||
            image.binding.is_target || !texture_cache.IsUpToDate(entry.id) ||
            (entry.proxy ? !proxies_on ||
                               !scene_targets->ProxyCurrent(image, entry.range.base.level,
                                                            entry.range.extent.levels)
                         : image.scene_proxy && proxies_on) ||
            (upscaler->Enabled() && upscaler->RedirectsSampled(entry.id))) {
            ++texture_set_why[2];
            ++texture_set_misses;
            set.generation = ~0ull;
            return false;
        }
    }
    ++texture_set_hits;
    // `slot` stays the hit set: BindSamplers reuses the samplers it resolved (SamplerSetMemo).
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = set.entries[i];
        if (!entry.id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            continue;
        }
        texture_cache.MarkFound(entry.id);
        auto& image = texture_cache.GetImage(entry.id);
        image.binding.is_bound = 1u;
        bound_images.emplace_back(entry.id);
        image.usage.texture = 1u;
        if (entry.proxy) {
            image_infos.emplace_back(VK_NULL_HANDLE, entry.view,
                                     scene_targets->PrepareSample(image, entry.range.base.level));
            ++proxy_samples;
            continue;
        }
        const auto new_layout = image.info.props.is_depth
                                    ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                    : vk::ImageLayout::eShaderReadOnlyOptimal;
        barrier |= runtime.Transit(&image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                                   vk::AccessFlagBits2::eShaderRead, entry.range);
        image_infos.emplace_back(VK_NULL_HANDLE, entry.view, image.backing->state.layout);
    }
    (void)first_image_idx;
    return true;
}

namespace {
bool DrawCountLog() {
    static const bool on = [] {
        const char* env = std::getenv("TLG_DRAW_COUNT_LOG");
        return env && env[0] == '1';
    }();
    return on;
}
} // namespace

// tlg: TLG_DRAW_COUNT_LOG=1: one line per flip (steady-clock ms, comparable with the shot lines):
// all draws, draws into 3+ colour targets (the G-buffer pass) and draws with a depth target.
// Tells a frame the game sent without its scenery from one whose draws went missing here.
void Rasterizer::CountDraw(const RenderState& state, const GraphicsPipeline* pipeline) {
    if (!DrawCountLog()) [[likely]] {
        return;
    }
    ++frame_draws;
    const auto& regs = Regs();
    const VAddr cb0 = regs.color_buffers[0] ? regs.color_buffers[0].Address() : 0;
    const VAddr db = state.depth_stencil_attachment.image_view ? regs.depth_buffer.DepthAddress() : 0;
    ++frame_targets[{cb0, db, u32(state.num_color_attachments), state.width, state.height}];
    // Pass sequence: a new entry whenever the targets change (d = depth cleared at pass start,
    // c = colour 0 cleared).
    {
        const bool dclear = state.depth_stencil_attachment.image_view && state.depth_stencil_attachment.depth_clear;
        const bool cclear = state.color_attachments[0].image_view && state.color_attachments[0].is_clear;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%llx/%llx/%u%s%s", (unsigned long long)(cb0 >> 16),
                      (unsigned long long)(db >> 16), u32(state.num_color_attachments),
                      dclear ? "d" : "", cclear ? "c" : "");
        if (frame_passes.empty() || frame_passes.back().first != buf) {
            frame_passes.emplace_back(buf, 0);
        }
        ++frame_passes.back().second;
    }
    if (cb0 == 0x70fcdf0000ull) { // tlg: the bridge's heavy frames (temporary)
        const auto stages = pipeline->GetStages();
        const u64 vs = stages[u32(Shader::SwStage::Vertex)] ? stages[u32(Shader::SwStage::Vertex)]->pgm_hash : 0;
        const u64 ps = stages[u32(Shader::SwStage::Fragment)] ? stages[u32(Shader::SwStage::Fragment)]->pgm_hash : 0;
        ++frame_shaders[{vs, ps}];
    }
    frame_mrt_draws += state.num_color_attachments >= 3 ? 1 : 0;
    frame_depth_draws += state.depth_stencil_attachment.image_view ? 1 : 0;
}

void Rasterizer::LogFrameDraws() {
    if (!DrawCountLog()) [[likely]] {
        return;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::printf("GPU frame draws: %lld ms %u draws, %u mrt, %u depth\n", (long long)ms,
                frame_draws, frame_mrt_draws, frame_depth_draws);
    // Frames with an unusual number of draws: their targets (cb0, depth, colour count, size).
    if (frame_draws > 3000) {
        for (const auto& [key, n] : frame_targets) {
            if (n >= 50) {
                std::printf("GPU   targets: %u draws cb0 0x%llx depth 0x%llx %u cb %ux%u\n", n,
                            (unsigned long long)std::get<0>(key), (unsigned long long)std::get<1>(key),
                            std::get<2>(key), std::get<3>(key), std::get<4>(key));
            }
        }
    } else if (frame_draws > 0 && ++normal_frames % 300 == 1) {
        for (const auto& [key, n] : frame_targets) {
            if (n >= 20) {
                std::printf("GPU   normal targets: %u draws cb0 0x%llx depth 0x%llx %u cb %ux%u\n", n,
                            (unsigned long long)std::get<0>(key), (unsigned long long)std::get<1>(key),
                            std::get<2>(key), std::get<3>(key), std::get<4>(key));
            }
        }
    }
    if (frame_draws > 3000 || (frame_draws > 0 && normal_frames % 300 == 1)) {
        for (const auto& [key, n] : frame_shaders) {
            if (n >= 30) {
                std::printf("GPU   %s shaders: %u draws vs %llx ps %llx\n",
                            frame_draws > 3000 ? "heavy" : "normal", n,
                            (unsigned long long)key.first, (unsigned long long)key.second);
            }
        }
    }
    // TLG_CAPTURE_HEAVY=1: one frame capture of a normal frame (700-2000 draws), then one of the
    // frame after a heavy one (they come in runs during camera turns).
    static const bool capture_heavy = [] {
        const char* env = std::getenv("TLG_CAPTURE_HEAVY");
        return env && env[0] == '1';
    }();
    static int captures = 0;
    if (capture_heavy) {
        if (captures == 0 && frame_draws >= 700 && frame_draws <= 2000 && ++capture_wait > 1500) {
            std::printf("GPU: capturing a normal frame after %u draws\n", frame_draws);
            FrameCapture::Request();
            captures = 1;
        } else if (captures == 1 && frame_draws > 4000 && ++capture_wait > 1800) {
            std::printf("GPU: capturing the frame after a heavy one (%u draws)\n", frame_draws);
            FrameCapture::Request();
            captures = 2;
        }
    }
    static u32 heavy_printed = 0, normal_printed = 0;
    if ((frame_draws > 3000 && heavy_printed++ < 6) ||
        (frame_draws > 0 && frame_draws < 3000 && normal_frames > 200 && normal_printed++ < 3)) {
        std::string line;
        for (const auto& [name, n] : frame_passes) {
            line += " " + name + "x" + std::to_string(n);
        }
        std::printf("GPU   passes (%s, %u draws):%s\n", frame_draws > 3000 ? "heavy" : "normal",
                    frame_draws, line.c_str());
    }
    frame_passes.clear();
    frame_shaders.clear();
    frame_targets.clear();
    frame_draws = frame_mrt_draws = frame_depth_draws = 0;
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    // tlg: RenderStateMemo (see BeginMemo).
    const bool memo_on = !TlgToggle::Disabled(TlgToggle::RenderStateMemo) && !FrameCapture::Active();
    if (TlgStats::enabled && ((begin_memo_hits + begin_memo_misses) & 0x3FFFF) == 0x3FFFF) {
        std::printf("Render state memo: %llu hits, %llu misses; texture sets: %llu hits, %llu "
                    "misses\n",
                    static_cast<unsigned long long>(begin_memo_hits),
                    static_cast<unsigned long long>(begin_memo_misses),
                    static_cast<unsigned long long>(texture_set_hits),
                    static_cast<unsigned long long>(texture_set_misses));
        std::printf("  texture set misses: other key %llu, generation %llu, image check %llu, "
                    "other %llu\n",
                    (unsigned long long)texture_set_why[0], (unsigned long long)texture_set_why[1],
                    (unsigned long long)texture_set_why[2], (unsigned long long)texture_set_why[3]);
        std::printf("  scene proxies sampled directly: %llu bindings\n",
                    static_cast<unsigned long long>(proxy_samples));
        proxy_samples = 0;
        texture_set_why = {};
        begin_memo_hits = begin_memo_misses = texture_set_hits = texture_set_misses = 0;
    }
    BeginSignature signature;
    if (memo_on) {
        signature = MakeBeginSignature(pipeline);
        const auto& regs = Regs();
        bool usable = begin_memo.valid && scheduler.IsRenderingWith(begin_memo.state) &&
                      !regs.depth_render_control.depth_clear_enable &&
                      !regs.depth_render_control.stencil_clear_enable &&
                      signature == begin_memo.signature;
        for (const auto id : signature.ids) {
            if (usable && id) {
                const auto& image = texture_cache.GetImage(id);
                usable = !image.binding.is_bound && !image.binding.needs_rebind;
            }
        }
        if (usable) {
            ++begin_memo_hits;
            attachment_feedback_loop = false;
            push_data.scene_size = begin_memo.scene_size;
            target_scale = begin_memo.target_scale;
            return begin_memo.state;
        }
        ++begin_memo_misses;
    }
    begin_memo.valid = false;
    RenderState state = BeginRenderingFull(pipeline);
    if (memo_on && !attachment_feedback_loop) {
        // (is_clear of the depth attachment shares its bytes with has_depth.)
        bool clears = state.depth_stencil_attachment.depth_clear ||
                      state.depth_stencil_attachment.stencil_clear;
        for (u32 i = 0; i < state.num_color_attachments; ++i) {
            clears |= state.color_attachments[i].is_clear != 0;
        }
        if (!clears) {
            // Recomputed after the full path: it may have started the scene (scene_started).
            begin_memo = {true, MakeBeginSignature(pipeline), state, push_data.scene_size,
                          target_scale};
        }
    }
    return state;
}

RenderState Rasterizer::BeginRenderingFull(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    // tlg: frame analyzer census of this pass (sample counts and path, stencil, clears).
    const bool census_on = FrameCapture::Active();
    PassCensus::State census;
    using VulkanUpscalerTarget = TemporalUpscaler::Target;
    VulkanUpscalerTarget redirect{};
    bool color_redirected = false, depth_redirected = false;
    std::pair<u32, u32> guest_extent{1, 1};
    std::pair<vk::ImageView, vk::ImageLayout> original_color{};
    const auto& regs = Regs();
    const auto& key = pipeline->GetGraphicsKey();
    if (std::popcount(key.mrt_mask & 0x7f) >= 5 && db_desc.first &&
        scene_targets->EligibleScene(texture_cache.GetImage(db_desc.first))) scene_started = true;
    // A proxy attachment cannot represent MSAA or a feedback loop that reads the
    // same image through the guest's native descriptor during this draw.
    bool reduced = scene_started && upscaler->RasterScaling() && key.num_samples == 1;
    // TLG_SCENE_DEBUG=<file>: once the file exists, why the passes of one frame keep the
    // native size (see NoteFrameStart).
    const bool debug_pass = scene_debug_frame && scene_started;
    std::string why;
    if (debug_pass) {
        why = fmt::format("pass vs {:08x} ps {:08x} mrt {:#x} samples {} raster_scaling {}:",
                          pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                          key.mrt_mask ? pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash
                                       : 0,
                          key.mrt_mask, u32(key.num_samples), upscaler->RasterScaling());
    }
    // Only this pass's attachments: slots past the mask's width keep earlier passes' targets,
    // often the G-buffer images the lighting passes sample (bound), which kept those native.
    const u32 num_attachments = TlgToggle::Disabled(TlgToggle::SceneAttachmentsOnly)
                                    ? u32(cb_descs.size())
                                    : u32(std::bit_width(key.mrt_mask));
    // All attachments of a reduced pass share one proxy size (scene or half resolution).
    std::optional<SceneResolution::Size> pass_size;
    const auto same_size = [&](const VideoCore::Image& image, u32 level) {
        const auto proxy = scene_targets->ProxySize(image, level);
        if (!pass_size) pass_size = proxy;
        return *pass_size == proxy;
    };
    for (u32 cb = 0; cb < num_attachments; ++cb) {
        const auto& [id, desc] = cb_descs[cb];
        if (id) {
            const auto& image = texture_cache.GetImage(id);
            // Mip levels of eligible chains have their own proxies (SceneTargets::Get).
            if (!scene_targets->Eligible(image) || image.binding.is_bound ||
                image.binding.needs_rebind || desc.view_info.range.base.layer ||
                !same_size(image, desc.view_info.range.base.level)) reduced = false;
            if (debug_pass) {
                why += fmt::format(" [{} {}x{} eligible {} bound {} rebind {} level {} layer {}]",
                                   vk::to_string(image.info.pixel_format), image.info.size.width,
                                   image.info.size.height, scene_targets->Eligible(image),
                                   u32(image.binding.is_bound), u32(image.binding.needs_rebind),
                                   desc.view_info.range.base.level,
                                   desc.view_info.range.base.layer);
            }
        }
    }
    if (db_desc.first && (!scene_targets->Eligible(texture_cache.GetImage(db_desc.first)) ||
                          !same_size(texture_cache.GetImage(db_desc.first), 0))) {
        reduced = false;
    }
    if (debug_pass) {
        if (db_desc.first) {
            const auto& depth = texture_cache.GetImage(db_desc.first);
            why += fmt::format(" depth [{} {}x{} eligible {}]",
                               vk::to_string(depth.info.pixel_format), depth.info.size.width,
                               depth.info.size.height, scene_targets->Eligible(depth));
        }
        std::printf("Scene pass: reduced %d %s\n", reduced, why.c_str());
    }
    push_data.scene_size = reduced ? SceneResolution::Pack(scene_targets->Size()) : 0;
    if (TlgStats::enabled) {
        TlgStats::reduced_draws.fetch_add(reduced, std::memory_order_relaxed);
        TlgStats::scene_draws.fetch_add(scene_started, std::memory_order_relaxed);
    }
    RenderState state;
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            last_targets[cb].generation = ~0ULL; // FindImage may rewrite the description
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        if (AddrTex().addr) [[unlikely]] {
            AddrTex().Note(pipeline && pipeline->GetStages()[u32(Shader::SwStage::Fragment)] ? pipeline->GetStages()[u32(Shader::SwStage::Fragment)]->pgm_hash : 0, cb,
                           image_id, *image, true);
        }
        texture_cache.UpdateImage(image_id);
        runtime.SetBackingSamples(image, key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (reduced) {
            // The native image is resolved lazily when a shader/copy actually reads it.
        } else if (image->binding.is_bound &&
                   !(image->binding.fb_fetch && !image->binding.fb_plain)) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            // tlg: TLG_FEEDBACK_STATS=1: every 5 s, colour feedback draws per fragment shader, and
            // whether the sampled range overlaps the target's level/layer (else: no real feedback).
            static const bool feedback_stats = [] {
                const char* env = std::getenv("TLG_FEEDBACK_STATS");
                return env && *env == '1';
            }();
            if (feedback_stats) [[unlikely]] {
                static std::map<u64, std::pair<u32, u32>> per_ps;
                static auto last = std::chrono::steady_clock::now();
                const auto& r = image->sampled_range;
                const bool level_overlap = mip >= r.base.level && mip < r.base.level + r.extent.levels;
                const bool layer_overlap = slice >= r.base.layer && slice < r.base.layer + r.extent.layers;
                const auto* fs = pipeline ? pipeline->GetStages()[u32(Shader::SwStage::Fragment)] : nullptr;
                // Key: shader hash, low 4 bits replaced by this target's colour write mask.
                const u64 key_ps = ((fs ? fs->pgm_hash : 0) << 8) | (regs.color_target_mask.GetMask(cb) & 0xf) |
                                   (u64(cb) << 4);
                auto& [overlap, disjoint] = per_ps[key_ps];
                (level_overlap && layer_overlap ? overlap : disjoint)++;
                // Once per shader and target: why framebuffer fetch did or did not take it.
                static std::set<u64> explained;
                if (fs && explained.insert(key_ps).second && explained.size() < 64) {
                    const auto cb_sw = col_buf.Swizzle();
                    for (const auto& desc : fs->images) {
                        const auto sharp = desc.GetSharp(*fs);
                        if (sharp.Address() != col_buf.Address()) {
                            continue;
                        }
                        std::printf("Feedback ps %llx cb%u: fb_fetchable %d written %d array %d depth %d type %u "
                                    "base_level %u sel %u%u%u%u fmt %u/%u target fmt %u/%u target swizzle %u%u%u%u "
                                    "mrt %x fb on %d\n",
                                    static_cast<unsigned long long>(fs->pgm_hash), cb, int(desc.fb_fetchable),
                                    int(desc.is_written), int(desc.is_array), int(desc.is_depth),
                                    u32(sharp.GetType()), u32(sharp.base_level), u32(sharp.dst_sel_x),
                                    u32(sharp.dst_sel_y), u32(sharp.dst_sel_z), u32(sharp.dst_sel_w),
                                    u32(sharp.GetDataFmt()), u32(sharp.GetNumberFmt()), u32(col_buf.GetDataFmt()),
                                    u32(col_buf.GetNumberFmt()), u32(cb_sw.r), u32(cb_sw.g), u32(cb_sw.b),
                                    u32(cb_sw.a), u32(pipeline->GetGraphicsKey().mrt_mask), int(FbFetch::Enabled()));
                    }
                }
                if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
                    last = std::chrono::steady_clock::now();
                    std::printf("Feedback draws (5 s) per ps: (hash<<8 | cb<<4 | write mask) overlapping / disjoint ranges, target %ux%u:",
                                image->info.size.width, image->info.size.height);
                    for (const auto& [ps, n] : per_ps) {
                        std::printf(" %llx %u/%u", static_cast<unsigned long long>(ps), n.first, n.second);
                    }
                    std::printf("\n");
                    per_ps.clear();
                }
            }
            runtime.FlushBarriers();
            needs_barrier |=
                runtime.Transit(image,
                                instance.IsAttachmentFeedbackLoopLayoutSupported()
                                    ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                    : vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                vk::AccessFlagBits2::eColorAttachmentWrite |
                                    vk::AccessFlagBits2::eColorAttachmentRead);
            attachment_feedback_loop = true;
        } else {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             vk::AccessFlagBits2::eColorAttachmentWrite |
                                                 vk::AccessFlagBits2::eColorAttachmentRead,
                                             desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        if (reduced) {
            const auto target = scene_targets->Attachment(image_id, desc.view_info);
            attachment.image_view = target.view;
            attachment.image_layout = target.layout;
        }
        image->usage.render_target = 1u;
        if (cb == 0 && upscaler->Enabled()) {
            color_redirected = upscaler->RedirectColor(image_id, desc.view_info, redirect);
            if (color_redirected) {
                guest_extent = {image->info.size.width, image->info.size.height};
                original_color = {attachment.image_view, attachment.image_layout};
                attachment.image_view = redirect.view;
                attachment.image_layout = redirect.layout;
            }
        }
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    // tlg: TLG_MAC_DEPTH_FEEDBACK=detach (diagnostic): a depth buffer the pass also samples is
    // not attached when the pass does not write depth (Metal has no read-only depth attachment:
    // sampling it while attached is a hazard on tile-based GPUs).
    static const bool detach_depth_feedback = [] {
        const char* env = std::getenv("TLG_MAC_DEPTH_FEEDBACK");
        return env && !std::strcmp(env, "detach");
    }();
    if (detach_depth_feedback && db_desc.first && !regs.depth_control.depth_write_enable &&
        texture_cache.GetImage(db_desc.first).binding.is_bound) {
        db_desc.first = {};
    }
    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        // tlg: a depth clear (the DB clear bit or a fast-cleared HTILE) becomes the pass's
        // load-op clear only in depth-only passes. ISS fast-clears the HTILE of its depth buffer
        // and then draws a full-screen colour pass sampling that depth (#47 in the bridge
        // captures): a pass-wide clear there wiped the depth before lighting, which then lit
        // every pixel as sky (scenery-less, foggy frames while the camera turns). Default on;
        // TLG_DEPTH_CLEAR_COLOR=1 restores the old behaviour.
        static const bool clear_with_color = [] {
            const char* env = std::getenv("TLG_DEPTH_CLEAR_COLOR");
            return env && env[0] == '1';
        }();
        const bool depth_only =
            state.num_color_attachments == 0 ||
            std::none_of(state.color_attachments.begin(),
                         state.color_attachments.begin() + state.num_color_attachments,
                         [](const RenderAttachment& a) { return bool(a.image_view); });
        const bool register_clear = regs.depth_render_control.depth_clear_enable &&
                                    regs.depth_control.depth_enable &&
                                    regs.depth_control.depth_write_enable;
        const bool meta_clear = texture_cache.IsMetaCleared(htile_address, slice);
        const bool wanted_clear = register_clear || meta_clear;
        const bool is_depth_clear = wanted_clear && (clear_with_color || depth_only);
        if (wanted_clear && !is_depth_clear) {
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                std::printf("GPU: depth clear (%s) not applied to a pass with colour targets\n",
                            meta_clear ? "htile" : "register");
            }
        }
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        // tlg: a stencil test that changes nothing (write mask 0 or every op Keep) keeps the
        // read-only depth/stencil layout. Otherwise the depth buffer, sampled in the same frame,
        // switches DepthReadOnlyStencilAttachment <-> DepthStencilReadOnly and each switch ends
        // the render pass (~1,700 a second on the bridge). TLG_STENCIL_RO=0: as before.
        static const bool stencil_ro = [] {
            const char* env = std::getenv("TLG_STENCIL_RO");
            return !(env && *env == '0');
        }();
        const auto stencil_changes = [&] {
            const auto& sc = regs.stencil_control;
            using SF = AmdGpu::StencilFunc;
            const bool front = regs.stencil_ref_front.stencil_write_mask != 0 &&
                               (sc.stencil_fail_front != SF::Keep ||
                                sc.stencil_zpass_front != SF::Keep ||
                                sc.stencil_zfail_front != SF::Keep);
            const bool back = regs.depth_control.backface_enable &&
                              regs.stencil_ref_back.stencil_write_mask != 0 &&
                              (sc.stencil_fail_back != SF::Keep ||
                               sc.stencil_zpass_back != SF::Keep ||
                               sc.stencil_zfail_back != SF::Keep);
            return front || back || is_stencil_clear;
        };
        const bool stencil_write = has_stencil && regs.depth_control.stencil_enable &&
                                   !desc.view_info.is_storage &&
                                   (!stencil_ro || stencil_changes());
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        if (census_on) {
            census.depth_samples = u8(regs.depth_buffer.NumSamples());
            census.stencil_test = regs.depth_control.stencil_enable;
            census.stencil_write = stencil_write;
            census.stencil_clear = is_stencil_clear;
            census.depth_clear = !is_depth_clear ? PassCensus::DepthClear::None
                                 : meta_clear
                                     ? PassCensus::DepthClear::Htile
                                     : PassCensus::DepthClear::Register;
            census.depth_layout = vk::to_string(new_layout);
            census.depth_size = {image.info.size.width, image.info.size.height};
        }
        if (!reduced) needs_barrier |= runtime.Transit(&image, new_layout,
                                         vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                             vk::PipelineStageFlagBits2::eLateFragmentTests,
                                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                             vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                                         desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};
        attachment.is_clear = 0;

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        if (reduced) {
            const auto target = scene_targets->Attachment(image_id, desc.view_info);
            attachment.image_view = target.view;
            attachment.image_layout = target.layout;
        }
        image.usage.depth_target = true;
        if (upscaler->Enabled()) {
            VulkanUpscalerTarget depth_redirect;
            depth_redirected = upscaler->RedirectDepth(image_id, depth_redirect);
            if (depth_redirected) {
                guest_extent = {image.info.size.width, image.info.size.height};
                attachment.image_view = depth_redirect.view;
                attachment.image_layout = depth_redirect.layout;
                redirect = depth_redirect;
            }
        }
    } else {
        state.depth_stencil_attachment = {};
    }

    // tlg: a pass drawn into the upscaler's output-size images (UI, display pass): every
    // attachment must be redirected, viewports and scissors are scaled.
    target_scale = {1.0f, 1.0f};
    if (color_redirected || depth_redirected) {
        u32 color_targets = 0;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            color_targets += state.color_attachments[cb].image_view ? 1 : 0;
        }
        const bool has_depth = bool(db_desc.first);
        if ((color_targets > 0 && !color_redirected) || (has_depth && !depth_redirected) ||
            color_targets > 1) {
            static u32 warned = 0;
            if (warned < 8) {
                ++warned;
                const auto describe = [&](VideoCore::ImageId id) {
                    if (!id) {
                        return std::string{"-"};
                    }
                    const auto& info = texture_cache.GetImage(id).info;
                    return fmt::format("{:#x} {}x{} {}", info.guest_address, info.size.width,
                                       info.size.height, vk::to_string(info.pixel_format));
                };
                std::printf("Upscaler: pass not redirected (color %s: %s, depth %s: %s, %u "
                            "targets)\n",
                            color_redirected ? "yes" : "no", describe(cb_descs[0].first).c_str(),
                            depth_redirected ? "yes" : "no", describe(db_desc.first).c_str(),
                            state.num_color_attachments);
            }
            if (color_redirected) {
                state.color_attachments[0].image_view = original_color.first;
                state.color_attachments[0].image_layout = original_color.second;
            }
            if (depth_redirected) {
                auto& depth_image = texture_cache.GetImage(db_desc.first);
                state.depth_stencil_attachment.image_view =
                    *texture_cache.FindDepthTarget(db_desc.first, db_desc.second).image_view;
                state.depth_stencil_attachment.image_layout = depth_image.backing->state.layout;
            }
        } else {
            state.width = redirect.width;
            state.height = redirect.height;
            // The UI movie viewport is now 1920x1080 even when the scene is 960x540.
            // A native viewport (or native window-space UI vertices) must not be doubled
            // again. The final display copy still uses the guest render-size coordinates.
            const auto& viewport = regs.viewports[0];
            const bool native_coordinates = redirect.native_ui &&
                (UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2) ||
                 (regs.IsClipDisabled() && !regs.viewport_control.xscale_enable &&
                  !regs.viewport_control.yscale_enable));
            target_scale = UiComposition::Scale(guest_extent.first, guest_extent.second,
                                                redirect.width, redirect.height,
                                                native_coordinates);
        }
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }
    // tlg (shadPS4 #5452): a pass with no colour or depth attachment kept the maximum framebuffer
    // size as its render area (16384x16384), which a tile-based GPU pays for: at the truck crash
    // three kinds of such passes took ~5.6 ms of GPU time a frame. Draws cannot pass the scissors,
    // so the area is bounded by them. TLG_BOUND_EMPTY_PASS=0: as before.
    static const bool bound_empty = [] {
        const char* env = std::getenv("TLG_BOUND_EMPTY_PASS");
        return !env || *env != '0';
    }();
    if (bound_empty && state.width == instance.GetMaxFramebufferWidth() &&
        state.height == instance.GetMaxFramebufferHeight()) {
        const bool enable_offset = !regs.window_scissor.window_offset_disable;
        const s16 x_offset = enable_offset ? s16(regs.window_offset.window_x_offset) : s16{0};
        const s16 y_offset = enable_offset ? s16(regs.window_offset.window_y_offset) : s16{0};
        const s32 right = std::min({s32(regs.screen_scissor.bottom_right_x),
                                    s32(s16(regs.window_scissor.bottom_right_x + x_offset)),
                                    s32(s16(regs.generic_scissor.bottom_right_x + x_offset))});
        const s32 bottom = std::min({s32(regs.screen_scissor.bottom_right_y),
                                     s32(s16(regs.window_scissor.bottom_right_y + y_offset)),
                                     s32(s16(regs.generic_scissor.bottom_right_y + y_offset))});
        if (right > 0 && bottom > 0) {
            state.width = std::min<u32>(state.width, u32(right));
            state.height = std::min<u32>(state.height, u32(bottom));
        }
    }

    if (reduced) {
        // Half-resolution passes keep the scene's factor (proxy / native = scene / 1920).
        const auto size = scene_targets->Size();
        const auto proxy = pass_size.value_or(size);
        state.width = proxy.width;
        state.height = proxy.height;
        target_scale = {float(size.width) / 1920.0f, float(size.height) / 1080.0f};
    }
    if (census_on) {
        std::array<const VideoCore::ImageInfo*, AmdGpu::NUM_COLOR_BUFFERS> colors{};
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            if (cb_descs[cb].first) {
                colors[cb] = &texture_cache.GetImage(cb_descs[cb].first).info;
                census.color_samples.push_back(u8(regs.color_buffers[cb].NumSamples()));
            }
        }
        const auto* depth =
            db_desc.first ? &texture_cache.GetImage(db_desc.first).info : nullptr;
        census.raster_samples = u8(key.num_samples);
        census.path = PassCensus::ResolveSamplePath(
            census.color_samples, census.depth_samples, depth != nullptr,
            instance.IsMixedAnySamplesSupported(), instance.IsMixedDepthSamplesSupported());
        census.sample_shading =
            regs.ps_input_addr.persp_sample_ena || regs.ps_input_addr.linear_sample_ena;
        census.feedback_loop = attachment_feedback_loop;
        census.area = {state.width, state.height};
        FrameCapture::BeginPass(colors.data(), state.num_color_attachments, depth,
                                PassCensus::Format(census));
        diag_targets.clear();
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            if (cb_descs[cb].first) {
                diag_targets.push_back(cb_descs[cb].first);
            }
        }
        // tlg: TLG_DIAG_DEPTH=1: the depth target too (its depth aspect as 32-bit floats).
        static const bool diag_depth = std::getenv("TLG_DIAG_DEPTH") != nullptr;
        if (diag_depth && db_desc.first) {
            diag_targets.push_back(db_desc.first);
        }
    }
    // tlg: object motion vector attachment of G-buffer pipelines.
    if (key.motion_vectors) {
        object_motion->Attach(state, state.width, state.height);
    }
    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = CbExtent(0);
    const auto& mrt1_hint = CbExtent(1);
    VideoCore::TextureCache::ImageDesc mrt0_desc{Regs().color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{Regs().color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 Regs().color_buffers[0].Address(),
                                 Regs().color_buffers[1].Address()));
    runtime.ResolveImage(&mrt0_image, &mrt1_image, mrt0_desc.view_info.range,
                         mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = Regs();

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), DbExtent(), false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), DbExtent(), true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = Regs().depth_view.slice_start;
    sub_range.extent.layers = Regs().depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    runtime.CopyDepthStencil(&read_image, &write_image, sub_range);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    DrainDrawPipe();
    ASSERT_MSG(address % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer address and size must be a multiple of 4 bytes");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!buffer_cache.IsRegionGpuModified(address, num_bytes)) {
            // tlg: in the split nothing protects the host's view of guest memory, so this write
            // does not fault as it does in one process: same invalidation, by hand.
            if (TlgSplit::Host()) {
                InvalidateMemory(address, num_bytes);
            }
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + (num_bytes / sizeof(u32)), value);
            return;
        }
    }
    const auto [buffer, offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (is_gds) {
            return {buffer_cache.GetGdsBuffer(), address};
        }
        VideoCore::written_binding_source = {"fill", 0};
        return buffer_cache.ObtainBuffer(address, num_bytes, true);
    }();
    // tlg: TLG_FILL_STATS=1: GPU fills (CP DMA_DATA) per second and their sizes, every 5 s.
    static const bool fill_stats = [] {
        const char* env = std::getenv("TLG_FILL_STATS");
        return env && *env == '1';
    }();
    if (fill_stats) [[unlikely]] {
        static u64 fills = 0, bytes = 0, largest = 0;
        static auto window = std::chrono::steady_clock::now();
        ++fills;
        bytes += num_bytes;
        largest = std::max<u64>(largest, num_bytes);
        const auto now = std::chrono::steady_clock::now();
        if (now - window >= std::chrono::seconds(5)) {
            const double secs = std::chrono::duration<double>(now - window).count();
            std::printf("GPU fills: %.1f/s, %.1f MiB/s, largest %.2f MiB (last at %#llx, value %#x%s)\n",
                        fills / secs, bytes / secs / 1048576.0, largest / 1048576.0,
                        (unsigned long long)address, value, is_gds ? ", GDS" : "");
            fills = bytes = largest = 0;
            window = now;
        }
    }
    runtime.FillBuffer(buffer, offset, num_bytes, value);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    // TLG_GDS_LOG=1 (diagnostic): each new GDS slot -> memory copy.
    static const bool gds_log = std::getenv("TLG_GDS_LOG") != nullptr;
    if (gds_log && src_gds) {
        static std::set<std::pair<VAddr, VAddr>> seen;
        if (seen.emplace(dst, src).second) {
            std::printf("GDS log: copy GDS %#llx -> %#llx, %u bytes\n", static_cast<unsigned long long>(src),
                        static_cast<unsigned long long>(dst), num_bytes);
        }
    }
    DrainDrawPipe();
    if (!dst_gds && !buffer_cache.IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !buffer_cache.IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            // tlg: in the split nothing protects the host's view of guest memory, so this copy
            // does not fault as it does in one process: same invalidation, by hand.
            if (TlgSplit::Host()) {
                static const bool log = std::getenv("TLG_HOST_WRITE_LOG") != nullptr;
                if (log && num_bytes >= 4096) {
                    std::printf("Host copy: %#llx <- %#llx, %u bytes\n", (unsigned long long)dst,
                                (unsigned long long)src, num_bytes);
                }
                InvalidateMemory(dst, num_bytes);
            }
            std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    const auto* gds_buffer = buffer_cache.GetGdsBuffer();
    // tlg: the copy of a GDS counter waits for the compute shaders that count: without the barrier
    // Metal may overlap them and it reads the particle count mid-count (smoke flicker).
    // TLG_GDS_BARRIER=0: as before.
    static const bool gds_barrier = [] {
        const char* env = std::getenv("TLG_GDS_BARRIER");
        return !env || *env != '0';
    }();
    if (src_gds && gds_barrier) {
        runtime.AccessBuffer(gds_buffer, src, num_bytes, vk::PipelineStageFlagBits2::eComputeShader,
                             vk::AccessFlagBits2::eShaderWrite);
    }
    const auto [src_buffer, src_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (src_gds) {
            return {gds_buffer, src};
        }
        return buffer_cache.ObtainBuffer(src, num_bytes, false, true);
    }();
    const auto [dst_buffer, dst_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (dst_gds) {
            return {gds_buffer, dst};
        }
        VideoCore::written_binding_source = {"copy", 0};
        return buffer_cache.ObtainBuffer(dst, num_bytes, true, true);
    }();
    const vk::BufferCopy copy = {
        .srcOffset = src_offset,
        .dstOffset = dst_offset,
        .size = num_bytes,
    };
    runtime.CopyBuffer(src_buffer, dst_buffer, std::span{&copy, 1});
}

bool Rasterizer::DeferUnifiedFence(Common::UniqueFunction<void>&& write,
                                   std::span<const DeferredFenceWrite> writes) {
    if (!buffer_cache.UnifiedActive()) {
        return false;
    }
    // TLG_UNIFIED_FENCE_WAITS=1 (off by default: no FPS difference measured, 54.1-55.1): waits of
    // other GPU queues on deferred fences are met in stream order instead of waiting for the GPU.
    static const bool known = [] {
        const char* env = std::getenv("TLG_UNIFIED_FENCE_WAITS");
        return env && *env == '1';
    }();
    if (!known || writes.empty()) {
        return DeferUnifiedFence(std::move(write));
    }
    u64 first = 0, last = 0;
    {
        std::scoped_lock lock{deferred_fence_mutex};
        first = deferred_fence_seq + 1;
        for (const auto& w : writes) {
            deferred_fence_writes.emplace_back(++deferred_fence_seq, w);
        }
        last = deferred_fence_seq;
    }
    return DeferUnifiedFence([this, write = std::move(write), first, last]() mutable {
        write();
        std::scoped_lock lock{deferred_fence_mutex};
        std::erase_if(deferred_fence_writes,
                      [&](const auto& e) { return e.first >= first && e.first <= last; });
    });
}

bool Rasterizer::DeferredFenceValue(VAddr address, u32& value) {
    std::scoped_lock lock{deferred_fence_mutex};
    for (auto it = deferred_fence_writes.rbegin(); it != deferred_fence_writes.rend(); ++it) {
        const auto& w = it->second;
        if (address >= w.address && address + sizeof(u32) <= w.address + w.num_bytes) {
            value = static_cast<u32>(w.data >> ((address - w.address) * 8));
            return true;
        }
    }
    return false;
}

bool g_fence_kind_compute = false;

bool Rasterizer::DeferUnifiedFence(Common::UniqueFunction<void>&& write) {
    if (!buffer_cache.UnifiedActive()) {
        return false;
    }
    // tlg: TLG_ACB_QUEUE: a compute queue's fence is written when that queue's work is done.
    Scheduler* fence_scheduler = &scheduler;
    std::unique_lock<std::recursive_mutex> acb_lock;
    if (g_fence_kind_compute && acb_scheduler && buffer_cache.UnifiedAlias()) {
        fence_scheduler = acb_scheduler.get();
        acb_lock = std::unique_lock{acb_scheduler->CompanionMutex()};
        if (acb_needs_main) {
            AcbWaitMain();
        }
    }
    Scheduler& scheduler = *fence_scheduler;
    // Every fence, not only those in the range: labels stay in order (a later fence written
    // first lets the guest free memory an earlier one still writes into).
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &barrier,
        });
    });
    // tlg: TLG_FENCE_LATENCY=1 (diagnostic): how long a deferred fence waits from here (its
    // submission) until the GPU is done and it is written, and how many earlier submissions were
    // still unfinished on the GPU then; every 5 s.
    static const bool latency_log = [] {
        const char* v = std::getenv("TLG_FENCE_LATENCY");
        return v && *v == '1';
    }();
    if (latency_log) [[unlikely]] {
        u32 pending = 0;
        for (u64 t = scheduler.CurrentTick(); t > 1 && pending < 32 && !scheduler.IsFree(t - 1); --t) {
            ++pending;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const int kind = g_fence_kind_compute ? 1 : 0;
        write = [inner = std::move(write), t0, pending, kind]() mutable {
            inner();
            static std::mutex ms[2];
            static std::vector<float> lats[2];
            static std::vector<u32> pends[2];
            static std::chrono::steady_clock::time_point lasts[2] = {std::chrono::steady_clock::now(),
                                                                     std::chrono::steady_clock::now()};
            auto& m = ms[kind];
            auto& lat = lats[kind];
            auto& pend = pends[kind];
            auto& last = lasts[kind];
            const auto now = std::chrono::steady_clock::now();
            std::scoped_lock lock{m};
            lat.push_back(std::chrono::duration<float, std::milli>(now - t0).count());
            pend.push_back(pending);
            if (now - last > std::chrono::seconds(5) && !lat.empty()) {
                last = now;
                auto l = lat;
                std::sort(l.begin(), l.end());
                double pavg = 0;
                for (u32 p : pend) pavg += p;
                pavg /= pend.size();
                std::printf("Fence latency %s (5 s): %zu fences, median %.2f ms, p90 %.2f ms, max %.2f ms; "
                            "unfinished submissions before them: %.1f on average\n",
                            kind ? "compute" : "graphics", l.size(), l[l.size() / 2],
                            l[l.size() * 9 / 10], l.back(), pavg);
                lat.clear();
                pend.clear();
            }
        };
    }
    if (fence_scheduler != &this->scheduler) {
        AcbDeferFence(std::move(write));
    } else {
        scheduler.DeferPriorityOperation(std::move(write));
        // tlg: TLG_GFX_FENCE_BATCH=N (default 8; 0: a submission per fence, as before 0.1.8): the
        // game's ~24 graphics fences a frame each ended a submission (a command buffer reset on
        // this thread and a GPU hand-over each). With N, a fence leaves with the next submission:
        // when this thread runs out of packets (AcbIdle), after N fences, or with the piled-up
        // draws. M3 Max, 5 alternated runs: truck 47.4 -> 48.3, Seattle 46.4 -> 47.5, bridge same.
        static const u32 batch = [] {
            const char* v = std::getenv("TLG_GFX_FENCE_BATCH");
            return v && *v ? u32(std::atoi(v)) : 8u;
        }();
        const u64 tick = scheduler.CurrentTick();
        if (tick != gfx_fence_tick) {
            gfx_fence_tick = tick;
            gfx_fences_unflushed = 0;
        }
        if (batch == 0 || !DrawPipe::OnStageB() || ++gfx_fences_unflushed >= batch) {
            gfx_fences_unflushed = 0;
            scheduler.Flush();
        }
    }
    VideoCore::Unified::Add(VideoCore::Unified::FencesDeferred);
    VideoCore::Unified::MaybeReport(static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
    return true;
}

void Rasterizer::SyncUnifiedRead(VAddr address, u64 size) {
    if (!buffer_cache.UnifiedActive() || !buffer_cache.IsUnified(address, size)) {
        return;
    }
    DrainDrawPipe();
    buffer_cache.SyncUnifiedRead(address, size);
}

void Rasterizer::WriteHiddenOcclusion(VAddr address, u32 num_pairs) {
    static constexpr u64 CounterValid = 1ULL << 63;
    num_pairs = std::clamp<u32>(num_pairs, 1, OcclusionQueries::MaxCounterPairs);
    const u32 size = num_pairs * 2 * sizeof(u64);
    if (IsMapped(address, size) && buffer_cache.IsRegionGpuModified(address, size)) {
        // Results the GPU wrote before (a counted query at this address): written there too.
        texture_cache.InvalidateMemoryFromGPU(address, size);
        VideoCore::written_binding_source = {"occlusion", 0};
        const auto [buffer, offset] = buffer_cache.ObtainBuffer(address, size, true);
        runtime.WriteOcclusionResults(buffer, offset, num_pairs,
                                      occlusion ? occlusion->Pool() : vk::QueryPool{}, {});
        return;
    }
    u64* results = std::bit_cast<u64*>(address);
    std::fill(results, results + 2 * num_pairs, CounterValid);
}

void Rasterizer::OcclusionDump(VAddr address, u32 num_pairs) {
    // TLG_OCCLUSION_LOG=1: counted and hidden queries, every 4096 dumps.
    static const bool log = [] {
        const char* v = std::getenv("TLG_OCCLUSION_LOG");
        return v && *v && std::atoi(v) != 0;
    }();
    static u64 dumps, counted, hidden, segments_used;
    const auto report = [&] {
        if (log && (++dumps % 4096) == 0) {
            std::printf("Occlusion: %llu dumps, %llu queries counted (%llu segments), %llu hidden, "
                        "%llu segments dropped, %u open\n",
                        static_cast<unsigned long long>(dumps),
                        static_cast<unsigned long long>(counted),
                        static_cast<unsigned long long>(segments_used),
                        static_cast<unsigned long long>(hidden),
                        static_cast<unsigned long long>(occlusion ? occlusion->Dropped() : 0),
                        occlusion ? occlusion->NumOpen() : 0u);
        }
    };
    num_pairs = std::clamp<u32>(num_pairs, 1, OcclusionQueries::MaxCounterPairs);
    const u32 size = num_pairs * 2 * sizeof(u64);
    if (!occlusion) {
        // Mode 5 (or no host query reset): the whole query block reads as zero samples.
        WriteHiddenOcclusion(address & ~VAddr{15}, num_pairs);
        report();
        return;
    }
    if (address % 16 == 0) {
        // Begin dump: nothing is written, the GPU writes the whole query at the end dump.
        if (!IsMapped(address, size) || !occlusion->Begin(address, num_pairs)) {
            WriteHiddenOcclusion(address, num_pairs);
            ++hidden;
        }
        report();
        return;
    }
    if (address % 16 != sizeof(u64)) {
        WriteHiddenOcclusion(address, num_pairs);
        ++hidden;
        report();
        return;
    }
    // End dump: the whole query, from its begin counters.
    const VAddr results = address - sizeof(u64);
    if (log && counted + hidden < 64) {
        std::printf("Occlusion: query at %llx, %u pairs\n", static_cast<unsigned long long>(results),
                    num_pairs);
    }
    OcclusionQueries::QueryList queries;
    if (!occlusion->End(results, queries)) {
        WriteHiddenOcclusion(results, num_pairs);
        ++hidden;
    } else {
        texture_cache.InvalidateMemoryFromGPU(results, size);
        VideoCore::written_binding_source = {"occlusion", 0};
        const auto [buffer, offset] = buffer_cache.ObtainBuffer(results, size, true);
        const std::span<const u32> used{queries.data(), queries.size()};
        runtime.WriteOcclusionResults(buffer, offset, num_pairs, occlusion->Pool(), used);
        occlusion->MarkUsed(used);
        ++counted;
        segments_used += queries.size();
    }
    // Writing the results ended the rendering scope: the other open queries go on counting from
    // the next one.
    occlusion->Resume();
    report();
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    DrainDrawPipe();
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size, bool assume_locks) {
    DrainDrawPipe();
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    texture_cache.ResolveReadbacks(addr, size, assume_locks, VideoCore::ReadbackSource::CpuWrite);
    buffer_cache.InvalidateMemory(addr, size, assume_locks);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::OnWriteFault(VAddr addr, bool assume_locks, bool nowait) {
    // tlg: a no-wait write takes the page over at once (BufferCache::NoWaitWrite): no draw pipe
    // drain, no GPU wait. Images there are still invalidated.
    if (nowait && !assume_locks && IsMapped(addr, 8) && buffer_cache.NoWaitWrite(addr)) {
        texture_cache.InvalidateMemory(addr, 8);
        buffer_cache.ExtendWriteFault(addr);
        return true;
    }
    DrainDrawPipe();
    if (!InvalidateMemory(addr, 8, assume_locks)) {
        return false;
    }
    buffer_cache.ExtendWriteFault(addr);
    return true;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size, bool assume_locks, bool lagged) {
    // tlg: a lagged read takes the bytes already in memory (the GPU data of an earlier frame)
    // and only queues the copy for the next read: no draw pipe drain, no GPU wait.
    if (lagged && !assume_locks && IsMapped(addr, size) && buffer_cache.LaggedRead(addr, size)) {
        return true;
    }
    DrainDrawPipe();
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    const bool image_readback =
        texture_cache.ResolveReadbacks(addr, size, assume_locks, VideoCore::ReadbackSource::CpuRead);
    // tlg: without GPU-modified buffer data there is nothing to download: the guest thread no
    // longer waits for the GPU command thread to reach its request (~3 ms per fault in ISS,
    // hundreds per second). TLG_FAST_READ_FAULT=0 restores the round trip.
    static const bool fast = [] { const char* e = std::getenv("TLG_FAST_READ_FAULT"); return !(e && *e == '0'); }();
    if (fast && !assume_locks && !buffer_cache.IsRegionGpuModified(addr, size)) {
        buffer_cache.ReleaseReadProtection(addr, size);
    } else if (!assume_locks && !image_readback && buffer_cache.TryEagerRead(addr, size)) {
        // tlg: served from the copy made at the last fence (TLG_EAGER_READBACK). Not after an
        // image readback: that resolved newer data the buffer copy would overwrite.
    } else if (!image_readback || buffer_cache.IsRegionGpuModified(addr, size)) {
        if (!assume_locks) {
            buffer_cache.NoteReadback(addr, size);
        }
        buffer_cache.ReadMemory(addr, size, false, assume_locks);
    }
    return true;
}

void Rasterizer::ProcessDownloadImages() {
    DrainDrawPipe();
    texture_cache.ProcessDownloadImages();
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::RegisterMemory(VAddr addr, u64 size) {
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    DrainDrawPipe();
    // tlg: the host memory hooks call this on a guest thread, where DrainDrawPipe does nothing:
    // buffer_cache.InvalidateMemory queues its own readback (SendCommand) but
    // texture_cache.UnmapMemory untracks and frees images, resolving their readbacks as a
    // GPU-side thread would, while the draw recording thread may be running earlier draws that
    // use them. TLG_FOREIGN_SERIALIZE=1 runs it in order with those draws instead.
    buffer_cache.InvalidateMemory(addr, size);
    RunForeign([this, addr, size] { texture_cache.UnmapMemory(addr, size); });
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.CommitWith(instance.IsDepthBoundsSupported(),
                             instance.IsDynamicColorWriteMaskSupported(),
                             instance.IsAttachmentFeedbackLoopLayoutSupported(),
                             [&](auto&& command) { scheduler.Record(std::move(command)); });
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = Regs();

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            // tlg: sub-pixel jitter of scene geometry for the temporal upscaler; the same
            // shift as jittering the projection.
            viewport.x = (xoffset - xscale) * target_scale[0] + draw_jitter[0];
            viewport.y = (yoffset - yscale) * target_scale[1] + draw_jitter[1];
            viewport.width = xscale * 2.0f * target_scale[0];
            viewport.height = yscale * 2.0f * target_scale[1];
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        if (target_scale[0] != 1.0f || target_scale[1] != 1.0f) {
            const auto scale = [](s32 v, float f) { return s32(std::lround(float(v) * f)); };
            const s32 x0 = scale(vp_scsr.top_left_x, target_scale[0]);
            const s32 y0 = scale(vp_scsr.top_left_y, target_scale[1]);
            const s32 x1 = scale(vp_scsr.top_left_x + s32(vp_scsr.GetWidth()), target_scale[0]);
            const s32 y1 = scale(vp_scsr.top_left_y + s32(vp_scsr.GetHeight()), target_scale[1]);
            scissors.push_back({
                .offset = {x0, y0},
                .extent = {u32(std::max(x1 - x0, 0)), u32(std::max(y1 - y0, 0))},
            });
        } else {
            scissors.push_back({
                .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
                .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
            });
        }
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

vk::Pipeline Rasterizer::SquarePass(const GraphicsPipeline& pipeline) const {
    const vk::Pipeline square = pipeline.SquarePassHandle();
    static const bool enabled = [] {
        const char* env = std::getenv("TLG_SQUARE_PASS");
        return !env || env[0] != '0';
    }();
    if (!square || !enabled) [[likely]] {
        return {};
    }
    // MIN(s * s, d * d) = MIN(s, d)^2: the second draw needs the first one's coverage, which a
    // depth or stencil write would change.
    const auto& regs = Regs();
    const bool depth_write = regs.depth_control.depth_enable &&
                             regs.depth_control.depth_write_enable && regs.depth_buffer.DepthValid();
    if (depth_write || regs.depth_control.stencil_enable) {
        static bool warned = false;
        if (!std::exchange(warned, true)) {
            LOG_WARNING(Render_Vulkan, "Scaled MIN/MAX blend not squared: the draw writes depth "
                                       "or stencil");
        }
        return {};
    }
    return square;
}

struct StencilFaceState {
    vk::StencilOp fail_op;
    vk::StencilOp pass_op;
    vk::StencilOp depth_fail_op;
    u32 reference;
};

// tlg: from shadPS4-Iss-FixBuild 03db8f14 (GPL-2.0-or-later). GCN has separate stencil test
// (STENCILTESTVAL) and replace (STENCILOPVAL) values, while a Vulkan face has a single reference
// for both. Finds a reference that keeps the test exact and still writes what every replacing op
// should, turning such ops into equivalent ones (zero, keep, invert, increment, decrement) where
// the stencil value is known. Returns nullopt if it can't. ISS hits the conflict ~9500 times
// a session (effects masked by the character silhouette). Checked by the fork against a model of
// the GCN stencil unit (400k random face configurations).
static std::optional<StencilFaceState> ResolveStencilFace(AmdGpu::CompareFunc func,
                                                          const AmdGpu::StencilRefMask& ref,
                                                          u32 write_mask, AmdGpu::StencilFunc fail,
                                                          AmdGpu::StencilFunc zpass,
                                                          AmdGpu::StencilFunc zfail) {
    using AmdGpu::CompareFunc;
    using AmdGpu::StencilFunc;
    const u32 test_val = ref.stencil_test_val;
    const u32 compare_mask = ref.stencil_mask;
    write_mask &= 0xFF;
    const bool test_uses_reference = func != CompareFunc::Always && func != CompareFunc::Never;

    const auto written_value = [&](StencilFunc op) -> std::optional<u32> {
        switch (op) {
        case StencilFunc::ReplaceTest:
            return test_val;
        case StencilFunc::ReplaceOp:
            return ref.stencil_op_val;
        case StencilFunc::Ones:
            return 0xFFU;
        default:
            return std::nullopt;
        }
    };

    // An EQUAL test that passed, or a NOTEQUAL test that failed, means the stencil value matches
    // test_val in the compared bits, which is enough to know what a write will change.
    const bool written_bits_compared = (write_mask & ~compare_mask) == 0;
    const bool whole_value_known = compare_mask == 0xFF && write_mask == 0xFF;
    const bool known_on_pass = func == CompareFunc::Equal && written_bits_compared;
    const bool known_on_fail = func == CompareFunc::NotEqual && written_bits_compared;

    const auto resolve = [&](StencilFunc op, u32 reference,
                             bool value_known) -> std::optional<vk::StencilOp> {
        const auto value = written_value(op);
        if (!value) {
            return LiverpoolToVK::StencilOp(op);
        }
        const u32 target = *value & write_mask;
        if (target == (reference & write_mask)) {
            return vk::StencilOp::eReplace;
        }
        if (target == 0) {
            return vk::StencilOp::eZero;
        }
        if (value_known) {
            if (target == (test_val & write_mask)) {
                return vk::StencilOp::eKeep;
            }
            if (target == (~test_val & write_mask)) {
                return vk::StencilOp::eInvert;
            }
            if (whole_value_known && *value == ((test_val + 1) & 0xFF)) {
                return vk::StencilOp::eIncrementAndWrap;
            }
            if (whole_value_known && *value == ((test_val - 1) & 0xFF)) {
                return vk::StencilOp::eDecrementAndWrap;
            }
        }
        return std::nullopt;
    };

    // Try each written value as the reference. When the test reads the reference, its compared
    // bits must stay those of test_val, and only the rest can carry the written value.
    const std::array<std::optional<u32>, 4> candidates = {
        written_value(zpass), written_value(zfail), written_value(fail), test_val};
    for (const auto& candidate : candidates) {
        if (!candidate) {
            continue;
        }
        const u32 reference = test_uses_reference
                                  ? (test_val & compare_mask) | (*candidate & ~compare_mask & 0xFF)
                                  : *candidate;
        const auto fail_op = resolve(fail, reference, known_on_fail);
        const auto pass_op = resolve(zpass, reference, known_on_pass);
        const auto depth_fail_op = resolve(zfail, reference, known_on_pass);
        if (fail_op && pass_op && depth_fail_op) {
            return StencilFaceState{*fail_op, *pass_op, *depth_fail_op, reference};
        }
    }
    return std::nullopt;
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_stencil = GetEffectiveDepthStencilState(regs);
    const bool depth_test_enabled = depth_stencil.depth_test_enable;
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    dynamic_state.SetDepthWriteEnabled(depth_stencil.depth_write_enable &&
                                       !regs.depth_render_control.depth_clear_enable);
    if (depth_test_enabled) {
        auto op = LiverpoolToVK::CompareOp(regs.depth_control.depth_func);
        // tlg (macOS): a pass that redraws geometry with depth EQUAL against an earlier pass's
        // depth (ISS's material pass after the depth pre-pass) needs bit-identical positions from
        // a different vertex shader; Metal does not guarantee that, and whole objects vanished
        // (the bridge's broken frames). TLG_MAC_DEPTH_EQUAL=lequal / gequal turns such read-only
        // EQUAL tests into LESS_OR_EQUAL / GREATER_OR_EQUAL; =off keeps EQUAL.
        static const vk::CompareOp equal_as = [] {
            const char* env = std::getenv("TLG_MAC_DEPTH_EQUAL");
            if (env && !std::strcmp(env, "lequal")) {
                return vk::CompareOp::eLessOrEqual;
            }
            if (env && !std::strcmp(env, "gequal")) {
                return vk::CompareOp::eGreaterOrEqual;
            }
            if (env && !std::strcmp(env, "always")) { // diagnostic: every read-only test passes
                return vk::CompareOp::eAlways;
            }
            if (env && !std::strcmp(env, "material")) { // diagnostic: only the material pass
                return vk::CompareOp::eNever; // marker, resolved below
            }
            return vk::CompareOp::eEqual;
        }();
        // "material": ISS's material pass (HDR target 0x70fcdf0000 with the stencil test on).
        if (equal_as == vk::CompareOp::eNever) {
            if (!depth_stencil.depth_write_enable && depth_stencil.stencil_test_enable &&
                op == vk::CompareOp::eGreaterOrEqual && regs.color_buffers[0] &&
                regs.color_buffers[0].Address() == 0x70fcdf0000ull) {
                op = vk::CompareOp::eAlways;
            }
        } else if (!depth_stencil.depth_write_enable &&
            (op == vk::CompareOp::eEqual ||
             (equal_as == vk::CompareOp::eAlways && op == vk::CompareOp::eGreaterOrEqual))) {
            op = equal_as;
        }
        static std::atomic<u32> seen{0};
        const u32 bit = 1u << (u32(regs.depth_control.depth_func) * 2 + (depth_stencil.depth_write_enable ? 1 : 0));
        if (!(seen.fetch_or(bit, std::memory_order_relaxed) & bit) && std::getenv("TLG_DEPTH_FUNC_LOG")) {
            std::printf("GPU: depth func %u write %d first used\n", u32(regs.depth_control.depth_func),
                        int(depth_stencil.depth_write_enable));
        }
        dynamic_state.SetDepthCompareOp(op);
    }

    const bool depth_bounds_test_enabled = depth_stencil.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        // tlg: TLG_LIGHTING_TEST=bias passes the slope scale as the guest gives it.
        const float slope = front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias, LightingTest::Current().bias ? slope : slope / 16.f);
    }

    // tlg: TLG_MAC_STENCIL_OFF=1 (diagnostic): no stencil test in passes that do not write depth.
    static const bool stencil_off = [] {
        const char* env = std::getenv("TLG_MAC_STENCIL_OFF");
        return env && env[0] == '1';
    }();
    const bool stencil_test_enabled =
        depth_stencil.stencil_test_enable && !(stencil_off && !depth_stencil.depth_write_enable);
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        const u32 front_write_mask = !stencil_clear ? front.stencil_write_mask : 0U;
        const u32 back_write_mask = !stencil_clear ? back.stencil_write_mask : 0U;

        const auto setup_face = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref,
                                   u32 write_mask, AmdGpu::StencilFunc fail,
                                   AmdGpu::StencilFunc zpass,
                                   AmdGpu::StencilFunc zfail) -> std::pair<StencilOps, u32> {
            if (const auto face = ResolveStencilFace(func, ref, write_mask, fail, zpass, zfail)) {
                return {StencilOps{
                            .fail_op = face->fail_op,
                            .pass_op = face->pass_op,
                            .depth_fail_op = face->depth_fail_op,
                            .compare_op = LiverpoolToVK::CompareOp(func),
                        },
                        face->reference};
            }
            // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so when both values can't be
            // honored a face whose stencil ops include ReplaceOp takes its reference from op_val.
            const bool uses_op_val = fail == AmdGpu::StencilFunc::ReplaceOp ||
                                     zpass == AmdGpu::StencilFunc::ReplaceOp ||
                                     zfail == AmdGpu::StencilFunc::ReplaceOp;
            const u32 reference = uses_op_val ? ref.stencil_op_val : ref.stencil_test_val;
            // tlg: per draw; the first 32 are enough to see which configurations remain.
            static std::atomic<u32> warned{0};
            if (warned.fetch_add(1, std::memory_order_relaxed) < 32) {
                LOG_WARNING(Render_Vulkan,
                            "Stencil func {} test_val {:#x} mask {:#x} can't be combined with ops "
                            "{}/{}/{} op_val {:#x} write mask {:#x}; using reference {:#x}",
                            static_cast<u32>(func), ref.stencil_test_val, ref.stencil_mask,
                            static_cast<u32>(fail), static_cast<u32>(zpass), static_cast<u32>(zfail),
                            ref.stencil_op_val, write_mask, reference);
            }
            return {StencilOps{
                        .fail_op = LiverpoolToVK::StencilOp(fail),
                        .pass_op = LiverpoolToVK::StencilOp(zpass),
                        .depth_fail_op = LiverpoolToVK::StencilOp(zfail),
                        .compare_op = LiverpoolToVK::CompareOp(func),
                    },
                    reference};
        };

        const auto& sc = regs.stencil_control;
        const auto [front_ops, front_reference] =
            setup_face(regs.depth_control.stencil_ref_func, front, front_write_mask,
                       sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const auto [back_ops, back_reference] =
            regs.depth_control.backface_enable
                ? setup_face(regs.depth_control.stencil_bf_func, back, back_write_mask,
                             sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : std::pair{front_ops, front_reference};
        dynamic_state.SetStencilOps(front_ops, back_ops);
        dynamic_state.SetStencilReferences(front_reference, back_reference);
        dynamic_state.SetStencilWriteMasks(front_write_mask, back_write_mask);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               (type == AmdGpu::PrimitiveType::QuadList && !LiverpoolToVK::MacQuadIndex()) ||
               type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

#if defined(__linux__) || defined(_WIN32)
u32 Rasterizer::GetGpuCommandProcessorThreadId() {
    return liverpool->GetGpuCommandProcessorThreadId();
}
#endif

} // namespace Vulkan

namespace Vulkan {

void Rasterizer::MarkPass(const GraphicsPipeline* pipeline, const RenderState& state) {
    // tlg: TLG_PASS_LABELS=1 (diagnostic): a "tlgpass ..." debug label right after each render pass
    // begins; the patched MoltenVK names the Metal render encoder after it, so a Metal System
    // Trace shows GPU time per kind of pass (tools/gpu_probes/trace_passes.py).
    static const bool labels = [] {
        const char* env = std::getenv("TLG_PASS_LABELS");
        return env && *env == '1';
    }();
    auto* profiler = GpuProfiler::Get();
    pass_label.clear();
    if ((!profiler && !labels) || !scheduler.WillBeginRendering(state)) {
        return;
    }
    const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    if (labels) {
        std::string targets;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            targets += cb_descs[cb].first
                           ? vk::to_string(texture_cache.GetImage(cb_descs[cb].first).info.pixel_format)
                           : std::string{"-"};
            targets += ',';
        }
        pass_label = fmt::format("tlgpass {}x{} [{}] d:{} vs:{:08x} ps:{:08x}", state.width, state.height,
                                 targets,
                                 db_desc.first ? vk::to_string(texture_cache.GetImage(db_desc.first)
                                                                   .info.pixel_format)
                                               : std::string{"-"},
                                 u32(vs.pgm_hash), u32(ps ? ps->pgm_hash : 0));
    }
    if (!profiler) {
        return;
    }
    std::array<u64, AmdGpu::NUM_COLOR_BUFFERS + 6> parts{};
    parts[0] = state.width | u64(state.height) << 32;
    parts[1] = vs.pgm_hash;
    parts[2] = ps ? ps->pgm_hash : 0;
    parts[3] = db_desc.first ? u64(texture_cache.GetImage(db_desc.first).info.pixel_format) : 0;
    for (u32 cb = 0; cb < state.num_color_attachments && cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        parts[4 + cb] = cb_descs[cb].first
                            ? u64(texture_cache.GetImage(cb_descs[cb].first).info.pixel_format) + 1
                            : 0;
    }
    const u64 key = XXH3_64bits(parts.data(), sizeof(parts));
    profiler->Mark(key, [&] {
        std::string targets;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            targets += cb_descs[cb].first
                           ? vk::to_string(texture_cache.GetImage(cb_descs[cb].first).info.pixel_format)
                           : std::string{"-"};
            targets += ' ';
        }
        return fmt::format("pass {}x{} [{}] depth {} vs {:016x} ps {:016x}", state.width,
                           state.height, targets,
                           db_desc.first ? vk::to_string(
                                               texture_cache.GetImage(db_desc.first).info.pixel_format)
                                         : std::string{"-"},
                           vs.pgm_hash, ps ? ps->pgm_hash : 0);
    });
}

void Rasterizer::NoteFrameStart() {
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->BeginFrame();
    }
    const u64 frame = TlgStats::gpu_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    // TLG_BUFFER_STATS=1: how many earlier frames the GPU has not finished when the GPU thread
    // starts a frame — the lag that decides whether guest memory can be read in place.
    static const bool stats = [] {
        const char* value = std::getenv("TLG_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    if (!stats) {
        return;
    }
    static std::array<u64, 16> frame_ticks{};
    static std::array<u64, 17> lag_histogram{};
    u32 lag = 0;
    for (u32 back = 1; back < frame_ticks.size() && back < frame; ++back) {
        const u64 tick = frame_ticks[(frame - back) % frame_ticks.size()];
        if (tick && !scheduler.IsFree(tick)) {
            lag = back;
        }
    }
    ++lag_histogram[lag];
    frame_ticks[frame % frame_ticks.size()] = scheduler.CurrentTick();
    if (frame % 600 == 0) {
        std::printf("GPU lag at frame start (frames not finished: count):");
        for (u32 i = 0; i < lag_histogram.size(); ++i) {
            if (lag_histogram[i]) {
                std::printf(" %u:%llu", i, static_cast<unsigned long long>(lag_histogram[i]));
            }
        }
        std::printf("\n");
        lag_histogram = {};
    }
}


vk::Buffer Rasterizer::QuadIndexBuffer() {
    // tlg: the index pattern of TLG_MAC_QUAD_INDEX, made once: 0,1,2, 0,2,3, 4,5,6, 4,6,7, ...
    if (!quad_index_buffer) {
        const u64 bytes = u64(MaxPatternQuads) * 6 * sizeof(u32);
        quad_index_buffer = std::make_unique<VideoCore::Buffer>(instance, 0, bytes, VideoCore::MemoryType::Stream,
                                                                "Quad index pattern");
        auto* indices = reinterpret_cast<u32*>(quad_index_buffer->mapped_data.data());
        for (u32 q = 0; q < MaxPatternQuads; ++q) {
            const u32 v = q * 4;
            const u32 pattern[6] = {v, v + 1, v + 2, v, v + 2, v + 3};
            std::memcpy(indices + q * 6, pattern, sizeof(pattern));
        }
        quad_index_buffer->Flush(0, bytes);
    }
    return quad_index_buffer->Handle();
}


std::tuple<vk::Buffer, u64, vk::Buffer, u64> Rasterizer::QuadIndexedIndirect(VAddr args_address,
                                                                            u32 stride,
                                                                            u32 max_count) {
    // tlg (macOS, TLG_MAC_QUAD_INDEX): host_shaders/quad_index_expand.comp writes each record's
    // quads as triangles into its own slot of a staging index buffer and rewrites the record.
    // Recorded outside the render pass, with a barrier to the indirect and index reads.
    struct Pipeline {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    static Pipeline pipe = [&] {
        const auto device = instance.GetDevice();
        Pipeline p;
        std::array<vk::DescriptorSetLayoutBinding, 4> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        p.set_layout = device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }).value;
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = 7 * sizeof(u32)};
        p.layout = device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*p.set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }).value;
        const auto module = CompileSPV(QUAD_INDEX_EXPAND_COMP, device);
        p.pipeline = device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *p.layout,
                }).value;
        device.destroyShaderModule(module);
        return p;
    }();

    if (!index_buffer_obj || index_buffer_bytes == 0 || max_count == 0) {
        return {};
    }
    const bool is16 = index_bind.type == vk::IndexType::eUint16;
    const u32 index_size = is16 ? 2u : 4u;
    const u32 total_indices = index_buffer_bytes / index_size;
    // Each record gets a slot for every quad the index buffer holds, 64 MiB at most in all.
    constexpr u64 MaxOutBytes = 64ull << 20;
    const u64 slot_quads = std::min<u64>(total_indices / 4, MaxOutBytes / (u64(max_count) * 24));
    if (slot_quads == 0) {
        return {};
    }
    if (std::getenv("TLG_QUAD_SIZE_LOG")) {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 30) {
            std::printf("Quad expand: %u records, %u indices in the buffer, %llu quads a slot\n",
                        max_count, total_indices, (unsigned long long)slot_quads);
        }
    }
    const u64 align = instance.StorageMinAlignment();
    const u64 in_bytes = u64(stride) * max_count;
    const auto [in_buffer, in_offset] = buffer_cache.ObtainBuffer(args_address, in_bytes, false);
    const u64 in_start = Common::AlignDown(in_offset, align);
    const u64 in_size = in_offset - in_start + in_bytes;
    const u64 idx_start = Common::AlignDown(index_bind.offset, align);
    const u64 idx_size = Common::AlignUp(index_bind.offset - idx_start + index_buffer_bytes, 4);
    const u64 args_bytes = u64(max_count) * sizeof(vk::DrawIndexedIndirectCommand);
    const u64 out_idx_bytes = u64(max_count) * slot_quads * 6 * sizeof(u32);
    auto& pool = runtime.GetStagingPool();
    const auto out_args = pool.Request(args_bytes, VideoCore::MemoryType::DeviceLocal, align);
    const auto out_idx = pool.Request(out_idx_bytes, VideoCore::MemoryType::DeviceLocal, align);

    scheduler.EndRendering();
    if (runtime.IsBufferAccessed(in_buffer, in_start, in_size) ||
        runtime.IsBufferAccessed(index_buffer_obj, idx_start, idx_size) ||
        runtime.IsBufferAccessed(out_args.buffer, out_args.offset, args_bytes, true) ||
        runtime.IsBufferAccessed(out_idx.buffer, out_idx.offset, out_idx_bytes, true)) {
        runtime.FlushBarriers();
    }
    const std::array<u32, 7> push{max_count,
                                  static_cast<u32>((in_offset - in_start) / 4),
                                  stride / 4,
                                  static_cast<u32>((index_bind.offset - idx_start) / index_size),
                                  is16 ? 1u : 0u,
                                  static_cast<u32>(slot_quads),
                                  total_indices};
    scheduler.Record([in = in_buffer->Handle(), in_start, in_size, idx = index_bind.handle, idx_start,
                      idx_size, oa = out_args.buffer->Handle(), oa_offset = out_args.offset,
                      args_bytes, oi = out_idx.buffer->Handle(), oi_offset = out_idx.offset,
                      out_idx_bytes, push](vk::CommandBuffer cmdbuf) {
        const std::array<vk::DescriptorBufferInfo, 4> infos{{
            {in, in_start, in_size},
            {idx, idx_start, idx_size},
            {oa, oa_offset, args_bytes},
            {oi, oi_offset, out_idx_bytes},
        }};
        std::array<vk::WriteDescriptorSet, 4> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipe.layout, 0, writes);
        cmdbuf.pushConstants(*pipe.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             push.data());
        cmdbuf.dispatch((push[5] + 63) / 64, push[0], 1);
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask =
                vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eIndexInput,
            .dstAccessMask =
                vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eIndexRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    });
    runtime.AccessBuffer(in_buffer, in_start, in_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);
    runtime.AccessBuffer(index_buffer_obj, idx_start, idx_size,
                         vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    runtime.AccessBuffer(out_args.buffer, out_args.offset, args_bytes,
                         vk::PipelineStageFlagBits2::eDrawIndirect,
                         vk::AccessFlagBits2::eIndirectCommandRead);
    runtime.AccessBuffer(out_idx.buffer, out_idx.offset, out_idx_bytes,
                         vk::PipelineStageFlagBits2::eIndexInput, vk::AccessFlagBits2::eIndexRead);
    return {out_args.buffer->Handle(), out_args.offset, out_idx.buffer->Handle(), out_idx.offset};
}

std::pair<vk::Buffer, u64> Rasterizer::QuadIndirectArgs(VAddr args_address, u32 stride, u32 max_count) {
    // tlg (macOS, TLG_MAC_QUAD_INDEX): host_shaders/quad_indirect.comp turns the game's records
    // into DrawIndexedIndirectCommands for the quad index pattern. Recorded outside the render
    // pass, with a barrier to the indirect read.
    struct Pipeline {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    static Pipeline pipe = [&] {
        const auto device = instance.GetDevice();
        Pipeline p;
        std::array<vk::DescriptorSetLayoutBinding, 2> bindings{};
        for (u32 i = 0; i < 2; ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        p.set_layout = device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }).value;
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = 4 * sizeof(u32)};
        p.layout = device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*p.set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }).value;
        const auto module = CompileSPV(QUAD_INDIRECT_COMP, device);
        p.pipeline = device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *p.layout,
                }).value;
        device.destroyShaderModule(module);
        return p;
    }();

    const u64 align = instance.StorageMinAlignment();
    const u64 in_bytes = u64(stride) * max_count;
    const auto [in_buffer, in_offset] = buffer_cache.ObtainBuffer(args_address, in_bytes, false);
    const u64 in_start = Common::AlignDown(in_offset, align);
    const u64 in_size = in_offset - in_start + in_bytes;
    const u64 out_bytes = u64(max_count) * sizeof(vk::DrawIndexedIndirectCommand);
    const auto out = runtime.GetStagingPool().Request(out_bytes, VideoCore::MemoryType::DeviceLocal, align);

    scheduler.EndRendering();
    if (runtime.IsBufferAccessed(in_buffer, in_start, in_size) ||
        runtime.IsBufferAccessed(out.buffer, out.offset, out_bytes, true)) {
        runtime.FlushBarriers();
    }
    const std::array<u32, 4> push{max_count, static_cast<u32>((in_offset - in_start) / 4), stride / 4,
                                  MaxPatternQuads};
    scheduler.Record([in = in_buffer->Handle(), in_start, in_size, out_handle = out.buffer->Handle(),
                      out_offset = out.offset, out_bytes, push](vk::CommandBuffer cmdbuf) {
        const std::array<vk::DescriptorBufferInfo, 2> infos{{
            {in, in_start, in_size},
            {out_handle, out_offset, out_bytes},
        }};
        std::array<vk::WriteDescriptorSet, 2> writes{};
        for (u32 i = 0; i < 2; ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipe.layout, 0, writes);
        cmdbuf.pushConstants(*pipe.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             push.data());
        cmdbuf.dispatch((push[0] + 63) / 64, 1, 1);
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect,
            .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    });
    runtime.AccessBuffer(in_buffer, in_start, in_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);
    runtime.AccessBuffer(out.buffer, out.offset, out_bytes, vk::PipelineStageFlagBits2::eDrawIndirect,
                         vk::AccessFlagBits2::eIndirectCommandRead);
    return {out.buffer->Handle(), out.offset};
}

} // namespace Vulkan
