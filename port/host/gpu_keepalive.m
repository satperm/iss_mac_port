// SPDX-FileCopyrightText: Copyright 2026 satperm
// SPDX-License-Identifier: GPL-2.0-or-later
// Keeps the GPU from idling while the game runs.
//
// macOS's GPU performance controller clocks an Apple GPU down when it is not fully busy: after
// ~30-40 s at ~94% load (a scene limited by the CPU side leaves the GPU short gaps) it went from
// performance state 13 to 1 of 15 within seconds and STAYED there at 98% load: Seattle 72 -> 33
// FPS, the truck crash 52 -> 29.5 with the recording thread slowed on purpose (2026-11-13, M3 Max,
// tools/gpu_probes/gpu_pstate.c; not thermal, not power, Game Mode on). Scenes that keep the GPU at
// 99-100% never drop. One tiny compute dispatch at a time on a Metal queue of its own, the next
// committed when the last completes, fills the gaps: the GPU reads 100% busy and stays at state
// 13 (Seattle 90 s: 72 FPS throughout; the same FPS before the drop with and without it).
//
// Only when needed: where the game keeps the GPU full by itself, the filler cost FPS (truck crash
// 74.5 -> 72.9, bridge 97.6 -> 97.1). A thread reads the GPU's performance-state residency twice a
// second (IOReport "GPU Stats", as tools/gpu_probes/gpu_pstate.c; no root) and fills only while
// the GPU is in use but not full (50..98% active for 2 s); every 10 s it stops for a second to
// see what the game does alone (the controller needs ~30 s of idling to clock down).
//
// TLG_GPU_KEEPALIVE=0: off; =2: always fill (no measuring). TLG_GPU_KEEPALIVE_STEPS=<n>: loop
// steps per thread (default 20000: ~0.4 ms of GPU time per dispatch when nothing else runs,
// ~2,000 dispatches a second, ~9% of one CPU core for the completions).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct IOReportSubscription *IOReportSubscriptionRef;
extern CFDictionaryRef IOReportCopyChannelsInGroup(CFStringRef group, CFStringRef subgroup, uint64_t a, uint64_t b, uint64_t c);
extern IOReportSubscriptionRef IOReportCreateSubscription(void *a, CFMutableDictionaryRef channels, CFMutableDictionaryRef *out, uint64_t d, CFTypeRef e);
extern CFDictionaryRef IOReportCreateSamples(IOReportSubscriptionRef sub, CFMutableDictionaryRef channels, CFTypeRef a);
extern CFDictionaryRef IOReportCreateSamplesDelta(CFDictionaryRef a, CFDictionaryRef b, CFTypeRef c);
extern CFStringRef IOReportChannelGetChannelName(CFDictionaryRef channel);
extern int IOReportStateGetCount(CFDictionaryRef channel);
extern int64_t IOReportStateGetResidency(CFDictionaryRef channel, int index);

static atomic_bool keepalive_filling;
static atomic_bool keepalive_in_flight;

static id<MTLCommandQueue> keepalive_queue;
static id<MTLComputePipelineState> keepalive_pipeline;
static id<MTLBuffer> keepalive_out, keepalive_steps;

static void keepalive_submit(void) {
    @autoreleasepool {
        id<MTLCommandBuffer> buffer = [keepalive_queue commandBufferWithUnretainedReferences];
        id<MTLComputeCommandEncoder> encoder = [buffer computeCommandEncoder];
        if (!buffer || !encoder) return;
        buffer.label = @"tlg keep-alive";
        [encoder setComputePipelineState:keepalive_pipeline];
        [encoder setBuffer:keepalive_out offset:0 atIndex:0];
        [encoder setBuffer:keepalive_steps offset:0 atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];
        [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
            (void)done;
            if (atomic_load(&keepalive_filling)) {
                keepalive_submit();
            } else {
                atomic_store(&keepalive_in_flight, false);
            }
        }];
        [buffer commit];
    }
}

static void keepalive_set(bool fill) {
    atomic_store(&keepalive_filling, fill);
    if (fill && !atomic_exchange(&keepalive_in_flight, true)) keepalive_submit();
}

/* The GPU's share of time in any powered state over the interval since the last call, in percent
 * (-1: unknown), and the mean powered state. */
static double keepalive_active(IOReportSubscriptionRef sub, CFMutableDictionaryRef subscribed, CFDictionaryRef *before,
                               double *mean_state) {
    CFDictionaryRef now = IOReportCreateSamples(sub, subscribed, NULL);
    if (!now) return -1;
    double result = -1;
    CFDictionaryRef delta = *before ? IOReportCreateSamplesDelta(*before, now, NULL) : NULL;
    CFArrayRef list = delta ? CFDictionaryGetValue(delta, CFSTR("IOReportChannels")) : NULL;
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); ++i) {
        CFDictionaryRef ch = CFArrayGetValueAtIndex(list, i);
        char name[64] = "";
        CFStringRef n = IOReportChannelGetChannelName(ch);
        if (n) CFStringGetCString(n, name, sizeof name, kCFStringEncodingUTF8);
        if (strcmp(name, "GPUPH") != 0) continue;
        const int count = IOReportStateGetCount(ch);
        int64_t total = 0, active = 0;
        double weighted = 0;
        for (int k = 0; k < count; ++k) {
            const int64_t r = IOReportStateGetResidency(ch, k);
            total += r;
            if (k > 0) { active += r; weighted += (double)r * k; }
        }
        if (total > 0) result = 100.0 * (double)active / (double)total;
        *mean_state = active ? weighted / (double)active : 0;
    }
    if (delta) CFRelease(delta);
    if (*before) CFRelease(*before);
    *before = now;
    return result;
}

static void *keepalive_thread(void *arg) {
    (void)arg;
    pthread_setname_np("tlg:GpuKeepAlive");
    CFDictionaryRef found = IOReportCopyChannelsInGroup(CFSTR("GPU Stats"), CFSTR("GPU Performance States"), 0, 0, 0);
    CFMutableDictionaryRef channels = found ? CFDictionaryCreateMutableCopy(NULL, 0, found) : NULL, subscribed = NULL;
    IOReportSubscriptionRef sub = channels ? IOReportCreateSubscription(NULL, channels, &subscribed, 0, NULL) : NULL;
    if (!sub) {
        printf("tlggpu-host: GPU keep-alive off (no GPU performance-state report)\n");
        return NULL;
    }
    const char *log_env = getenv("TLG_GPU_KEEPALIVE_LOG");
    const bool log = log_env && *log_env == '1';
    CFDictionaryRef before = NULL;
    double state = 0;
    keepalive_active(sub, subscribed, &before, &state);
    unsigned short_of_full = 0, filled_ticks = 0, probe_ticks = 0;
    bool filling = false;
    unsigned long long fills = 0, ticks = 0;
    for (;;) {
        usleep(500 * 1000);
        const double active = keepalive_active(sub, subscribed, &before, &state);
        ++ticks;
        if (filling) {
            /* Every 10 s: stop for two samples and look at the game alone. */
            if (++filled_ticks >= 20) {
                filling = false;
                keepalive_set(false);
                probe_ticks = 2;
                short_of_full = 0;
            }
        } else if (probe_ticks) {
            /* The first sample still holds some filler work; the second decides. */
            if (--probe_ticks == 0 && active >= 50 && active < 98) {
                filling = true;
            }
        } else {
            short_of_full = active >= 50 && active < 98 ? short_of_full + 1 : 0;
            if (short_of_full >= 4) filling = true;
        }
        if (filling && !atomic_load(&keepalive_filling)) {
            filled_ticks = 0;
            ++fills;
            keepalive_set(true);
        }
        if (log && ticks % 10 == 0) {
            printf("tlggpu-host: GPU keep-alive: GPU %.0f%% active at state %.1f, filler %s (%llu starts)\n", active, state,
                   filling ? "on" : "off", fills);
        }
    }
    return NULL;
}

void tlg_gpu_keepalive_start(void) {
    const char *on = getenv("TLG_GPU_KEEPALIVE");
    if (on && *on == '0') return;
    const bool always = on && *on == '2';
    const char *env = getenv("TLG_GPU_KEEPALIVE_STEPS");
    uint32_t steps = env && *env ? (uint32_t)strtoul(env, NULL, 10) : 20000u;
    if (!steps) return;
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return;
        NSString *source = @"#include <metal_stdlib>\n"
                            "using namespace metal;\n"
                            "kernel void fill(device uint* out [[buffer(0)]], constant uint& steps [[buffer(1)]],\n"
                            "                 uint id [[thread_position_in_grid]]) {\n"
                            "    uint x = id + 1;\n"
                            "    for (uint i = 0; i < steps; ++i) { x = x * 1664525u + 1013904223u; }\n"
                            "    out[id] = x;\n"
                            "}\n";
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        id<MTLFunction> function = [library newFunctionWithName:@"fill"];
        if (!function) {
            printf("tlggpu-host: GPU keep-alive off (no shader)\n");
            return;
        }
        keepalive_pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        keepalive_queue = [device newCommandQueue];
        keepalive_out = [device newBufferWithLength:64 * 4 options:MTLResourceStorageModePrivate];
        keepalive_steps = [device newBufferWithBytes:&steps length:4 options:MTLResourceStorageModeShared];
        if (!keepalive_pipeline || !keepalive_queue || !keepalive_out || !keepalive_steps) {
            printf("tlggpu-host: GPU keep-alive off (setup failed)\n");
            return;
        }
        keepalive_queue.label = @"tlg keep-alive";
        printf("tlggpu-host: GPU keep-alive %s (%u steps; TLG_GPU_KEEPALIVE=0: off)\n",
               always ? "always on" : "when the GPU is short of full", steps);
        if (always) {
            keepalive_set(true);
        } else {
            pthread_t thread;
            if (pthread_create(&thread, NULL, keepalive_thread, NULL) == 0) pthread_detach(thread);
        }
    }
}
