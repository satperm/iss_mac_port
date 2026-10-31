// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_perf_stats.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include "video_core/renderer_vulkan/vk_instance.h"

#include <cstring>
#include <dlfcn.h>
#ifdef __APPLE__
#include <sys/sysctl.h> // hw.memsize (no sysinfo; /proc reads below just fail: CPU/RSS stay 0)
#else
#include <sys/sysinfo.h>
#endif
#include <unistd.h>

namespace TlgPerfStats {

namespace {

std::mutex mutex;
Sample latest;
std::once_flag started;

// tlg: Linux measurements. CPU and RAM from /proc, VRAM from VK_EXT_memory_budget, GPU load from
// NVML (NVIDIA, loaded at run time) or the amdgpu sysfs counter; anything else stays "n/a".
class GpuUsage {
public:
    GpuUsage() {
        for (int card = 0; card < 8 && !sysfs; ++card) {
            char path[96];
            std::snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/gpu_busy_percent", card);
            if (std::FILE* f = std::fopen(path, "r")) { std::fclose(f); sysfs_path = path; sysfs = true; }
        }
        if (sysfs) return;
        nvml = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!nvml) return;
        using Init = int (*)();
        using Handle = int (*)(unsigned, void**);
        auto init = reinterpret_cast<Init>(dlsym(nvml, "nvmlInit_v2"));
        auto handle = reinterpret_cast<Handle>(dlsym(nvml, "nvmlDeviceGetHandleByIndex_v2"));
        rates = reinterpret_cast<Rates>(dlsym(nvml, "nvmlDeviceGetUtilizationRates"));
        if (!init || !handle || !rates || init() != 0 || handle(0, &device) != 0) rates = nullptr;
    }
    float Percent() {
        if (sysfs) {
            std::FILE* f = std::fopen(sysfs_path.c_str(), "r");
            int value = -1;
            if (f) { if (std::fscanf(f, "%d", &value) != 1) value = -1; std::fclose(f); }
            return value < 0 ? -1.0f : float(value);
        }
        struct { unsigned gpu, memory; } u{};
        return rates && rates(device, &u) == 0 ? float(u.gpu) : -1.0f;
    }
private:
    using Rates = int (*)(void*, void*);
    bool sysfs = false;
    std::string sysfs_path;
    void* nvml = nullptr;
    void* device = nullptr;
    Rates rates = nullptr;
};

uint64_t ProcessTicks() { // utime + stime in clock ticks
    std::FILE* f = std::fopen("/proc/self/stat", "r");
    if (!f) return 0;
    char buffer[1024];
    const size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
    std::fclose(f);
    buffer[n] = 0;
    const char* p = std::strrchr(buffer, ')'); // the command name may contain spaces
    unsigned long long utime = 0, stime = 0;
    if (!p || std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) != 2)
        return 0;
    return utime + stime;
}

void Run(const Vulkan::Instance* instance) {
    GpuUsage gpu;
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const long hz = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);
    constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
    uint64_t last_process = 0;
    auto last_wall = std::chrono::steady_clock::now();
    while (true) {
        Sample s;
        s.gpu_name = std::string(instance->GetModelName());
        const uint64_t process = ProcessTicks();
        const auto wall = std::chrono::steady_clock::now();
        if (last_process && hz > 0) {
            const uint64_t wall_ticks = uint64_t(std::chrono::duration<double>(wall - last_wall).count() * double(hz));
            s.cpu_percent = CpuPercent(process - last_process, wall_ticks, cores);
        }
        last_process = process;
        last_wall = wall;
        if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
            unsigned long long size = 0, resident = 0;
            if (std::fscanf(f, "%llu %llu", &size, &resident) == 2) s.ram_used_gb = double(resident) * double(page) / GiB;
            std::fclose(f);
        }
#ifdef __APPLE__
        unsigned long long memsize = 0;
        size_t memsize_len = sizeof(memsize);
        if (sysctlbyname("hw.memsize", &memsize, &memsize_len, nullptr, 0) == 0) s.ram_total_gb = double(memsize) / GiB;
#else
        struct sysinfo info{};
        if (sysinfo(&info) == 0) s.ram_total_gb = double(info.totalram) * double(info.mem_unit) / GiB;
#endif
        if (instance->CanReportMemoryUsage()) {
            s.vram_used_gb = double(instance->GetDeviceMemoryUsage()) / GiB;
#ifdef TLG_RENDERER_V2
            s.vram_total_gb = double(instance->GetTotalMemoryBudget()) / GiB;
#else
            s.vram_total_gb = double(instance->GetDeviceMemoryBudgetNow()) / GiB;
#endif
        }
        s.gpu_percent = gpu.Percent();
        {
            std::scoped_lock lock{mutex};
            latest = std::move(s);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

} // namespace

void Start(const Vulkan::Instance& instance) {
    // ponytail: never joined; a thread that ended while the game ran crashed in ntdll
    // (vk_pipeline_serialization.cpp, StopWarmUp). It sleeps between samples.
    std::call_once(started, [&] { std::thread(Run, &instance).detach(); });
}

Sample Latest() {
    std::scoped_lock lock{mutex};
    return latest;
}

} // namespace TlgPerfStats
