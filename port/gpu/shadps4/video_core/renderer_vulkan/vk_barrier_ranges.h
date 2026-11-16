// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_barrier_tracker.h"

namespace Vulkan {

/**
 * tlg: the sorted-vector interval sets of upstream shadPS4 PR #5112 ("Renderer optimizations
 * pt1"), which replaced the red-black tree of BarrierTracker with them: one pair of lists
 * (read, written) per buffer, each sorted by address and binary searched.
 *
 * It is not upstream's common/interval_set.h, because this one has to answer exactly like the
 * tree did (the two are compared in tests/test_barrier_tracker.cpp):
 *  - ranges are inclusive [range_start, range_end], as the callers build them;
 *  - a zero-sized access arrives as range_end == range_start - 1. The tree keeps it as a
 *    node that overlaps whatever covers both bytes around it (and size 0 at offset 0 wraps
 *    to the whole buffer); upstream's lists drop it;
 *  - ranges that only touch stay separate (invisible to FindRange, but it keeps the
 *    statistics comparable).
 * The lists keep their storage across Clear(): a barrier flush happens many times a frame.
 */
class BarrierRangeTracker {
public:
    /// Checks whether there is a pending access of a given type
    bool FindRange(const AddressRange& range, Access access_type) const {
        const u32 index = table[Probe(range.resource)];
        if (!index) {
            return false;
        }
        const List& list = resources[index - 1].lists[access_type == Access::Write];
        const auto it = FirstEndingAt(list, range.range_start);
        return it != list.end() && it->start <= range.range_end;
    }

    /// Inserts address range for a given access type
    void InsertRange(const AddressRange& range, Access access_type) {
        List& list = Acquire(range.resource).lists[access_type == Access::Write];
        const auto first = FirstEndingAt(list, range.range_start);
        if (first == list.end() || first->start > range.range_end) {
            // tlg: nothing overlaps. A zero-sized range does not overlap its own copy
            // either (the tree stores each one again), but one is enough here.
            if (first != list.begin() && std::prev(first)->start == range.range_start &&
                std::prev(first)->end == range.range_end) [[unlikely]] {
                ++stat_contained;
                return;
            }
            list.insert(first, Range{range.range_start, range.range_end});
            ++stat_new;
            return;
        }
        if (first->start <= range.range_start && first->end >= range.range_end) {
            ++stat_contained;
            return;
        }
        ++stat_other;

        // The ranges are disjoint and sorted, so everything overlapping the new one is a run
        // starting here. Replace the run with one range covering it and the new one.
        auto last = std::next(first);
        while (last != list.end() && last->start <= range.range_end) {
            ++last;
        }
        first->start = std::min(first->start, range.range_start);
        first->end = std::max(std::prev(last)->end, range.range_end);
        list.erase(std::next(first), last);
    }

    /// Clears the entire structure
    void Clear() {
        for (u32 i = 0; i < used; ++i) {
            Resource& resource = resources[i];
            resource.lists[0].clear();
            resource.lists[1].clear();
            table[resource.slot] = 0;
        }
        used = 0;
    }

    /// Checks whether any resources are dirty
    bool Empty() const {
        return used == 0;
    }

    /// tlg (TLG_DRAW_RUNS): ranges held now, and buffers they belong to.
    size_t Nodes() const {
        size_t count = 0;
        for (u32 i = 0; i < used; ++i) {
            count += resources[i].lists[0].size() + resources[i].lists[1].size();
        }
        return count;
    }
    size_t Resources() const {
        return used;
    }
    u64 stat_new = 0, stat_contained = 0, stat_other = 0; ///< tlg: InsertRange outcomes

private:
    struct Range {
        u64 start;
        u64 end; ///< Inclusive; start - 1 for a zero-sized access.
    };
    using List = std::vector<Range>;

    struct Resource {
        u64 handle{};
        u32 slot{}; ///< Position in table, to clear it without walking the table.
        List lists[2]; ///< Read, written.
    };

    /// First range ending at or after the address: the only candidate to overlap a range
    /// starting there, since the ends ascend along with the starts.
    template <class L>
    static auto FirstEndingAt(L& list, u64 address) -> decltype(list.begin()) {
        return std::partition_point(list.begin(), list.end(),
                                    [address](const Range& r) { return r.end < address; });
    }

    /// Slot of the resource in the open-addressed table, or the empty slot where it belongs.
    u32 Probe(u64 handle) const {
        const u32 mask = static_cast<u32>(table.size()) - 1;
        u64 hash = handle * 0x9E3779B97F4A7C15ull;
        u32 slot = static_cast<u32>(hash >> 40) & mask;
        while (table[slot] && resources[table[slot] - 1].handle != handle) {
            slot = (slot + 1) & mask;
        }
        return slot;
    }

    Resource& Acquire(u64 handle) {
        u32 slot = Probe(handle);
        if (table[slot]) {
            return resources[table[slot] - 1];
        }
        if ((used + 1) * 2 > table.size()) [[unlikely]] {
            // Keep the table at most half full, so that probes stay short and always end.
            table.assign(table.size() * 2, 0);
            for (u32 i = 0; i < used; ++i) {
                resources[i].slot = Probe(resources[i].handle);
                table[resources[i].slot] = i + 1;
            }
            slot = Probe(handle);
        }
        if (used == resources.size()) {
            resources.emplace_back();
        }
        Resource& resource = resources[used++];
        resource.handle = handle;
        resource.slot = slot;
        table[slot] = used;
        return resource;
    }

    std::vector<Resource> resources; ///< The first `used` are live; the rest keep their storage.
    u32 used = 0;
    std::vector<u32> table = std::vector<u32>(128); ///< Index into resources plus one, 0 if empty.
};

/// tlg: TLG_BARRIER_VECTOR=0 keeps the red-black tree, to measure one against the other in
/// the same build. Anything else, or unset: the sorted vectors.
inline bool IsBarrierVectorEnabled(const char* env) {
    return !(env && env[0] == '0' && env[1] == '\0');
}

/**
 * tlg: what Runtime holds: either container behind the interface of BarrierTracker, chosen
 * once per process.
 */
class SwitchedBarrierTracker {
public:
    struct Stats {
        size_t nodes, free_nodes;
        u64 inserted, contained, widened;
    };

    bool FindRange(const AddressRange& range, Access access_type) const {
        return use_vector ? ranges.FindRange(range, access_type)
                           : tree.FindRange(range, access_type);
    }

    void InsertRange(const AddressRange& range, Access access_type) {
        if (use_vector) {
            ranges.InsertRange(range, access_type);
        } else {
            tree.InsertRange(range, access_type);
        }
    }

    void Clear() {
        if (use_vector) {
            ranges.Clear();
        } else {
            tree.Clear();
        }
    }

    bool Empty() const {
        return use_vector ? ranges.Empty() : tree.Empty();
    }

    /// tlg (TLG_DRAW_RUNS): sizes now and the insert outcomes since the last call.
    Stats TakeStats() {
        if (use_vector) {
            return {ranges.Nodes(), 0, std::exchange(ranges.stat_new, 0),
                    std::exchange(ranges.stat_contained, 0), std::exchange(ranges.stat_other, 0)};
        }
        return {tree.Nodes(), tree.FreeNodes(), std::exchange(tree.stat_new, 0),
                std::exchange(tree.stat_contained, 0), std::exchange(tree.stat_other, 0)};
    }

    static bool UseVector() {
        static const bool on = [] {
            const bool value = IsBarrierVectorEnabled(std::getenv("TLG_BARRIER_VECTOR"));
            if (!value) {
                std::printf("GPU: barrier tracker on the red-black tree (TLG_BARRIER_VECTOR=0)\n");
            }
            return value;
        }();
        return on;
    }

private:
    const bool use_vector = UseVector();
    BarrierTracker tree;
    BarrierRangeTracker ranges;
};

} // namespace Vulkan
