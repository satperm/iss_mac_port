// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// tlg: partial-wave and multi-wave LDS barriers and the uniform-ReadLane divergence search are
// ported from the Shadow of the Colossus native port (github.com/AjncSystem/ShadowOfTheColossus-PC,
// commit 6f0b3e56 and its file at 70bb4385; the ReadLane part comes from Pink-shadPS4 by
// luizgustavs). There they were SotC-only; here ISS turns them on (recompiler_toggles.h).

#include <algorithm>
#include <queue>
#include <unordered_set>
#include <vector>
#include "common/logging/log.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler_toggles.h"

namespace Shader::Optimization {

static bool IsLoadShared(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::LoadSharedU16 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU32 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU64;
}

static bool IsWriteShared(const IR::Inst& inst) {
    const IR::Opcode opcode = inst.GetOpcode();
    if (opcode >= IR::Opcode::SharedAtomicIAdd32 && opcode <= IR::Opcode::SharedAtomicCmpSwap64) {
        return !inst.Flags<bool>();
    }
    return opcode == IR::Opcode::WriteSharedU16 || opcode == IR::Opcode::WriteSharedU32 ||
           opcode == IR::Opcode::WriteSharedU64;
}

// Workgroup barrier, or a subgroup one when every guest wave fits in one host subgroup (below).
static void EmitSync(IR::IREmitter& ir, bool subgroup) {
    if (subgroup) {
        ir.SubgroupBarrier();
    } else {
        ir.Barrier();
    }
}

// Inserts barriers when a shared memory write and read occur in the same basic block.
static void EmitBarrierInBlock(IR::Block* block, bool subgroup) {
    enum class BarrierAction : u32 {
        None,
        BarrierOnWrite,
        BarrierOnRead,
    };
    BarrierAction action{};
    for (IR::Inst& inst : block->Instructions()) {
        if (IsLoadShared(inst)) {
            if (action == BarrierAction::BarrierOnRead) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                EmitSync(ir, subgroup);
            }
            action = BarrierAction::BarrierOnWrite;
            continue;
        }
        if (IsWriteShared(inst)) {
            if (action == BarrierAction::BarrierOnWrite) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                EmitSync(ir, subgroup);
            }
            action = BarrierAction::BarrierOnRead;
        }
    }
    if (action != BarrierAction::None) {
        IR::IREmitter ir{*block, --block->end()};
        EmitSync(ir, subgroup);
    }
}

using NodeSet = std::unordered_set<const IR::Block*>;

static void EmitBarrierAtBlockStart(IR::Block* block, bool subgroup) {
    auto insert_point = std::ranges::find_if_not(block->Instructions(), IR::IsPhi);
    IR::IREmitter ir{*block, insert_point};
    EmitSync(ir, subgroup);
}

struct DivergenceContext {
    // Blocks whose ReadLane with a constant lane is uniform in the workgroup: one wave (the
    // wave64 lowering exchanges it through LDS on 32-wide hardware).
    std::unordered_set<const IR::Block*> uniform_readlane_blocks;
    // Synchronize host subgroups only (every guest wave inside one host subgroup).
    bool subgroup{};
};

// A condition is divergent when it depends on LocalInvocationId (or, with TLG_LANE_DIVERGENCE,
// another lane-varying source: IsLaneVaryingSource). With uniform_readlane_blocks,
// the search does not continue through uniform ReadLanes: a wave-wide reduction (a tile min/max
// depth, for example) is the same in every invocation even though its inputs are not.
// From Pink-shadPS4 (luizgustavs), lds_barrier_uniform_readlane, via SotC.
static bool SearchDivergence(const IR::U1& cond,
                             const std::unordered_set<const IR::Block*>* uniform_readlane_blocks) {
    if (cond.IsImmediate()) {
        return false;
    }
    std::unordered_set<const IR::Inst*> visited{cond.Inst()};
    std::queue<const IR::Inst*> queue;
    queue.push(cond.Inst());
    while (!queue.empty()) {
        const IR::Inst* inst = queue.front();
        queue.pop();
        if (IsLaneVaryingSource(*inst)) {
            return true;
        }
        if (uniform_readlane_blocks && inst->GetOpcode() == IR::Opcode::ReadLane &&
            inst->Arg(1).IsImmediate() && uniform_readlane_blocks->contains(inst->GetParent())) {
            continue;
        }
        for (size_t arg = inst->NumArgs(); arg--;) {
            const IR::Value value = inst->Arg(arg);
            if (value.IsImmediate()) {
                continue;
            }
            const IR::Inst* arg_inst = value.Inst();
            if (visited.insert(arg_inst).second) {
                queue.push(arg_inst);
            }
        }
    }
    return false;
}

static bool IsDivergent(const IR::U1& cond, const DivergenceContext& ctx) {
    return SearchDivergence(cond, ctx.uniform_readlane_blocks.empty()
                                      ? nullptr
                                      : &ctx.uniform_readlane_blocks);
}

// Inserts a barrier after divergent conditional blocks to avoid undefined
// behavior when some threads write and others read from shared memory.
static void EmitBarrierInMergeBlock(const IR::AbstractSyntaxNode::Data& data,
                                    NodeSet& divergence_end, u32& divergence_depth,
                                    const DivergenceContext& ctx) {
    const IR::U1 cond = data.if_node.cond;
    if (IsDivergent(cond, ctx)) {
        if (divergence_depth == 0) {
            EmitBarrierAtBlockStart(data.if_node.merge, ctx.subgroup);
        }
        ++divergence_depth;
        divergence_end.emplace(data.if_node.merge);
    }
}

// A barrier inside a loop is invalid when different invocations leave on different iterations.
// Mark such loops so their shared-memory synchronization can be deferred to the merge block.
static NodeSet FindDivergentLoops(const IR::AbstractSyntaxList& syntax_list,
                                  const DivergenceContext& ctx) {
    NodeSet divergent_loops;
    for (const IR::AbstractSyntaxNode& node : syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Repeat:
            if (IsDivergent(node.data.repeat.cond, ctx)) {
                divergent_loops.emplace(node.data.repeat.merge);
            }
            break;
        case IR::AbstractSyntaxNode::Type::Break:
            if (IsDivergent(node.data.break_node.cond, ctx)) {
                divergent_loops.emplace(node.data.break_node.merge);
            }
            break;
        default:
            break;
        }
    }
    return divergent_loops;
}

static constexpr u32 GcnSubgroupSize = 64;

// From SotC (AjncSystem/ShadowOfTheColossus-PC):
// - Workgroups smaller than a wave are one partial GCN wave: their lanes write LDS and read the
//   values of the other lanes with no s_barrier at all. On the host the compiler may move those
//   reads before the writes, so they read stale LDS: SotC's particle count prefix sum (16
//   threads) gave 0 instances to every GPU particle draw. They get workgroup barriers like a
//   full wave (TLG_LDS_SMALL).
// - Workgroups of several waves synchronize with s_barrier between waves, but the last steps of
//   a reduction (s[tid] += s[tid + 32] for tid < 32, ...) run inside one wave and rely on its
//   lanes executing in lockstep. A host subgroup does not guarantee that, and the compiler may
//   reorder the loads of other lanes' values (SotC's auto-exposure averaged to 0.78 instead of
//   2.06). Those workgroups get barriers at the same points (TLG_LDS_MULTI).
// tlg: the scope comes from how the guest waves land on host subgroups. The workgroup is cut
// into 64-lane guest waves, the last one partial when the size is not a multiple of 64 (96 =
// 64 + 32). If every guest wave fits inside one host subgroup, the hazards stay inside one
// subgroup and a subgroup barrier is enough (and does not wait on the other waves, which the
// guest never does). Otherwise a guest wave spans several host subgroups: on 32-wide hardware
// the step "tid < 32 reads s[tid + 32]" reads what the other host subgroup wrote, which only a
// workgroup barrier orders. Any doubt (unknown subgroup size) means a workgroup barrier.
static bool GuestWavesFitHostSubgroups(u32 threadgroup_size, u32 host_subgroup_size) {
    if (host_subgroup_size == 0 || host_subgroup_size % GcnSubgroupSize != 0) {
        // Narrower host subgroups (or an unknown size): only a workgroup that is itself no
        // larger than one host subgroup avoids crossing them, and that is one subgroup anyway.
        return false;
    }
    for (u32 start = 0; start < threadgroup_size; start += GcnSubgroupSize) {
        const u32 last = std::min(start + GcnSubgroupSize, threadgroup_size) - 1;
        if (start / host_subgroup_size != last / host_subgroup_size) {
            return false;
        }
    }
    return true;
}

void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute) {
        return;
    }
    const auto& cs_info = runtime_info.hw.cs;
    const u32 shared_memory_size = cs_info.shared_memory_size;
    const u32 threadgroup_size =
        cs_info.workgroup_size[0] * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    // The compiler can only omit barriers when the local workgroup size is the same as the HW
    // subgroup. tlg: every size other than 64 is now covered, 96 or 192 included (they used to
    // get no barriers at all).
    const bool multi_wave = threadgroup_size > GcnSubgroupSize && Toggles::MultiWaveLdsBarriers();
    const bool partial_wave = threadgroup_size < GcnSubgroupSize && Toggles::SmallGroupLdsBarriers();
    if (shared_memory_size == 0 || threadgroup_size == 0 ||
        (threadgroup_size != GcnSubgroupSize && !multi_wave && !partial_wave) ||
        !profile.needs_lds_barriers) {
        return;
    }
    using Type = IR::AbstractSyntaxNode::Type;
    DivergenceContext ctx;
    ctx.subgroup = multi_wave && GuestWavesFitHostSubgroups(threadgroup_size, profile.subgroup_size);
    // A ReadLane is uniform within one wave, not across the waves of a larger workgroup.
    if (!multi_wave && Toggles::LdsUniformReadLane()) {
        const auto blocks = FindWave64UniformBlocks(program);
        ctx.uniform_readlane_blocks.insert(blocks.begin(), blocks.end());
    }
    u32 divergence_depth{};
    NodeSet divergence_end;
    const NodeSet divergent_loops = FindDivergentLoops(program.syntax_list, ctx);
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::EndIf) {
            if (divergence_end.contains(node.data.end_if.merge)) {
                --divergence_depth;
            }
            continue;
        }
        // Check if branch depth is zero, we don't want to insert barrier in potentially divergent
        // code.
        if (node.type == Type::If) {
            EmitBarrierInMergeBlock(node.data, divergence_end, divergence_depth, ctx);
            continue;
        }
        if (node.type == Type::Loop && divergent_loops.contains(node.data.loop.merge)) {
            ++divergence_depth;
            continue;
        }
        if (node.type == Type::Repeat && divergent_loops.contains(node.data.repeat.merge)) {
            ASSERT(divergence_depth > 0);
            --divergence_depth;
            if (divergence_depth == 0) {
                EmitBarrierAtBlockStart(node.data.repeat.merge, ctx.subgroup);
            }
            continue;
        }
        if (node.type == Type::Block && divergence_depth == 0) {
            EmitBarrierInBlock(node.data.block, ctx.subgroup);
        }
    }
}

// --- Wave memory barriers (ported from iss_pc renderer 2, WaveMemoryBarrierPass) ---------------

static bool IsLoadBuffer(const IR::Inst& inst) {
    const IR::Opcode opcode = inst.GetOpcode();
    return opcode >= IR::Opcode::LoadBufferU8 && opcode <= IR::Opcode::LoadBufferFormatF32;
}

static bool IsStoreBuffer(const IR::Inst& inst) {
    const IR::Opcode opcode = inst.GetOpcode();
    return opcode >= IR::Opcode::StoreBufferU8 && opcode <= IR::Opcode::StoreBufferFormatF32;
}

// Values that may differ between the invocations of a workgroup split across host subgroups:
// invocation ids, subgroup operations (only uniform within one subgroup) and atomics (each
// invocation receives its own previous value).
static bool IsWorkgroupVaryingSource(const IR::Inst* inst) {
    const IR::Opcode opcode = inst->GetOpcode();
    if ((opcode >= IR::Opcode::SharedAtomicIAdd32 && opcode <= IR::Opcode::SharedAtomicCmpSwap64) ||
        (opcode >= IR::Opcode::BufferAtomicIAdd32 && opcode <= IR::Opcode::BufferAtomicFCmpSwap32) ||
        (opcode >= IR::Opcode::ImageAtomicIAdd32 && opcode <= IR::Opcode::ImageAtomicCmpSwap32)) {
        return true;
    }
    switch (opcode) {
    case IR::Opcode::GetAttributeU32: {
        const IR::Attribute attribute = inst->Arg(0).Attribute();
        return attribute == IR::Attribute::LocalInvocationId ||
               attribute == IR::Attribute::LocalInvocationIndex ||
               attribute == IR::Attribute::GlobalInvocationId ||
               attribute == IR::Attribute::SubgroupLtMask;
    }
    case IR::Opcode::LaneId:
    case IR::Opcode::Ballot:
    case IR::Opcode::BallotFindLsb:
    case IR::Opcode::ReadLane:
    case IR::Opcode::ReadFirstLane:
    case IR::Opcode::WriteLane:
    case IR::Opcode::QuadBroadcast:
    case IR::Opcode::MaskedBitCount32:
    case IR::Opcode::DataAppend:
    case IR::Opcode::DataConsume:
        return true;
    default:
        return false;
    }
}

static bool IsWorkgroupDivergent(const IR::U1& cond) {
    if (cond.IsImmediate()) {
        return false;
    }
    return IR::BreadthFirstSearch(cond.Inst(), [](IR::Inst* inst) -> std::optional<bool> {
               if (IsWorkgroupVaryingSource(inst)) {
                   return true;
               }
               return std::nullopt;
           }) == true;
}

// Instructions whose order with the barriers of a workgroup matters: memory accesses and
// barriers. Code after a conditional block without them cannot observe which invocations ran it.
static bool HasOrderedInst(const IR::Block& block) {
    for (const IR::Inst& inst : block.Instructions()) {
        const IR::Opcode opcode = inst.GetOpcode();
        if (IsLoadBuffer(inst) || IsStoreBuffer(inst) || IsLoadShared(inst) ||
            IsWriteShared(inst) || opcode == IR::Opcode::Barrier ||
            opcode == IR::Opcode::WorkgroupMemoryBarrier ||
            opcode == IR::Opcode::DeviceMemoryBarrier ||
            (opcode >= IR::Opcode::SharedAtomicIAdd32 &&
             opcode <= IR::Opcode::SharedAtomicCmpSwap64) ||
            (opcode >= IR::Opcode::BufferAtomicIAdd32 &&
             opcode <= IR::Opcode::BufferAtomicFCmpSwap32) ||
            (opcode >= IR::Opcode::ImageAtomicIAdd32 &&
             opcode <= IR::Opcode::ImageAtomicCmpSwap32) ||
            opcode == IR::Opcode::ImageWrite || opcode == IR::Opcode::DataAppend ||
            opcode == IR::Opcode::DataConsume) {
            return true;
        }
    }
    return false;
}

// GCN waves run their 64 invocations in lockstep: within a wave, code can exchange data through
// buffer memory without barriers, ordering its stores before the loads of the next step with
// s_waitcnt only (the GPU particle sort merging runs no longer than a wave in a loop). Hosts with
// smaller subgroups (Apple and NVIDIA: 32) split each wave into parts that run independently, and
// the compiler may move the loads of a step before the stores of the previous one: the steps
// race, the sorted list gets duplicated and missing particles, and the smoke flickers (iss_pc
// #557: 1203-1632 duplicated quads a frame of ps 24dad4be / vs bbd2d927 -> 0). Workgroups spanning
// several host subgroups are synchronized after the buffer stores of their uniform loops that
// also load buffer memory. Invocations that skipped such a loop with a condition that leads to
// the end of the shader do not take part in the barriers.
void WaveMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile) {
    if (!Toggles::WaveMemoryBarriers() || program.info.hw_stage != HwStage::Compute) {
        return;
    }
    const auto& cs_info = runtime_info.hw.cs;
    const u32 threadgroup_size =
        cs_info.workgroup_size[0] * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    if (profile.subgroup_size == 0 || threadgroup_size <= profile.subgroup_size) {
        return;
    }
    using Type = IR::AbstractSyntaxNode::Type;
    const IR::AbstractSyntaxList& list = program.syntax_list;
    const size_t num_nodes = list.size();

    // Matching structure nodes: the EndIf of each If, the Repeat of each Loop (by merge block).
    std::vector<size_t> end_of(num_nodes, num_nodes);
    {
        std::vector<size_t> open;
        for (size_t i = 0; i < num_nodes; ++i) {
            const auto& node = list[i];
            if (node.type == Type::If || node.type == Type::Loop) {
                open.push_back(i);
            } else if (node.type == Type::EndIf || node.type == Type::Repeat) {
                if (open.empty()) {
                    return;
                }
                const size_t start = open.back();
                open.pop_back();
                const auto& start_node = list[start];
                const bool matches =
                    node.type == Type::EndIf
                        ? start_node.type == Type::If &&
                              start_node.data.if_node.merge == node.data.end_if.merge
                        : start_node.type == Type::Loop &&
                              start_node.data.loop.merge == node.data.repeat.merge;
                if (!matches) {
                    return; // Unexpected nesting: leave the shader as it is.
                }
                end_of[start] = i;
            }
        }
        if (!open.empty()) {
            return;
        }
    }
    // An If is a tail if when nothing after its EndIf accesses memory or synchronizes: the
    // invocations that do not take it only leave the shader.
    std::vector<bool> inert_suffix(num_nodes + 1, true);
    for (size_t i = num_nodes; i-- > 0;) {
        const auto& node = list[i];
        bool inert = inert_suffix[i + 1];
        switch (node.type) {
        case Type::Block:
            inert = inert && !HasOrderedInst(*node.data.block);
            break;
        case Type::EndIf:
        case Type::Return:
        case Type::Unreachable:
            break;
        default:
            inert = false;
            break;
        }
        inert_suffix[i] = inert;
    }
    const auto loads_in_range = [&](size_t first, size_t last) {
        for (size_t i = first; i <= last && i < num_nodes; ++i) {
            if (list[i].type != Type::Block) {
                continue;
            }
            for (const IR::Inst& inst : list[i].data.block->Instructions()) {
                if (IsLoadBuffer(inst)) {
                    return true;
                }
            }
        }
        return false;
    };
    NodeSet divergent_loops;
    for (const IR::AbstractSyntaxNode& node : list) {
        if (node.type == Type::Repeat && IsWorkgroupDivergent(node.data.repeat.cond)) {
            divergent_loops.emplace(node.data.repeat.merge);
        } else if (node.type == Type::Break && IsWorkgroupDivergent(node.data.break_node.cond)) {
            divergent_loops.emplace(node.data.break_node.merge);
        }
    }

    struct Scope {
        size_t end;
        bool divergent;    ///< Invocations of the workgroup may take different paths in it.
        bool uniform_loop; ///< A loop all invocations iterate together.
        bool loop_loads;   ///< The loop body loads buffer memory.
    };
    std::vector<Scope> scopes;
    u32 num_barriers{};
    for (size_t i = 0; i < num_nodes; ++i) {
        while (!scopes.empty() && scopes.back().end < i) {
            scopes.pop_back();
        }
        const auto& node = list[i];
        if (node.type == Type::If) {
            const size_t end = end_of[i];
            const bool tail = end < num_nodes && inert_suffix[end + 1];
            const bool divergent = !tail && IsWorkgroupDivergent(node.data.if_node.cond);
            scopes.push_back({end, divergent, false, false});
            continue;
        }
        if (node.type == Type::Loop) {
            const size_t end = end_of[i];
            const bool divergent = divergent_loops.contains(node.data.loop.merge);
            scopes.push_back({end, divergent, !divergent, !divergent && loads_in_range(i, end)});
            continue;
        }
        if (node.type != Type::Block) {
            continue;
        }
        bool divergent = false;
        bool in_loop_with_loads = false;
        for (const auto& scope : scopes) {
            divergent |= scope.divergent;
        }
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            if (it->uniform_loop) {
                in_loop_with_loads = it->loop_loads;
                break;
            }
        }
        if (divergent || !in_loop_with_loads) {
            continue;
        }
        IR::Block* const block = node.data.block;
        // A barrier after each store followed by a load in the block, and after the last store.
        bool pending_store = false;
        for (auto it = block->Instructions().begin(); it != block->Instructions().end(); ++it) {
            if (IsLoadBuffer(*it) && pending_store) {
                IR::IREmitter ir{*block, it};
                ir.WorkgroupMemoryBarrier();
                ir.Barrier();
                ++num_barriers;
                pending_store = false;
            }
            if (IsStoreBuffer(*it)) {
                pending_store = true;
            }
        }
        if (pending_store) {
            auto last_store = block->Instructions().end();
            for (auto it = block->Instructions().begin(); it != block->Instructions().end(); ++it) {
                if (IsStoreBuffer(*it)) {
                    last_store = it;
                }
            }
            IR::IREmitter ir{*block, std::next(last_store)};
            ir.WorkgroupMemoryBarrier();
            ir.Barrier();
            ++num_barriers;
        }
    }
    if (num_barriers != 0) {
        LOG_INFO(Render_Recompiler,
                 "Compute shader {:#x}: {} buffer memory barriers for {} invocations exchanging "
                 "data within GCN waves",
                 program.info.pgm_hash, num_barriers, threadgroup_size);
    }
}

} // namespace Shader::Optimization
