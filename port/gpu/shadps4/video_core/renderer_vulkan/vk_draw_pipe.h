// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: two-stage draw pipeline (docs/parallel_gpu.md, "Two-stage draw pipeline").
//
// The GPU command thread (stage A) decodes PM4, keeps the register file and selects pipelines;
// for a direct draw it writes a packet (the register blocks changed since the previous packet,
// the stages' user data, the draw parameters) into this ring and goes on decoding. The draw
// recording thread (stage B) owns the caches, the barriers and the command recording while
// packets are in flight: it applies each packet to its own copy of the registers and runs the
// rest of the draw. Anything else stage A does on that state waits for stage B to run dry
// (Drain) and then runs on stage A as before.

#pragma once

#include <cstdlib>
#include "tlg_threads.h"
#ifndef _WIN32
#include <pthread.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include "common/cpu_relax.h"

#include "common/assert.h"
#include "common/thread.h"
#include "common/types.h"

namespace Vulkan {

class DrawPipe {
public:
    /// Runs a packet on stage B.
    using Handler = void (*)(void* context, const u8* packet, u32 size);

    /// tlg: lanes. Lane 0 carries everything by default; with TLG_ACB_LANE=1 the game's compute
    /// queues post into lane 1, which stage B serves first (docs/DESIGN_ACB_LANE.md). Positions
    /// (Head, Consumed, Reached) are packet sequence numbers over both lanes: a position is reached
    /// once every packet committed before it, in either lane, has run.
    static constexpr u32 NumLanes = 2;

    DrawPipe(Handler handler_, void* context_) : handler{handler_}, context{context_} {
        for (auto& lane : lanes) {
            lane.ring = std::make_unique<u8[]>(Capacity);
        }
        thread = std::jthread([this](std::stop_token stop) { Run(stop); });
    }

    ~DrawPipe() {
        Drain();
        thread.request_stop();
        published_seq.fetch_add(0, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// True on the draw recording thread.
    [[nodiscard]] static bool OnStageB() noexcept {
        return on_stage_b;
    }

    [[nodiscard]] u32 StageBThreadId() const noexcept {
        return stage_b_tid.load(std::memory_order_acquire);
    }

    /// Stage A: the lane the next packets go to (0 unless TLG_ACB_LANE routes compute there).
    void SetLane(u32 lane) noexcept {
        current_lane = lane < NumLanes ? lane : 0;
    }
    [[nodiscard]] u32 CurrentLane() const noexcept {
        return current_lane;
    }
    /// Stage A: the next packet of `lane` runs only after every packet committed so far in the
    /// other lane (a wait of that lane met by a fence still queued in the other).
    void GateNext(u32 lane) noexcept {
        if (lane < NumLanes) {
            lanes[lane].gate = seq_head;
        }
    }

    /// Stage A: space for a packet of `size` bytes (8-byte aligned), valid until Commit().
    u8* Begin(u32 size) {
        auto& lane = lanes[current_lane];
        pending_lane = current_lane;
        size = Align(size + sizeof(Header));
        ASSERT(size <= Capacity / 4);
        u64 at = lane.head;
        const u64 offset = at % Capacity;
        if (offset + size > Capacity) {
            // Wrap: the rest of the ring is skipped (a header with zero size marks it).
            WaitForSpace(lane, at, Capacity - offset);
            reinterpret_cast<Header*>(lane.ring.get() + offset)->size = 0;
            at += Capacity - offset;
            lane.head = at;
            lane.published.store(at, std::memory_order_release);
        }
        WaitForSpace(lane, at, size);
        pending_size = size;
        auto* header = reinterpret_cast<Header*>(lane.ring.get() + at % Capacity);
        header->size = size;
        return reinterpret_cast<u8*>(header + 1);
    }

    /// Stage A: hands the packet from Begin() to stage B.
    void Commit(u32 payload_size) {
        auto& lane = lanes[pending_lane];
        auto* header = reinterpret_cast<Header*>(lane.ring.get() + lane.head % Capacity);
        header->payload = payload_size;
        header->seq = seq_head;
        header->gate = std::exchange(lane.gate, 0);
        lane.head += pending_size;
        // Published in the lane before the sequence count: stage B reads the count first, so a
        // packet it counts is always visible in its lane.
        lane.published.store(lane.head, std::memory_order_seq_cst);
        ++seq_head;
        published_seq.store(seq_head, std::memory_order_seq_cst);
        ++packets;
        if (sleeping.load(std::memory_order_seq_cst)) {
            wake.fetch_add(1, std::memory_order_seq_cst);
            wake.notify_one();
        }
    }

    /// Why stage A drained (statistics): a PM4 opcode, or one of these.
    enum Reason : u32 {
        ReasonConstRam = 256,
        ReasonCommands,
        ReasonCompute,
        ReasonSubmissionEnd,
        ReasonDraw,
        ReasonRasterizer,
        NumReasons,
    };
    std::array<u64, NumReasons> drains_by_reason{};
    std::array<u64, NumReasons> cycles_by_reason{};

    /// tlg: where the asynchronous compute queues spend stage A, by step (TLG_DCB_STATS): a PM4
    /// opcode of the ACB, or ReasonCommands for the ProcessCommands call before each packet. The
    /// drain of each packet is counted apart, in cycles_by_reason[ReasonCompute].
    std::array<u64, NumReasons> compute_cycles{};
    std::array<u64, NumReasons> compute_packets{};

    /// Stage A: waits until stage B has run every committed packet.
    void Drain(u32 reason = ReasonRasterizer) {
        if (done.load(std::memory_order_acquire) == seq_head) {
            return;
        }
        ++drains;
        const u32 slot = reason < NumReasons ? reason : ReasonRasterizer;
        ++drains_by_reason[slot];
        const u64 start = Common::CpuCycles();
        for (u32 spins = 0; done.load(std::memory_order_acquire) != seq_head; ++spins) {
            if (spins < 4096) {
                Common::CpuRelax();
            } else if (SleepInDrain()) {
                SleepUntilDrained();
                break;
            } else {
                std::this_thread::yield();
            }
        }
        const u64 waited = Common::CpuCycles() - start;
        drain_cycles += waited;
        cycles_by_reason[slot] += waited;
    }

    /// Stage A: position after the last committed packet; Reached(position) once B ran it.
    [[nodiscard]] u64 Head() const noexcept {
        return seq_head;
    }
    /// Stage B's position: read once for many comparisons (stage B writes it constantly, so each
    /// read is a cache miss between cores).
    [[nodiscard]] u64 Consumed() const noexcept {
        return done.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool Reached(u64 position) const noexcept {
        return done.load(std::memory_order_acquire) >= position;
    }

    [[nodiscard]] bool Idle() const noexcept {
        return done.load(std::memory_order_acquire) == seq_head;
    }

    /// Statistics (stage A): packets, drains that had to wait, cycles waited, cycles stage B
    /// spent running packets, ring-full waits and their cycles.
    u64 packets = 0, drains = 0, drain_cycles = 0, ring_waits = 0, ring_wait_cycles = 0;
    std::atomic<u64> busy_cycles{0};
    /// tlg: packets run per lane, and lane-1 packets run ahead of queued lane-0 packets (stage B).
    std::array<std::atomic<u64>, NumLanes> lane_packets{};
    std::atomic<u64> lane_overtakes{0};
    /// Stage B, inside a packet: its lane and sequence number.
    [[nodiscard]] u32 RunningLane() const noexcept {
        return running_lane;
    }
    [[nodiscard]] u64 RunningSeq() const noexcept {
        return running_seq;
    }
    /// Stage B: true when `lane` holds no packet below `seq` that has not run yet.
    [[nodiscard]] bool LaneClearBelow(u32 lane, u64 seq) {
        return NextSeq(lanes[lane]) >= seq;
    }
    /// tlg: called on stage B when lane 1 has just run dry (the end of a compute burst), while
    /// lane 0 may still hold packets: what the idle handler does for compute work.
    void SetLaneIdleHandler(void (*handler)(void*), void* context) noexcept {
        lane_idle_context = context;
        lane_idle_handler = handler;
    }
    /// tlg: called on stage B when it has run out of packets, before it spins or sleeps.
    void SetIdleHandler(void (*handler)(void*), void* context) noexcept {
        idle_context = context;
        idle_handler = handler;
    }

    /// tlg: stage B spinning for a packet before it sleeps (counted on the recording thread,
    /// read by the statistics; not part of busy_cycles, which is only the packet bodies).
    std::atomic<u64> spin_cycles{0};
    std::atomic<u64> sleeps{0};
    /// tlg: the recording thread's CPU clock (Linux) / thread id (Windows), for the frame
    /// statistics (as TlgStats::gpu_thread_clock does for the command thread).
    std::atomic<int> cpu_clock{-1};
    std::atomic<u32> cpu_tid{0};

private:
    struct Header {
        u32 size;    ///< bytes to the next packet (0: wrap to the ring start)
        u32 payload; ///< bytes written by the producer
        u64 seq;     ///< sequence number over both lanes
        u64 gate;    ///< runs once the other lane has run every packet below this sequence
    };
    static constexpr u64 Capacity = 16ull << 20;
    static constexpr u64 NoPacket = ~0ull;

    struct Lane {
        std::unique_ptr<u8[]> ring;
        u64 head = 0; ///< stage A's write position (bytes)
        u64 gate = 0; ///< stage A: gate of the next packet
        alignas(64) std::atomic<u64> published{0};
        alignas(64) std::atomic<u64> consumed{0};
        alignas(64) std::atomic<u32> space_waiters{0}; ///< stage A threads asleep waiting for room
        u64 at = 0; ///< stage B's read position (bytes)
    };

    static constexpr u32 Align(u32 size) {
        return (size + 63) & ~63u;
    }

    /// tlg: a drain that outlived its spin sleeps on `done` instead of yielding. The yield
    /// loop kept a core busy for the whole wait (perf: ~8% of all samples in sched_yield on a
    /// 16-thread machine) and takes a core from the game's threads on CPUs with few. Measured
    /// on the 16-thread machine: no FPS difference beyond run-to-run noise (30.8-32.9 either
    /// way), the GPU command thread from 47% to 35% of a core. TLG_DRAIN_SLEEP=0 restores the
    /// yield loop for comparisons.
    static bool SleepInDrain() {
        static const bool sleep = [] {
            const char* env = std::getenv("TLG_DRAIN_SLEEP");
            return !(env && *env == '0');
        }();
        return sleep;
    }
    void SleepUntilDrained() {
        for (;;) {
            const u64 seen = done.load(std::memory_order_seq_cst);
            if (seen == seq_head) {
                return;
            }
            // Counted before the check, and stage B reads the count after its store: one of
            // the two sees the other, so no wake-up is lost. The target is stored first, so
            // stage B sees it with the count.
            drain_target.store(seq_head, std::memory_order_seq_cst);
            drain_waiters.fetch_add(1, std::memory_order_seq_cst);
            if (done.load(std::memory_order_seq_cst) == seen) {
                done.wait(seen, std::memory_order_seq_cst);
            }
            drain_waiters.fetch_sub(1, std::memory_order_seq_cst);
        }
    }
    /// Stage B, after advancing a lane and `done`.
    /// tlg: a drain is woken once stage B has reached the position it waits for, not after every
    /// packet (TLG_DRAIN_TARGET=0: every packet, as before 0.1.12). Woken per packet, the GPU
    /// command thread re-armed libc++'s atomic wait each time, which polls the clock for tens of
    /// microseconds before it sleeps: ~40% of a core spent waiting (mach_continuous_time).
    void NotifyDrain(Lane& lane, u64 now_done) {
        static const bool targeted = [] {
            const char* env = std::getenv("TLG_DRAIN_TARGET");
            return !(env && *env == '0');
        }();
        if (lane.space_waiters.load(std::memory_order_seq_cst) != 0) {
            lane.consumed.notify_all();
        }
        if (drain_waiters.load(std::memory_order_seq_cst) != 0 &&
            (!targeted || now_done >= drain_target.load(std::memory_order_seq_cst))) {
            done.notify_all();
        }
    }
    void WaitForSpace(Lane& lane, u64 at, u64 size) {
        if (at + size - lane.consumed.load(std::memory_order_acquire) <= Capacity) {
            return;
        }
        // tlg: the ring holds 16 MB (many frames of packets): a full ring means stage B is far
        // behind, so this wait is worth reporting (TLG_FRAME_STATS).
        ++ring_waits;
        const u64 start = Common::CpuCycles();
        for (u32 spins = 0; at + size - lane.consumed.load(std::memory_order_acquire) > Capacity;
             ++spins) {
            if (spins < 4096) {
                Common::CpuRelax();
            } else if (RingSleep()) {
                SleepUntilSpace(lane, at + size);
                break;
            } else {
                std::this_thread::yield();
            }
        }
        ring_wait_cycles += Common::CpuCycles() - start;
    }

    /// tlg: TLG_PIPE_RING_SLEEP=1 sleeps on `consumed` (as an outlived drain does) instead of
    /// spinning; the yield loop above is what ships until this is measured.
    static bool RingSleep() {
        static const bool sleep = [] {
            const char* env = std::getenv("TLG_PIPE_RING_SLEEP");
            return env && *env == '1';
        }();
        return sleep;
    }
    void SleepUntilSpace(Lane& lane, u64 end) {
        while (end - lane.consumed.load(std::memory_order_seq_cst) > Capacity) {
            const u64 seen = lane.consumed.load(std::memory_order_seq_cst);
            if (end - seen <= Capacity) {
                return;
            }
            lane.space_waiters.fetch_add(1, std::memory_order_seq_cst);
            if (lane.consumed.load(std::memory_order_seq_cst) == seen) {
                lane.consumed.wait(seen, std::memory_order_seq_cst);
            }
            lane.space_waiters.fetch_sub(1, std::memory_order_seq_cst);
        }
    }

    /// Stage B: skips wrap markers; the header of the lane's next packet, or null when empty.
    const Header* Peek(Lane& lane) {
        for (;;) {
            if (lane.published.load(std::memory_order_acquire) == lane.at) {
                return nullptr;
            }
            const auto* header = reinterpret_cast<const Header*>(lane.ring.get() + lane.at % Capacity);
            if (header->size != 0) {
                return header;
            }
            lane.at += Capacity - lane.at % Capacity;
            lane.consumed.store(lane.at, std::memory_order_seq_cst);
        }
    }
    /// Stage B: sequence of the lane's next packet (NoPacket when it is empty).
    u64 NextSeq(Lane& lane) {
        const Header* header = Peek(lane);
        return header ? header->seq : NoPacket;
    }
    /// Stage B: every packet below the result has run (both lanes).
    u64 ComputeDone() {
        // The count first: a packet below it is visible in its lane by now (Commit's order).
        u64 result = published_seq.load(std::memory_order_seq_cst);
        for (auto& lane : lanes) {
            result = std::min(result, NextSeq(lane));
        }
        return result;
    }

    void (*idle_handler)(void*) = nullptr;
    void* idle_context = nullptr;
    void (*lane_idle_handler)(void*) = nullptr;
    void* lane_idle_context = nullptr;

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("tlg:DrawRec");
        on_stage_b = true;
        stage_b_tid.store(static_cast<u32>(gettid()), std::memory_order_release);
#if defined(__APPLE__)
        // macOS: no per-thread CPU clock ids (pthread_getcpuclockid); the statistic stays unset.
#elif !defined(_WIN32)
        if (clockid_t clock; pthread_getcpuclockid(pthread_self(), &clock) == 0) {
            cpu_clock.store(static_cast<int>(clock), std::memory_order_release);
        }
#else
        cpu_tid.store(static_cast<u32>(GetCurrentThreadId()), std::memory_order_release);
#endif
        const auto any_packet = [&] {
            return lanes[0].published.load(std::memory_order_acquire) != lanes[0].at ||
                   lanes[1].published.load(std::memory_order_acquire) != lanes[1].at;
        };
        while (true) {
            // Packets follow each other within microseconds while a frame is decoded: spin,
            // and sleep only after a longer pause (between frames).
            if (!any_packet() && idle_handler) {
                idle_handler(idle_context);
            }
            if (!any_packet()) {
                // With few cores (Steam Deck: 8 threads) a long spin takes time from the
                // game's own threads; TLG_PHYS_CORES=1 counts physical cores instead of SMT
                // threads here. TLG_PIPE_SPIN_US overrides.
                static const auto spin_time = std::chrono::microseconds([] {
                    if (const char* env = std::getenv("TLG_PIPE_SPIN_US")) {
                        return std::max(0, std::atoi(env));
                    }
                    return TlgThreads::SizingCount() >= 12 ? 200 : 50;
                }());
                const auto spin_until = std::chrono::steady_clock::now() + spin_time;
                const u64 spin_start = Common::CpuCycles();
                for (u32 spins = 1; !any_packet(); ++spins) {
                    if (stop.stop_requested()) {
                        return;
                    }
                    Common::CpuRelax();
                    if (!(spins & 255) && std::chrono::steady_clock::now() >= spin_until) {
                        const u32 seen = wake.load(std::memory_order_seq_cst);
                        sleeping.store(true, std::memory_order_seq_cst);
                        if (!any_packet() && !stop.stop_requested()) {
                            sleeps.fetch_add(1, std::memory_order_relaxed);
                            wake.wait(seen, std::memory_order_seq_cst);
                        }
                        sleeping.store(false, std::memory_order_relaxed);
                    }
                }
                // tlg: how much of stage B is spent spinning for a packet (a core taken from
                // the game's threads), reported as idle, not as work.
                spin_cycles.fetch_add(Common::CpuCycles() - spin_start, std::memory_order_relaxed);
            }
            // Lane 1 first, unless it must wait for lane 0 (its gate); lane 0 likewise.
            const Header* h1 = Peek(lanes[1]);
            const Header* h0 = Peek(lanes[0]);
            u32 pick = NumLanes;
            if (h1 && (h1->gate == 0 || NextSeq(lanes[0]) >= h1->gate)) {
                pick = 1;
            } else if (h0 && (h0->gate == 0 || NextSeq(lanes[1]) >= h0->gate)) {
                pick = 0;
            }
            if (pick == NumLanes) {
                // Both heads wait for the other lane's progress: cannot happen (a gate names
                // packets committed earlier); spin until the producer catches up.
                Common::CpuRelax();
                ComputeDoneAndNotify(lanes[0]);
                continue;
            }
            auto& lane = lanes[pick];
            const Header* header = pick ? h1 : h0;
            if (pick == 1 && h0 && h0->seq < h1->seq) {
                lane_overtakes.fetch_add(1, std::memory_order_relaxed);
            }
            running_lane = pick;
            running_seq = header->seq;
            const u64 start = Common::CpuCycles();
            handler(context, reinterpret_cast<const u8*>(header + 1), header->payload);
            busy_cycles.fetch_add(Common::CpuCycles() - start, std::memory_order_relaxed);
            lane_packets[pick].fetch_add(1, std::memory_order_relaxed);
            lane.at += header->size;
            lane.consumed.store(lane.at, std::memory_order_seq_cst);
            ComputeDoneAndNotify(lane);
            if (pick == 1 && lane_idle_handler && !Peek(lanes[1]) && Peek(lanes[0])) {
                lane_idle_handler(lane_idle_context);
            }
        }
    }
    void ComputeDoneAndNotify(Lane& lane) {
        const u64 now_done = ComputeDone();
        done.store(now_done, std::memory_order_seq_cst);
        NotifyDrain(lane, now_done);
    }

    static inline thread_local bool on_stage_b = false;
    Handler handler;
    void* context;
    std::array<Lane, NumLanes> lanes;
    u32 running_lane = 0; ///< stage B: lane of the packet being run
    u64 running_seq = 0;  ///< stage B: its sequence number
    u32 current_lane = 0; ///< stage A
    u32 pending_lane = 0; ///< stage A: lane of the packet between Begin and Commit
    u64 seq_head = 0;     ///< stage A: sequence number of the next packet
    u32 pending_size = 0;
    alignas(64) std::atomic<u64> published_seq{0};
    alignas(64) std::atomic<u64> done{0}; ///< stage B: every packet below this has run
    alignas(64) std::atomic<u32> wake{0};
    alignas(64) std::atomic<u32> drain_waiters{0}; ///< stage A threads asleep in a drain
    std::atomic<u64> drain_target{0}; ///< tlg: the position a sleeping drain waits for
    std::atomic<bool> sleeping{false};
    std::atomic<u32> stage_b_tid{0};
    std::jthread thread;
};

} // namespace Vulkan
