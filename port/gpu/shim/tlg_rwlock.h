// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: a reader-writer lock whose readers share one atomic word. libc++'s std::shared_mutex takes an
// internal mutex in lock_shared/unlock_shared, so the draw preparation workers (all readers of
// PipelineCache::programs_mutex) queued on it: ~25% of their time in __psynch_mutexwait (Seattle,
// 2026-11-12). Writers (new programs and permutations) are rare; a pending writer stops new readers.
#pragma once
#include <atomic>
#include <cstdint>
#include <thread>

class TlgRwLock {
public:
    void lock_shared() {
        for (unsigned spins = 0;; ++spins) {
            std::uint32_t s = state.load(std::memory_order_relaxed);
            if (!(s & (Writer | Pending)) &&
                state.compare_exchange_weak(s, s + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                return;
            }
            Pause(spins);
        }
    }
    bool try_lock_shared() {
        std::uint32_t s = state.load(std::memory_order_relaxed);
        return !(s & (Writer | Pending)) &&
               state.compare_exchange_strong(s, s + 1, std::memory_order_acquire, std::memory_order_relaxed);
    }
    void unlock_shared() {
        state.fetch_sub(1, std::memory_order_release);
    }
    void lock() {
        for (unsigned spins = 0;; ++spins) {
            std::uint32_t s = state.load(std::memory_order_relaxed);
            if ((s == 0 || s == Pending) &&
                state.compare_exchange_weak(s, Writer, std::memory_order_acquire, std::memory_order_relaxed)) {
                return;
            }
            if (!(s & Pending)) {
                state.fetch_or(Pending, std::memory_order_relaxed);
            }
            Pause(spins);
        }
    }
    bool try_lock() {
        std::uint32_t s = 0;
        return state.compare_exchange_strong(s, Writer, std::memory_order_acquire, std::memory_order_relaxed);
    }
    void unlock() {
        state.fetch_and(~Writer, std::memory_order_release);
    }

private:
    static constexpr std::uint32_t Writer = 1u << 31, Pending = 1u << 30;
    static void Pause(unsigned spins) {
        if (spins < 64) {
#if defined(__aarch64__)
            asm volatile("yield");
#elif defined(__x86_64__)
            __builtin_ia32_pause();
#endif
        } else {
            std::this_thread::yield();
        }
    }
    std::atomic<std::uint32_t> state{0};
};
