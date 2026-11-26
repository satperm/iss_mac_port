// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the Perf line, see tlg_perf_line.h. Keys (any may be missing):
//   fps ft_p50 ft_p99 ft_p999 ft_max held       frame times (ms) from flip to flip, held frames
//   skip_shader skip_pipe                       draws skipped: a shader stage / the pipeline still compiling
//   gpu_busy                                     % of the window the GPU ran command buffers (MoltenVK)
//   cpu_game cpu_gpu cpu.<thread>                % of one core: both processes, then the busiest threads
//   draws passes dispatches (per frame) new_pipes (in the window)
//   faults protects (per s) copy_mb (MB/s uploaded to the GPU) unbounded unbounded_mb (size-less V#)
//   fp_probe_mb fp_host_mb swap_mb pressure (0 normal, 1 warn, 2 critical) therm (0-3)
#include "tlg_perf_line.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tlg_toggles.h"

#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <libproc.h>
#include <notify.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

namespace TlgPerfLine {
namespace {

using Clock = std::chrono::steady_clock;

double Percentile(std::vector<float>& v, double q) {
    if (v.empty()) {
        return 0.0;
    }
    const size_t i = std::min(v.size() - 1, size_t(q * double(v.size())));
    std::nth_element(v.begin(), v.begin() + i, v.end());
    return v[i];
}

#ifdef __APPLE__
/// CPU time per thread of one process, by thread id; the window's share is the difference.
struct ProcessThreads {
    pid_t pid = 0;
    const char* unnamed = "unnamed";
    std::unordered_map<uint64_t, uint64_t> last_ns;

    /// Adds (name, ns this window) for each thread to out; returns the process total for the window.
    uint64_t Sample(std::vector<std::pair<std::string, uint64_t>>& out) {
        if (pid <= 0) {
            return 0;
        }
        std::vector<uint64_t> ids(512);
        const int bytes = proc_pidinfo(pid, PROC_PIDLISTTHREADS, 0, ids.data(),
                                       int(ids.size() * sizeof(uint64_t)));
        if (bytes <= 0) {
            return 0;
        }
        ids.resize(size_t(bytes) / sizeof(uint64_t));
        std::unordered_map<uint64_t, uint64_t> now_ns;
        uint64_t total = 0;
        for (const uint64_t id : ids) {
            proc_threadinfo info{};
            if (proc_pidinfo(pid, PROC_PIDTHREADINFO, id, &info, sizeof(info)) != sizeof(info)) {
                continue;
            }
            const uint64_t ns = info.pth_user_time + info.pth_system_time;
            now_ns[id] = ns;
            const auto it = last_ns.find(id);
            const uint64_t delta = it == last_ns.end() ? 0 : (ns > it->second ? ns - it->second : 0);
            if (delta == 0) {
                continue;
            }
            total += delta;
            std::string name = info.pth_name;
            for (const char* prefix : {"tlg:", "shadPS4:"}) {
                if (name.rfind(prefix, 0) == 0) {
                    name.erase(0, std::strlen(prefix));
                }
            }
            for (char& c : name) {
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) {
                    c = '_';
                }
            }
            out.emplace_back(name.empty() ? std::string(unnamed) : name, delta);
        }
        last_ns = std::move(now_ns);
        return total;
    }
};

uint64_t FootprintMb(pid_t pid) {
    rusage_info_v4 ri{};
    if (pid <= 0 || proc_pid_rusage(pid, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri)) != 0) {
        return 0;
    }
    return ri.ri_phys_footprint >> 20;
}

int ThermalLevel() {
    static int token = -1;
    static bool ok = notify_register_check("com.apple.system.thermalpressurelevel", &token) ==
                     NOTIFY_STATUS_OK;
    uint64_t state = 0;
    if (!ok || notify_get_state(token, &state) != NOTIFY_STATUS_OK) {
        return -1;
    }
    return int(std::min<uint64_t>(state, 3)); // nominal, moderate, heavy, trapping and above
}
#endif

} // namespace

void OnFlip() {
    static const bool enabled = [] {
        const char* env = std::getenv("TLG_PERF_LINE");
        return !env || *env != '0';
    }();
    if (!enabled) {
        return;
    }
    static Clock::time_point window_start = Clock::now(), last_flip = window_start;
    static std::vector<float> frame_ms;
    static uint64_t last_held, last_draws, last_dispatches, last_passes, last_pipes, last_faults,
        last_protects, last_upload, last_gpu_ns, last_unb, last_unb_mb, last_sk_sh, last_sk_pl;
    static bool primed = false;
    const Clock::time_point now = Clock::now();
    frame_ms.push_back(std::chrono::duration<float, std::milli>(now - last_flip).count());
    last_flip = now;
    const double window = std::chrono::duration<double>(now - window_start).count();
    if (window < 5.0) {
        return;
    }
    window_start = now;

    const uint64_t held = TlgStats::held_frames.load(std::memory_order_relaxed);
    const uint64_t draws = TlgStats::draws.load(std::memory_order_relaxed);
    const uint64_t dispatches = TlgStats::dispatches.load(std::memory_order_relaxed);
    const uint64_t passes = TlgStats::render_passes.load(std::memory_order_relaxed);
    const uint64_t pipes = TlgStats::pipelines_built.load(std::memory_order_relaxed);
    const uint64_t faults = TlgStats::perf_faults.load(std::memory_order_relaxed);
    const uint64_t protects = TlgStats::protect_calls.load(std::memory_order_relaxed);
    const uint64_t upload = TlgStats::image_upload_bytes.load(std::memory_order_relaxed) +
                            TlgStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    const uint64_t sk_sh = TlgStats::skipped_shader.load(std::memory_order_relaxed);
    const uint64_t sk_pl = TlgStats::skipped_pipeline.load(std::memory_order_relaxed);
    const uint64_t unb = TlgStats::unbounded_binds.load(std::memory_order_relaxed);
    const uint64_t unb_mb = TlgStats::unbounded_bind_mb.load(std::memory_order_relaxed);
    uint64_t gpu_ns = 0;
    bool have_gpu = false;
#ifdef __APPLE__
    // MoltenVK comes in through the Vulkan loader (RTLD_LOCAL): look its image up by name, once
    // it is loaded; the stock MoltenVK has no such function and the key stays out.
    using BusyFn = uint64_t (*)();
    static BusyFn gpu_busy_fn = nullptr;
    static int lookups = 0;
    if (!gpu_busy_fn && lookups < 12) {
        ++lookups;
        for (uint32_t i = 0, count = _dyld_image_count(); i < count; ++i) {
            const char* name = _dyld_get_image_name(i);
            if (name && std::strstr(name, "libMoltenVK")) {
                if (void* handle = dlopen(name, RTLD_LAZY | RTLD_NOLOAD)) {
                    gpu_busy_fn = reinterpret_cast<BusyFn>(dlsym(handle, "mvkTlgGpuBusyNs"));
                }
                break;
            }
        }
    }
    if (gpu_busy_fn) {
        gpu_ns = gpu_busy_fn();
        have_gpu = true;
    }
    // The game process is this process's parent (tlggpu-host is spawned by tlg-probe).
    static ProcessThreads game{getppid(), "game_unnamed", {}}, gpu{getpid(), "gpu_unnamed", {}};
    std::vector<std::pair<std::string, uint64_t>> threads;
    const uint64_t game_ns = game.Sample(threads);
    const uint64_t gpu_cpu_ns = gpu.Sample(threads);
#endif
    if (!primed) { // the first window only sets the baselines
        primed = true;
        last_held = held, last_draws = draws, last_dispatches = dispatches, last_passes = passes;
        last_pipes = pipes, last_faults = faults, last_protects = protects, last_upload = upload;
        last_gpu_ns = gpu_ns, last_unb = unb, last_unb_mb = unb_mb, last_sk_sh = sk_sh, last_sk_pl = sk_pl;
        frame_ms.clear();
        return;
    }

    const double frames = double(frame_ms.size());
    char line[2048];
    int n = std::snprintf(line, sizeof(line), "Perf: fps=%.1f", frames / window);
    auto add = [&](const char* fmt, auto... args) {
        if (n > 0 && size_t(n) < sizeof(line)) {
            n += std::snprintf(line + n, sizeof(line) - size_t(n), fmt, args...);
        }
    };
    add(" ft_p50=%.1f ft_p99=%.1f ft_p999=%.1f", Percentile(frame_ms, 0.50), Percentile(frame_ms, 0.99),
        Percentile(frame_ms, 0.999));
    add(" ft_max=%.1f held=%llu", double(*std::max_element(frame_ms.begin(), frame_ms.end())),
        static_cast<unsigned long long>(held - last_held));
    // Draws skipped because a shader stage or the pipeline was still compiling (the held frames' cause).
    add(" skip_shader=%llu skip_pipe=%llu", static_cast<unsigned long long>(sk_sh - last_sk_sh),
        static_cast<unsigned long long>(sk_pl - last_sk_pl));
    if (have_gpu) {
        add(" gpu_busy=%.0f", std::min(100.0, double(gpu_ns - last_gpu_ns) / (window * 1e7)));
    }
#ifdef __APPLE__
    add(" cpu_game=%.0f cpu_gpu=%.0f", double(game_ns) / (window * 1e7),
        double(gpu_cpu_ns) / (window * 1e7));
    // Threads with the same name (job workers) add up; the ten busiest at 2% or more.
    std::unordered_map<std::string, uint64_t> by_name;
    for (const auto& [name, ns] : threads) {
        by_name[name] += ns;
    }
    std::vector<std::pair<std::string, uint64_t>> sorted(by_name.begin(), by_name.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < sorted.size() && i < 10; ++i) {
        const double pct = double(sorted[i].second) / (window * 1e7);
        if (pct < 2.0) {
            break;
        }
        add(" cpu.%s=%.0f", sorted[i].first.c_str(), pct);
    }
#endif
    add(" draws=%.0f passes=%.0f dispatches=%.0f new_pipes=%llu", double(draws - last_draws) / frames,
        double(passes - last_passes) / frames, double(dispatches - last_dispatches) / frames,
        static_cast<unsigned long long>(pipes - last_pipes));
    add(" faults=%.0f protects=%.0f copy_mb=%.1f", double(faults - last_faults) / window,
        double(protects - last_protects) / window, double(upload - last_upload) / 1e6 / window);
    // Read-only buffers bound without a size (shadPS4 #5770): count per frame and average MB.
    if (unb > last_unb) {
        add(" unbounded=%.1f unbounded_mb=%.0f", double(unb - last_unb) / frames,
            double(unb_mb - last_unb_mb) / double(unb - last_unb));
    }
#ifdef __APPLE__
    add(" fp_probe_mb=%llu fp_host_mb=%llu", static_cast<unsigned long long>(FootprintMb(getppid())),
        static_cast<unsigned long long>(FootprintMb(getpid())));
    xsw_usage swap{};
    size_t size = sizeof(swap);
    if (sysctlbyname("vm.swapusage", &swap, &size, nullptr, 0) == 0) {
        add(" swap_mb=%llu", static_cast<unsigned long long>(swap.xsu_used >> 20));
    }
    int level = 0;
    size = sizeof(level);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &size, nullptr, 0) == 0) {
        add(" pressure=%d", level >= 4 ? 2 : level == 2 ? 1 : 0);
    }
    if (const int therm = ThermalLevel(); therm >= 0) {
        add(" therm=%d", therm);
    }
#endif
    std::printf("%s\n", line);
    std::fflush(stdout);

    last_held = held, last_draws = draws, last_dispatches = dispatches, last_passes = passes;
    last_pipes = pipes, last_faults = faults, last_protects = protects, last_upload = upload;
    last_gpu_ns = gpu_ns, last_unb = unb, last_unb_mb = unb_mb, last_sk_sh = sk_sh, last_sk_pl = sk_pl;
    frame_ms.clear();
}

} // namespace TlgPerfLine
