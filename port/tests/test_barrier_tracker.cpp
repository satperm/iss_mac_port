// SPDX-License-Identifier: GPL-2.0-or-later

// tlg: the sorted-vector barrier tracker (TLG_BARRIER_VECTOR, upstream PR #5112) must answer
// exactly like the red-black tree it replaces: Runtime::IsBufferAccessed decides from
// FindRange whether a draw or dispatch flushes barriers first, so equal answers for equal
// access sequences are equal barriers in equal order.
//
// Three implementations run the same pseudo-random sequences (fixed seed):
//  - Model: a plain list per buffer and access type, searched linearly (what both mean);
//  - BarrierRangeTracker (new): compared with the model on every sequence;
//  - BarrierTracker (tree): compared on the "strict" sequences only. A zero-sized access that
//    lands in free space becomes a tree node with range_end == range_start - 1, and the tree's
//    ordering (by range_start alone) cannot tell it from a range starting at the same offset:
//    it then misplaces or misses that range (also in its own overlap test for a range
//    covering the node), and from there its answers depend on the shape of the tree. Strict
//    sequences leave those combinations out (a zero-sized node and a range at one offset,
//    an insert across such a node); the others keep them and show the new tracker still
//    agrees with the model.
//
// `--bench` times InsertRange for both on a frame-like pattern instead.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include "video_core/renderer_vulkan/vk_barrier_ranges.h"

using Vulkan::Access;
using Vulkan::AddressRange;
using Vulkan::BarrierRangeTracker;
using Vulkan::BarrierTracker;

namespace {

struct Rng {
    u64 state;
    u64 Next() {
        // splitmix64
        u64 z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    u64 Below(u64 n) {
        return Next() % n;
    }
    bool Chance(u32 percent) {
        return Below(100) < percent;
    }
};

/// What the trackers are meant to do, written the slow way.
class Model {
public:
    bool FindRange(const AddressRange& range, Access access) const {
        const auto it = lists.find({range.resource, access == Access::Write});
        if (it == lists.end()) {
            return false;
        }
        return std::ranges::any_of(it->second, [&](const AddressRange& r) { return r.Overlaps(range); });
    }

    void InsertRange(const AddressRange& range, Access access) {
        auto& list = lists[{range.resource, access == Access::Write}];
        AddressRange merged = range;
        bool any = false;
        for (size_t i = 0; i < list.size();) {
            if (list[i].Overlaps(range)) {
                merged.range_start = std::min(merged.range_start, list[i].range_start);
                merged.range_end = std::max(merged.range_end, list[i].range_end);
                list.erase(list.begin() + i);
                any = true;
            } else {
                ++i;
            }
        }
        if (!any) {
            for (const AddressRange& r : list) {
                if (r.range_start == range.range_start && r.range_end == range.range_end) {
                    return;
                }
            }
        }
        list.push_back(merged);
    }

    void Clear() {
        lists.clear();
    }

    bool Empty() const {
        return lists.empty();
    }

    /// Whether a range held for the buffer starts at the offset.
    bool StartsAt(u64 resource, Access access, u64 offset) const {
        const auto it = lists.find({resource, access == Access::Write});
        return it != lists.end() && std::ranges::any_of(it->second, [&](const AddressRange& r) {
            return r.range_start == offset;
        });
    }

private:
    std::map<std::pair<u64, bool>, std::vector<AddressRange>> lists;
};

/// As Runtime builds it from (buffer, offset, size).
AddressRange MakeRange(u64 resource, u64 offset, u64 size) {
    return {resource, offset, offset + size - 1};
}

struct ZeroNode {
    u64 resource;
    bool write;
    u64 offset;
};

struct Totals {
    u64 sequences = 0, strict = 0, operations = 0, barriers = 0, zero_sized = 0, skipped = 0;
};

[[noreturn]] void Fail(const char* what, u64 sequence, u64 op, const AddressRange& range, Access access) {
    std::printf("FAIL: %s differs: sequence %llu, operation %llu, buffer %llx [%llx, %llx] %s\n", what,
                (unsigned long long)sequence, (unsigned long long)op,
                (unsigned long long)range.resource, (unsigned long long)range.range_start,
                (unsigned long long)range.range_end, access == Access::Write ? "write" : "read");
    std::exit(1);
}

void RunSequence(Rng& rng, u64 sequence, bool strict, BarrierTracker& tree,
                 BarrierRangeTracker& ranges, Model& model, Totals& totals) {
    // Shape of this sequence: how many buffers (many: the tree's 32 buckets collide and the
    // new table grows), how wide the offsets spread (narrow: overlaps, containment and
    // touching ranges all the time), 4 KiB steps like the rounded reads, or the top of the
    // address space.
    static constexpr u32 kBufferCounts[] = {1, 2, 3, 8, 40, 300};
    const u32 buffers = kBufferCounts[rng.Below(std::size(kBufferCounts))];
    const u64 span = u64{1} << (3 + rng.Below(8));
    const u64 unit = rng.Chance(25) ? 0x1000 : 1;
    const u64 base = rng.Chance(5) ? ~u64{0} - span * unit * 2 : rng.Chance(30) ? rng.Below(1 << 20) : 0;
    const u32 operations = 1 + static_cast<u32>(rng.Below(buffers > 8 ? 400 : 80));
    const u32 zero_percent = rng.Chance(50) ? 0 : 1 + static_cast<u32>(rng.Below(6));

    std::vector<ZeroNode> zero_nodes; // strict: zero-sized nodes the tree holds
    std::vector<u32> barriers[3];     // operations that would flush, per implementation

    const auto pick = [&](bool allow_zero) {
        // Buffers are 16-byte aligned objects, as the handles are in the renderer.
        const u64 resource = 0x7f0012340000ull + rng.Below(buffers) * 0x1d0;
        u64 size = rng.Chance(50) ? 1 + rng.Below(4) : 1 + rng.Below(span);
        if (allow_zero && rng.Chance(zero_percent)) {
            size = 0;
        }
        const u64 offset = base + rng.Below(span) * unit;
        return MakeRange(resource, offset, size * unit);
    };
    // Offset 0, size 0 wraps to the whole buffer: an ordinary range.
    const auto zero_sized = [](const AddressRange& r) {
        return r.range_start != 0 && r.range_end + 1 == r.range_start;
    };
    // Strict: stay clear of what the tree cannot order once it holds a zero-sized node.
    const auto tree_safe = [&](const AddressRange& r, bool write, bool insert) {
        for (const ZeroNode& node : zero_nodes) {
            if (node.resource != r.resource || node.write != write) {
                continue;
            }
            if (!zero_sized(r) && r.range_start == node.offset) {
                return false;
            }
            if (insert && (zero_sized(r) ? r.range_start == node.offset
                                         : r.range_start < node.offset && r.range_end >= node.offset)) {
                return false;
            }
        }
        return true;
    };
    const auto find = [&](u32 op, const AddressRange& r, Access access, bool record) {
        const bool expected = model.FindRange(r, access);
        if (ranges.FindRange(r, access) != expected) {
            Fail("FindRange (vector against model)", sequence, op, r, access);
        }
        if (strict && tree.FindRange(r, access) != expected) {
            Fail("FindRange (tree against vector)", sequence, op, r, access);
        }
        if (record && expected) {
            // Each implementation logs from its own answer; the lists are compared at the end.
            barriers[0].push_back(op);
            if (ranges.FindRange(r, access)) {
                barriers[1].push_back(op);
            }
            if (strict && tree.FindRange(r, access)) {
                barriers[2].push_back(op);
            }
        }
        return expected;
    };
    const auto clear = [&] {
        model.Clear();
        ranges.Clear();
        tree.Clear();
        zero_nodes.clear();
    };

    for (u32 op = 0; op < operations; ++op) {
        ++totals.operations;
        const u32 kind = static_cast<u32>(rng.Below(100));
        if (kind < 45) {
            // Runtime::AccessBuffer: written, read, or both.
            const AddressRange r = pick(true);
            const u32 mode = static_cast<u32>(rng.Below(10));
            for (const Access access : {Access::Write, Access::Read}) {
                if ((access == Access::Write) ? (mode >= 3 && mode != 9) : (mode < 3)) {
                    continue;
                }
                const bool write = access == Access::Write;
                if (strict && !tree_safe(r, write, true)) {
                    ++totals.skipped;
                    continue;
                }
                if (zero_sized(r)) {
                    ++totals.zero_sized;
                    if (strict && !model.FindRange(r, access)) {
                        if (model.StartsAt(r.resource, access, r.range_start)) {
                            ++totals.skipped;
                            continue;
                        }
                        zero_nodes.push_back({r.resource, write, r.range_start});
                    }
                }
                model.InsertRange(r, access);
                ranges.InsertRange(r, access);
                if (strict) {
                    tree.InsertRange(r, access);
                }
            }
        } else if (kind < 92) {
            // Runtime::IsBufferAccessed, and FlushBarriers when it says so.
            const AddressRange r = pick(true);
            const bool check_read = rng.Chance(40);
            if (strict && !(tree_safe(r, true, false) && tree_safe(r, false, false))) {
                ++totals.skipped;
                continue;
            }
            bool accessed = find(op, r, Access::Write, true);
            if (check_read && !accessed) {
                accessed = find(op, r, Access::Read, true);
            }
            if (accessed) {
                ++totals.barriers;
                // The renderer always flushes here; keeping the state half of the time
                // reaches the deeper trees and the merges.
                if (rng.Chance(50)) {
                    clear();
                }
            }
        } else if (kind < 98) {
            // A burst of ranges next to each other, then one over all of them: the merge.
            const AddressRange r = pick(false);
            const Access access = rng.Chance(50) ? Access::Write : Access::Read;
            const u64 step = (r.range_end - r.range_start + 1) * (1 + rng.Below(2));
            AddressRange piece = r;
            AddressRange all = r;
            const u32 count = 2 + static_cast<u32>(rng.Below(5));
            for (u32 i = 0; i <= count; ++i) {
                const AddressRange& next = i < count ? piece : all;
                if (next.range_end < next.range_start) {
                    break; // wrapped at the top of the address space
                }
                if (strict && !tree_safe(next, access == Access::Write, true)) {
                    ++totals.skipped;
                    break;
                }
                model.InsertRange(next, access);
                ranges.InsertRange(next, access);
                if (strict) {
                    tree.InsertRange(next, access);
                }
                all.range_end = piece.range_end;
                piece.range_start += step;
                piece.range_end += step;
            }
        } else {
            clear();
        }
        if (ranges.Empty() != model.Empty() || (strict && tree.Empty() != model.Empty())) {
            Fail("Empty", sequence, op, {}, Access::None);
        }
    }

    // What is left, seen from every offset in reach.
    const u32 probes = buffers > 8 ? 64 : 256;
    for (u32 i = 0; i < probes; ++i) {
        const AddressRange r = pick(true);
        for (const Access access : {Access::Write, Access::Read}) {
            if (strict && !tree_safe(r, access == Access::Write, false)) {
                continue;
            }
            find(operations + i, r, access, true);
        }
    }
    if (barriers[1] != barriers[0] || (strict && barriers[2] != barriers[0])) {
        Fail("the barrier list", sequence, operations, {}, Access::None);
    }
    clear();
    ++totals.sequences;
    totals.strict += strict;
}

int Test(u64 sequences) {
    assert(Vulkan::IsBarrierVectorEnabled(nullptr));
    assert(Vulkan::IsBarrierVectorEnabled("1"));
    assert(Vulkan::IsBarrierVectorEnabled(""));
    assert(Vulkan::IsBarrierVectorEnabled("00"));
    assert(!Vulkan::IsBarrierVectorEnabled("0"));

    // The trackers live through all sequences, like the one in Runtime: Clear() has to
    // leave nothing behind.
    BarrierTracker tree;
    BarrierRangeTracker ranges;
    Model model;
    Totals totals;
    Rng rng{0x715BA221E5ull};
    for (u64 i = 0; i < sequences; ++i) {
        RunSequence(rng, i, !rng.Chance(20), tree, ranges, model, totals);
    }
    std::printf("barrier tracker: %llu sequences (%llu with the tree), %llu operations, %llu "
                "barriers, %llu zero-sized accesses, %llu left out for the tree: all equal\n",
                (unsigned long long)totals.sequences, (unsigned long long)totals.strict,
                (unsigned long long)totals.operations, (unsigned long long)totals.barriers,
                (unsigned long long)totals.zero_sized, (unsigned long long)totals.skipped);
    return 0;
}

struct BenchAccess {
    AddressRange range;
    Access access;
};

template <class Tracker>
double BenchInsert(const std::vector<BenchAccess>& frame, u32 flush_every, u32 frames, u64& sink) {
    Tracker tracker;
    const auto run = [&](u32 count) {
        for (u32 f = 0; f < count; ++f) {
            u32 since = 0;
            for (const BenchAccess& a : frame) {
                tracker.InsertRange(a.range, a.access);
                if (++since == flush_every) {
                    tracker.Clear();
                    since = 0;
                }
            }
            sink += tracker.Empty();
            tracker.Clear();
        }
    };
    run(frames / 10); // storage and caches
    const auto start = std::chrono::steady_clock::now();
    run(frames);
    const std::chrono::duration<double, std::nano> elapsed = std::chrono::steady_clock::now() - start;
    return elapsed.count() / (double(frames) * frame.size());
}

template <class Tracker>
double BenchFind(const std::vector<BenchAccess>& frame, u32 frames, u64& sink) {
    Tracker tracker;
    for (const BenchAccess& a : frame) {
        tracker.InsertRange(a.range, a.access);
    }
    const auto start = std::chrono::steady_clock::now();
    for (u32 f = 0; f < frames; ++f) {
        for (const BenchAccess& a : frame) {
            // The other access type: mostly misses, as before most draws.
            sink += tracker.FindRange(a.range, a.access == Access::Write ? Access::Read : Access::Write);
        }
    }
    const std::chrono::duration<double, std::nano> elapsed = std::chrono::steady_clock::now() - start;
    return elapsed.count() / (double(frames) * frame.size());
}

int Bench() {
    // One frame as the tracker sees it behind Runtime's memo: about 600 distinct ranges on
    // 40 buffers, most of them on a few (the stream buffer, the big guest buffers). Reads
    // are rounded to 4 KiB as AccessBuffer does; writes keep their bytes.
    Rng rng{0xBE7C4ull};
    std::vector<BenchAccess> frame;
    for (u32 i = 0; i < 600; ++i) {
        const u32 hot = static_cast<u32>(rng.Below(100));
        const u64 buffer = hot < 60 ? rng.Below(4) : rng.Below(40);
        const u64 resource = 0x7f0012340000ull + buffer * 0x1d0;
        const bool write = rng.Chance(25);
        const u64 offset = rng.Below(16 << 20);
        const u64 size = 16 + rng.Below(write ? 4096 : 65536);
        AddressRange range = MakeRange(resource, offset, size);
        if (!write) {
            range.range_start &= ~u64{0xFFF};
            range.range_end |= 0xFFF;
        }
        frame.push_back({range, write ? Access::Write : Access::Read});
    }
    static constexpr u32 kFrames = 4000;
    u64 sink = 0;
    std::printf("InsertRange, %zu ranges a frame on 40 buffers, %u frames, ns per insert:\n",
                frame.size(), kFrames);
    std::printf("  %-30s %8s %8s\n", "barrier flush", "tree", "vector");
    for (const u32 flush_every : {600u, 100u, 20u}) {
        // Best of three: other work on the machine only ever adds time.
        double tree = 1e9, vector = 1e9;
        for (u32 round = 0; round < 3; ++round) {
            tree = std::min(tree, BenchInsert<BarrierTracker>(frame, flush_every, kFrames, sink));
            vector = std::min(vector, BenchInsert<BarrierRangeTracker>(frame, flush_every, kFrames, sink));
        }
        char label[64];
        std::snprintf(label, sizeof(label), flush_every == 600 ? "once a frame" : "every %u inserts",
                      flush_every);
        std::printf("  %-30s %8.1f %8.1f\n", label, tree, vector);
    }
    double tree = 1e9, vector = 1e9;
    for (u32 round = 0; round < 3; ++round) {
        tree = std::min(tree, BenchFind<BarrierTracker>(frame, kFrames, sink));
        vector = std::min(vector, BenchFind<BarrierRangeTracker>(frame, kFrames, sink));
    }
    std::printf("FindRange with a frame's ranges held, ns per call: tree %.1f, vector %.1f (%llu)\n",
                tree, vector, (unsigned long long)sink);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--bench")) {
        return Bench();
    }
    // Optional: the number of sequences.
    return Test(argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 300'000);
}
