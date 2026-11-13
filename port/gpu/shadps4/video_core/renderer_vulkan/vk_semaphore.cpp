// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <thread>
#include <limits>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#include "common/assert.h"

namespace Vulkan {

constexpr u64 WAIT_TIMEOUT = std::numeric_limits<u64>::max();

Semaphore::Semaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
}

Semaphore::~Semaphore() = default;

void Semaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

#ifdef __APPLE__
    // tlg (macOS): polls the counter instead of vkWaitSemaphores (TLG_SEMAPHORE_POLL=0: waits).
    // Every vkWaitSemaphores call makes MoltenVK create an MTLSharedEventListener and register
    // a notification on the queue's MTLSharedEvent; they pile up inside Metal and later
    // submissions get slower (see CommandPool::ResetThread, vk_resource_pool.cpp). A wait here
    // blocks a thread that needs the GPU's answer: spin briefly, then sleep 20 us at a time.
    static const bool poll = [] {
        const char* env = std::getenv("TLG_SEMAPHORE_POLL");
        return !(env && *env == '0');
    }();
    if (poll) {
        for (u32 spins = 0;; ++spins) {
            Refresh();
            if (IsFree(tick)) {
                return;
            }
            if (spins < 64) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
        }
    }
#endif
    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    while (instance.GetDevice().waitSemaphores(&wait_info, WAIT_TIMEOUT) != vk::Result::eSuccess) {
    }
    Refresh();
}

} // namespace Vulkan
