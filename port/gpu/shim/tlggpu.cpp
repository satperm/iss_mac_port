#include "tlg_split.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "common/signal_context.h"
#include "tlg_perf_trace.h"
#include "tlg_write_log.h"
// tlg: glue between the C loader and the vendored shadPS4 video core.
#include "tlg_overlay.h"
#include "tlg_png.h"
#include "tlg_settings.h"
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include "tlg_copy.h"
#include <sys/resource.h>
#include "tlg_region.h"
#include "tlg_toggles.h"
#include "tlg_freeze.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/backports.h"
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif
#include "../tlggpu.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
int runtime_memory_fill_backing(uintptr_t address, int value, uint64_t size);
int runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
void* runtime_memory_backing_view(uintptr_t address, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
uint64_t runtime_read_tsc(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
// runtime.c: exit counters (and the ISA site report), as on the guest's own exit
void runtime_report(void);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const TlgGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            // tlg: TLG_MAP_LOG=1: the guest's GPU mappings (arena layout measurements).
            static const bool map_log = [] {
                const char* env = std::getenv("TLG_MAP_LOG");
                return env && *env == '1';
            }();
            if (map_log) {
                std::printf("GPU map %#llx +%llu KiB\n", (unsigned long long)address,
                            (unsigned long long)(size >> 10));
            }
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    // tlg: in the split the copies to invalidate are the host's.
    if (TlgSplit::Remote()) {
        const TlgGpuRange record{{TLGGPU_REC_INVALIDATE, sizeof(TlgGpuRange)}, address, size};
        TlgSplit::Push(&record);
        return;
    }
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    return runtime_memory_clamp(virtual_addr, size);
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        // tlg: read through the unprotected backing view so readback protection (Precise
        // readbacks) cannot fault on the recorder/copy threads; direct read as fallback.
        if (!mapped) std::memset(dest, 0, n);
        else if (!runtime_memory_read_backing(source, dest, n))
            std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        source += n; dest += n; size -= n;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // tlg: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !TlgCopy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    TlgCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    TlgWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, TlgWriteLog::Backing);
    const bool written =
        runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
    // tlg (gow3_pc #331): the write skipped page tracking, so textures over it must reload.
    // Fence and label writes (<= 8 bytes) never hold texels and are left out: each one would send
    // every texture binding through the locked path.
    if (written && rasterizer && size > 8) {
        rasterizer->GetTextureCache().NoteBackingWrite(reinterpret_cast<VAddr>(address), size);
    }
    return written;
}
void* MemoryManager::BackingView(VAddr address, u64 size) {
    return runtime_memory_backing_view(address, size);
}
bool MemoryManager::FillBacking(void* address, int fill, u64 size) {
    // The write log keeps only the first bytes of a write; the pattern stands in for the fill
    // byte (Record reads at most the first 64 of them, and only 8 for writes over 64 bytes).
    std::uint8_t pattern[64];
    std::memset(pattern, fill, sizeof(pattern));
    TlgWriteLog::Note(reinterpret_cast<uintptr_t>(address), pattern, size, TlgWriteLog::Backing);
    const bool written =
        runtime_memory_fill_backing(reinterpret_cast<uintptr_t>(address), fill, size) != 0;
    // tlg (gow3_pc #331): the write skipped page tracking, so textures over it must reload; as
    // in TryWriteBacking, small writes cannot hold texels and are left out.
    if (written && rasterizer && size > 8) {
        rasterizer->GetTextureCache().NoteBackingWrite(reinterpret_cast<VAddr>(address), size);
    }
    return written;
}

void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    TlgStats::Timer timer{TlgStats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    TlgStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    TlgStats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        TlgStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        TlgStats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return runtime_read_tsc(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

u32 TlgDisplayRefreshHz() {
    static const u32 hz = [] {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        const u32 rate = mode && mode->refresh_rate > 0 ? u32(mode->refresh_rate + 0.5f) : 60;
        std::printf("GPU: vblank follows the display refresh rate, %u Hz\n", rate);
        return std::max<u32>(rate, 60);
    }();
    return hz;
}

#ifdef TLG_PGO_GENERATE
#ifdef __clang__
// clang's profile runtime: the file (default_<id>.profraw in TLG_PGO_DIR) merges each write.
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_reset_counters(void);
static void __gcov_dump(void) {
    __llvm_profile_write_file();
}
static void __gcov_reset(void) {
    __llvm_profile_reset_counters();
}
#else
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
#endif
// Instrumented build (TLG_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("tlg:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

// tlg: what is switched on for this player, written at the top of every log: the tlg.ini keys (the
// launcher's choices and the in-game menu's) and the TLG_/ISS_ variables set in the environment
// (paths are left out: logs are posted in public issues).
static void LogPlayerConfig() {
    std::printf("Config: ---- player configuration ----\n");
    const char* cfg = std::getenv("TLG_CONFIG");
    std::ifstream ini(cfg && *cfg ? cfg : "tlg.ini");
    std::string line;
    int n = 0;
    while (ini && std::getline(ini, line) && n < 300) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#' || line.find('=') == std::string::npos) continue;
        std::printf("Config ini: %s\n", line.c_str());
        ++n;
    }
    if (n == 0) std::printf("Config ini: (empty or missing: every setting at its default)\n");
    std::vector<std::string> vars;
    extern char** environ;
    char** envp = environ;
    for (char** e = envp; e && *e; ++e) {
        const std::string entry = *e;
        if (entry.rfind("TLG_", 0) != 0 && entry.rfind("ISS_", 0) != 0) continue;
        const auto eq = entry.find('=');
        if (eq == std::string::npos) continue;
        const std::string value = entry.substr(eq + 1);
        if (value.find('/') != std::string::npos || value.find('\\') != std::string::npos || value.size() > 40) continue;
        vars.push_back(entry);
    }
    std::sort(vars.begin(), vars.end());
    for (const auto& v : vars) std::printf("Config env: %s\n", v.c_str());
    if (vars.empty()) std::printf("Config env: (no TLG_/ISS_ variable set)\n");
    std::printf("Config: ----\n");
}

// tlg: the part of tlggpu_init that the shader precompiler needs without a window or the game:
// the player's settings (the backports chosen there change the shader cache version) and the
// game's identity (some recompiler switches are on by default for this serial).
extern "C" void tlggpu_init_headless(const TlgGpuConfig* config) {
    TlgSettings::Load();
    {
        const auto& settings = TlgSettings::Get();
        for (int i = 0; i < TlgSettings::kBackportPrefs; ++i) {
            const int pref = settings.backport_pref[i].load();
            if (pref >= 0) {
                Common::Backports::ApplyConfig(static_cast<std::size_t>(i), pref != 0);
            }
        }
    }
    g_sdk_version = config->sdk_version;
    if (config->user_dir) setenv("TLG_GPU_USER_DIR", config->user_dir, 0);
    TlgPerf::InitFromEnvironment();
    Core::Emulator::FillElfInfo(*config);
}

extern "C" int tlggpu_init(const TlgGpuConfig* config) {
    TlgSettings::Load();
    // tlg: the fixes chosen in the launcher or the in-game menu (bpNN in tlg.ini); an environment
    // variable TLG_BPnn / ISS_BPnn still wins.
    {
        const auto& settings = TlgSettings::Get();
        for (int i = 0; i < TlgSettings::kBackportPrefs; ++i) {
            const int pref = settings.backport_pref[i].load();
            if (pref >= 0) {
                Common::Backports::ApplyConfig(static_cast<std::size_t>(i), pref != 0);
            }
        }
    }
    LogPlayerConfig();
#ifdef TLG_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
    if (config->user_dir) setenv("TLG_GPU_USER_DIR", config->user_dir, 0);
    TlgPerf::InitFromEnvironment();
    Core::Emulator::FillElfInfo(*config);
    const std::string title = config->title ? config->title : "ISS";
    const s32 width = config->width, height = config->height;
    auto window_loop = [title, width, height] {
        Common::SetCurrentThreadName("tlg:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        LOG_INFO(Frontend, "Window closed by user");
        runtime_report();
        std::fflush(stdout);
        TlgPerf::Stop(); // _Exit bypasses atexit; finish diagnostics on normal window close.
        std::_Exit(0);
    };
#ifdef __APPLE__
    // Cocoa only works on the main thread: tlg-probe runs the game on another thread and the main
    // run loop on the main thread, so the window and its event loop go to the main queue.
    // tlg: one event pass per main-queue block, the next one 2 ms later, instead of one block that
    // never returns: other main-queue work runs in between (Metal Performance HUD attaches its
    // overlay there; with the endless block it never showed). TLG_WINDOW_LOOP=block: as before.
    static const bool block_loop = [] {
        const char* v = std::getenv("TLG_WINDOW_LOOP");
        return v && !std::strcmp(v, "block");
    }();
    if (!block_loop) {
        struct Tick {
            static void Run(void* data) {
                auto* window = static_cast<Frontend::WindowSDL*>(data);
                if (window->PollEvents()) {
                    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_MSEC),
                                     dispatch_get_main_queue(), window, &Run);
                    return;
                }
                LOG_INFO(Frontend, "Window closed by user");
                runtime_report();
                std::fflush(stdout);
                TlgPerf::Stop();
                std::_Exit(0);
            }
        };
        struct Start {
            std::string title;
            s32 width, height;
        };
        dispatch_async_f(dispatch_get_main_queue(), new Start{title, width, height}, [](void* data) {
            std::unique_ptr<Start> start{static_cast<Start*>(data)};
            Common::SetCurrentThreadName("tlg:window");
            auto* window = new Frontend::WindowSDL(start->width, start->height, start->title.c_str());
            {
                std::scoped_lock lock{g_window_mutex};
                g_window = window;
                g_window_ready = true;
            }
            g_window_cv.notify_all();
            Tick::Run(window);
        });
    } else {
        using Loop = decltype(window_loop);
        dispatch_async_f(dispatch_get_main_queue(), new Loop(std::move(window_loop)),
                         [](void* loop) { (*static_cast<Loop*>(loop))(); });
    }
#else
    g_window_thread = std::thread(std::move(window_loop));
    g_window_thread.detach();
#endif
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

extern std::unique_ptr<Vulkan::Presenter> presenter;
namespace Libraries::Kernel { void StartKernelService(); }
namespace Libraries::GnmDriver {
bool TlgSplitHost(const TlgGpuRecord* record);
}
namespace Libraries::VideoOut {
bool TlgSplitHost(const TlgGpuRecord* record);
u64 TlgSplitLabelAddress();
}

// ---- tlg: ISS UI calls across the process split (host/tlggpu_shared.h TlgGpuUiCall) ----
// The game's side sends its UI and input calls (save dialogs, notices, text entry, the mouse) to
// the host, where the window and the overlay are; the host runs the same functions locally.
namespace TlgUi {
struct Blob {
    std::string bytes;
    void Str(const char* text) {
        bytes += text ? text : "";
        bytes += '\0';
    }
    void Raw(const void* data, size_t n) {
        bytes.append(static_cast<const char*>(data), n);
    }
    void Entry(const TlgSaveEntry& e) {
        Str(e.title);
        Str(e.subtitle);
        Str(e.detail);
        Str(e.icon);
        Raw(&e.mtime, sizeof e.mtime);
        Raw(&e.bytes, sizeof e.bytes);
        Raw(&e.is_new, sizeof e.is_new);
    }
};
struct Reader {
    const char* at;
    const char* end;
    const char* Str() {
        const char* s = at;
        at += std::strlen(at) + 1;
        return s;
    }
    template <typename T>
    T Raw() {
        T v{};
        std::memcpy(&v, at, sizeof v);
        at += sizeof v;
        return v;
    }
    /// The entry's strings point into the record (valid while it is handled).
    TlgSaveEntry Entry() {
        TlgSaveEntry e{};
        e.title = Str();
        e.subtitle = Str();
        e.detail = Str();
        const char* icon = Str();
        e.icon = *icon ? icon : nullptr;
        e.mtime = Raw<int64_t>();
        e.bytes = Raw<uint64_t>();
        e.is_new = Raw<int>();
        return e;
    }
};
std::mutex call_mutex; // ui_reply is one buffer

/// Sends a UI call and waits for its answer; `reply` (if any) receives ui_reply's first bytes.
s64 Call(u32 op, std::initializer_list<s32> ints = {}, u64 value = 0,
         std::initializer_list<float> floats = {}, const Blob* blob = nullptr, void* reply = nullptr,
         size_t reply_bytes = 0) {
    static thread_local std::unique_ptr<TlgGpuUiCall> record{new TlgGpuUiCall};
    TlgGpuUiCall& c = *record;
    c.op = op;
    std::fill(std::begin(c.i), std::end(c.i), 0);
    std::fill(std::begin(c.f), std::end(c.f), 0.0f);
    size_t k = 0;
    for (const s32 v : ints) c.i[k++] = v;
    k = 0;
    for (const float v : floats) c.f[k++] = v;
    c.value = value;
    c.blob_bytes = 0;
    if (blob) {
        c.blob_bytes = u32(std::min<size_t>(blob->bytes.size(), TLGGPU_UI_BLOB_MAX));
        std::memcpy(c.blob, blob->bytes.data(), c.blob_bytes);
    }
    c.r = {TLGGPU_REC_UI_CALL, tlggpu_ui_call_bytes(c.blob_bytes)};
    std::scoped_lock lock{call_mutex};
    const s64 result = TlgSplit::Request(&c, &c.slot);
    if (reply && reply_bytes) {
        std::memcpy(reply, TlgSplit::Shared()->ui_reply,
                    std::min(reply_bytes, sizeof(TlgSplit::Shared()->ui_reply)));
    }
    return result;
}
} // namespace TlgUi

// ---- tlg (from bloodborne_mac_port): the two-process split (tlg_split.h, host/tlggpu_shared.h) ----

extern "C" int tlggpu_host_start(const TlgGpuConfig* config, void* shared) {
    TlgSplit::InitHost(static_cast<TlgGpuShared*>(shared));
    const int result = tlggpu_init(config);
    if (result == 0) {
        auto* s = static_cast<TlgGpuShared*>(shared);
        s->label_address = Libraries::VideoOut::TlgSplitLabelAddress();
        s->hdr_supported = presenter && presenter->IsHDRSupported() ? 1 : 0;
        TlgSplit::ForwardInterrupts();
        // Keyboard for the game's pad emulation (the window is the host's).
        std::thread([s] {
            Common::SetCurrentThreadName("tlg:KeyBridge");
            for (;;) {
                int count = 0;
                const bool* keys = SDL_GetKeyboardState(&count);
                if (keys) {
                    std::memcpy(s->keys, keys, std::min<size_t>(size_t(count), sizeof s->keys));
                    __atomic_store_n(&s->keys_valid, 1u, __ATOMIC_RELEASE);
                }
                __atomic_store_n(&s->overlay_captures, tlggpu_overlay_captures_input() ? 1u : 0u,
                                 __ATOMIC_RELAXED);
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }).detach();
    }
    return result;
}

extern "C" int tlggpu_host_fault(uint64_t address, int write) {
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
    if (!rasterizer) {
        return 0;
    }
    if (write) {
        return rasterizer->OnWriteFault(address, false) ? 1 : 0;
    }
    return rasterizer->ReadMemory(address, 8, false) ? 1 : 0;
}

extern "C" void tlggpu_mouse_take(TlgGpuMouse* mouse);
void TlgSplitMouseTake(TlgGpuMouse* out) {
    TlgUi::Call(TLGGPU_UI_MOUSE_TAKE, {}, 0, {}, nullptr, out, sizeof *out);
}

/// Host side: runs a UI call from the game (the same functions, locally) and answers it.
static void HostUiCall(const TlgGpuUiCall& c) {
    TlgGpuShared* s = TlgSplit::Shared();
    TlgUi::Reader in{c.blob, c.blob + c.blob_bytes};
    s64 result = 0;
    switch (c.op) {
    case TLGGPU_UI_TEXT_BEGIN: {
        const char* initial = in.Str();
        const char* prompt = in.Str();
        result = tlggpu_text_input_begin(initial, prompt);
        break;
    }
    case TLGGPU_UI_TEXT_POLL: {
        char* text = reinterpret_cast<char*>(s->ui_reply);
        result = tlggpu_text_input_poll(text, sizeof s->ui_reply);
        break;
    }
    case TLGGPU_UI_SAVE_LIST_BEGIN: {
        std::vector<TlgSaveEntry> entries;
        for (int i = 0; i < c.i[1] && in.at < in.end; ++i) entries.push_back(in.Entry());
        result = tlggpu_save_list_begin(c.i[0], entries.data(), int(entries.size()), c.i[2]);
        break;
    }
    case TLGGPU_UI_SAVE_LIST_POLL:
        result = tlggpu_save_list_poll();
        break;
    case TLGGPU_UI_SAVE_MESSAGE_BEGIN: {
        const char* text = in.Str();
        TlgSaveEntry entry{};
        if (c.i[3]) entry = in.Entry();
        result = tlggpu_save_message_begin(c.i[0], c.i[1], c.value, text, c.i[2],
                                           c.i[3] ? &entry : nullptr);
        break;
    }
    case TLGGPU_UI_SAVE_MESSAGE_POLL:
        result = tlggpu_save_message_poll();
        break;
    case TLGGPU_UI_SAVE_DIALOG_CLOSE:
        tlggpu_save_dialog_close();
        break;
    case TLGGPU_UI_SAVE_PROGRESS: {
        const char* text = in.Str();
        tlggpu_save_progress(c.i[0], c.i[1], c.i[2], c.i[3] ? text : nullptr);
        break;
    }
    case TLGGPU_UI_GRAFFITI_GUIDE:
        tlggpu_graffiti_guide(c.i[0]);
        break;
    case TLGGPU_UI_SAVE_TOAST:
        tlggpu_save_toast(c.i[0]);
        break;
    case TLGGPU_UI_OVERLAY_NOTICE:
        tlggpu_overlay_notice(in.Str(), c.f[0]);
        break;
    case TLGGPU_UI_SCREENSHOT:
        tlggpu_screenshot();
        break;
    case TLGGPU_UI_MOUSE_TAKE: {
        static_assert(sizeof(TlgGpuMouse) <= sizeof s->ui_reply);
        TlgGpuMouse mouse{};
        tlggpu_mouse_take(&mouse);
        std::memcpy(s->ui_reply, &mouse, sizeof mouse);
        break;
    }
    case TLGGPU_UI_MOUSE_PAD_HUD:
        tlggpu_mouse_pad_hud(c.i[0], c.f[0], c.f[1], c.i[1], c.i[2]);
        break;
    case TLGGPU_UI_SENSOR_AIM_SCALE: {
        const float scale = tlggpu_sensor_aim_scale();
        std::memcpy(s->ui_reply, &scale, sizeof scale);
        break;
    }
    case TLGGPU_UI_INPUT_SETTINGS: {
        TlgGpuInputSettings settings{};
        tlggpu_input_settings(&settings);
        std::memcpy(s->ui_reply, &settings, sizeof settings);
        break;
    }
    default:
        result = -1;
    }
    tlggpu_reply_send(s, c.slot, result);
}

extern "C" int tlggpu_host_dispatch(const void* record) {
    const auto* r = static_cast<const TlgGpuRecord*>(record);
    if (r->type == TLGGPU_REC_UI_CALL) {
        HostUiCall(*static_cast<const TlgGpuUiCall*>(record));
        return 1;
    }
    if (r->type == TLGGPU_REC_INVALIDATE) {
        const auto* range = static_cast<const TlgGpuRange*>(record);
        Core::Memory::Instance()->InvalidateMemory(range->address, range->size);
        return 1;
    }
    return Libraries::GnmDriver::TlgSplitHost(r) || Libraries::VideoOut::TlgSplitHost(r) ? 1 : 0;
}

extern "C" void tlggpu_split_attach(void* shared, int host_pid) {
    auto* s = static_cast<TlgGpuShared*>(shared);
    s->host_pid = host_pid;
    s->game_pid = getpid();
    TlgSplit::Attach(s);
}

extern "C" void tlggpu_split_post_restart(void) {
    const TlgGpuRecord record{TLGGPU_EV_RESTART, sizeof(TlgGpuRecord)};
    TlgSplit::Post(&record);
}

// The pipeline caches are written as they change (PipelineCacheIO); nothing waits at exit yet.
extern "C" int tlggpu_flush_caches(unsigned timeout_ms) {
    (void)timeout_ms;
    return 0;
}

extern "C" void tlggpu_split_push(const void* record) {
    TlgSplit::Push(record);
}

extern "C" const bool* tlggpu_split_keyboard(void) {
    if (!TlgSplit::Remote() || !__atomic_load_n(&TlgSplit::Shared()->keys_valid, __ATOMIC_ACQUIRE)) {
        return nullptr;
    }
    static_assert(sizeof(bool) == 1);
    return reinterpret_cast<const bool*>(TlgSplit::Shared()->keys);
}

extern "C" int tlggpu_init_remote(const TlgGpuConfig* config, void* shared_block) {
    auto* s = static_cast<TlgGpuShared*>(shared_block);
    TlgSettings::Load();
    g_sdk_version = config->sdk_version;
    Core::Emulator::FillElfInfo(*config);
    TlgSplit::InitRemote(s);
    std::snprintf(s->title, sizeof s->title, "%s", config->title ? config->title : "ISS");
    std::snprintf(s->serial, sizeof s->serial, "%s", config->serial ? config->serial : "UNKNOWN");
    std::snprintf(s->user_dir, sizeof s->user_dir, "%s", config->user_dir ? config->user_dir : "user");
    s->sdk_version = config->sdk_version;
    s->psf_attributes = config->psf_attributes;
    s->width = config->width;
    s->height = config->height;
    s->tsc_hz = runtime_tsc_frequency();
    {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        s->process_start_ns = u64(now.tv_sec) * 1'000'000'000ull + u64(now.tv_nsec) -
                              runtime_process_time_counter();
    }
    TlgGpuStart start{{TLGGPU_REC_START, sizeof(TlgGpuStart)}, 0, 0};
    const s64 result = TlgSplit::Request(&start, &start.slot);
    if (result != 0) {
        std::printf("GPU split: the GPU host could not start the renderer (%lld)\n",
                    static_cast<long long>(result));
        return -1;
    }
    Core::Loader::SymbolsResolver resolver;
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    TlgSplit::StartEventDispatcher();
    std::printf("GPU split: rendering in the GPU host (labels at %#llx, HDR %s)\n",
                static_cast<unsigned long long>(s->label_address), s->hdr_supported ? "yes" : "no");
    return 0;
}
extern "C" void tlggpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
}

extern "C" uintptr_t tlggpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int tlggpu_handle_fault(void* ucontext, void* address) {
    if (TlgSplit::Remote()) {
        // tlg: the host tracks GPU pages; it unprotects before it answers.
        return TlgSplit::RelayFault(reinterpret_cast<u64>(address), Common::IsWriteError(ucontext))
                   ? 1
                   : 0;
    }
    // tlg: the freeze report shows threads stuck in a page-protection fault (and how many).
    TlgFreeze::faults_in_progress.fetch_add(1, std::memory_order_relaxed);
    int handled;
    {
        TlgFreeze::WaitScope wait{"GPU page fault handler", address};
        handled = Core::Signals::Instance()->DispatchAccessViolation(ucontext, address) ? 1 : 0;
    }
    TlgFreeze::faults_in_progress.fetch_sub(1, std::memory_order_relaxed);
    TlgFreeze::faults_handled.fetch_add(handled, std::memory_order_relaxed);
    return handled;
}

extern "C" void tlggpu_set_wait_hooks(TlgGpuWaitBegin begin, TlgGpuWaitEnd end) {
    TlgFreeze::end_hook.store(end);
    TlgFreeze::begin_hook.store(begin);
}

namespace TlgGuestImage {
std::atomic<uintptr_t> base{0};
std::atomic<uint64_t> size{0};
} // namespace TlgGuestImage

extern "C" void tlggpu_set_guest_image(uintptr_t base, uint64_t size) {
    TlgGuestImage::size.store(size);
    TlgGuestImage::base.store(base);
}

extern "C" void tlggpu_note_open(const char* guest_path) {
    TlgRegion::NoteOpen(guest_path);
}

extern "C" void tlggpu_frame_counters(TlgGpuFrameCounters* out) {
    out->submitted = TlgFreeze::flips_submitted.load(std::memory_order_relaxed);
    out->shown = TlgFreeze::flips_shown.load(std::memory_order_relaxed);
    out->presented = TlgFreeze::presents.load(std::memory_order_relaxed);
    out->vblanks = TlgFreeze::vblanks.load(std::memory_order_relaxed);
    out->faults_in_progress = TlgFreeze::faults_in_progress.load(std::memory_order_relaxed);
    out->faults_handled = TlgFreeze::faults_handled.load(std::memory_order_relaxed);
}

extern "C" uint64_t tlggpu_total_draws(void) {
    return TlgStats::draws.load(std::memory_order_relaxed);
}

extern "C" unsigned tlggpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("tlg:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore

extern "C" int tlggpu_cpu_write(void* dst, const void* src, int fill, uint64_t size) {
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
    return rasterizer && rasterizer->GetBufferCache().CpuWriteMerge(
                             reinterpret_cast<VAddr>(dst), src, fill, size)
               ? 1
               : 0;
}


namespace Vulkan {
void RequestFrameShot();
}
extern "C" void tlggpu_screenshot(void) {
    if (TlgSplit::Remote()) {
        TlgUi::Call(TLGGPU_UI_SCREENSHOT, {}, 0, {});
        return;
    }
    Vulkan::RequestFrameShot();
}

extern "C" void tlggpu_overlay_notice(const char* text, float seconds) {
    if (TlgSplit::Remote()) {
        TlgUi::Blob b;
        b.Str(text);
        TlgUi::Call(TLGGPU_UI_OVERLAY_NOTICE, {}, 0, {seconds}, &b);
        return;
    }
    TlgOverlay::Notice(text ? text : "", seconds);
}

extern "C" int tlggpu_overlay_captures_input(void) {
    if (TlgSplit::Remote()) {
        return __atomic_load_n(&TlgSplit::Shared()->overlay_captures, __ATOMIC_RELAXED) ? 1 : 0;
    }
    return TlgOverlay::CapturesInput() ? 1 : 0;
}

extern "C" void tlggpu_input_settings(TlgGpuInputSettings* out) {
    if (TlgSplit::Remote()) {
        static_assert(sizeof(TlgGpuInputSettings) <= sizeof(TlgGpuShared::ui_reply));
        TlgUi::Call(TLGGPU_UI_INPUT_SETTINGS, {}, 0, {}, nullptr, out, sizeof *out);
        return;
    }
    const auto& s = TlgSettings::Get();
    out->mouse_enabled = s.mouse_enabled ? 1 : 0;
    out->invert_y = s.mouse_invert_y ? 1 : 0;
    out->rumble = s.rumble ? 1 : 0;
    out->sensitivity = s.mouse_sensitivity;
    out->smoothing = s.mouse_smoothing;
    out->accel = s.mouse_accel;
    out->deadzone = s.mouse_deadzone;
    std::snprintf(out->key_map, sizeof(out->key_map), "%s", s.key_map.c_str());
}

extern "C" void tlggpu_mouse_pad_hud(int active, float x, float y, int touching, int clicking) {
    if (TlgSplit::Remote()) {
        TlgUi::Call(TLGGPU_UI_MOUSE_PAD_HUD, {active, touching, clicking}, 0, {x, y});
        return;
    }
    TlgOverlay::SetMousePad(active != 0, x, y, touching != 0, clicking != 0);
}

extern "C" int tlggpu_text_input_begin(const char* initial, const char* prompt) {
    if (TlgSplit::Remote()) {
        TlgUi::Blob b;
        b.Str(initial);
        b.Str(prompt);
        return int(TlgUi::Call(TLGGPU_UI_TEXT_BEGIN, {}, 0, {}, &b));
    }
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

static TlgOverlay::SaveRow SaveRowOf(const TlgSaveEntry& entry) {
    TlgOverlay::SaveRow row;
    row.title = entry.title ? entry.title : "";
    row.subtitle = entry.subtitle ? entry.subtitle : "";
    row.detail = entry.detail ? entry.detail : "";
    row.mtime = entry.mtime;
    row.bytes = entry.bytes;
    row.is_new = entry.is_new != 0;
    return row;
}

static TlgPng::Image SavePicture(const char* path) {
    TlgPng::Image picture;
    if (path && !TlgPng::LoadRgba(path, picture)) {
        LOG_WARNING(Lib_SaveDataDialog, "save dialog: no picture from {}", path);
    }
    return picture;
}

extern "C" int tlggpu_save_list_begin(int kind, const TlgSaveEntry* entries, int count, int focus) {
    if (TlgSplit::Remote()) {
        if (count <= 0 || !entries) return 0;
        TlgUi::Blob b;
        for (int i = 0; i < count; ++i) b.Entry(entries[i]);
        return int(TlgUi::Call(TLGGPU_UI_SAVE_LIST_BEGIN, {kind, count, focus}, 0, {}, &b));
    }
    if (!g_window || count <= 0 || !entries) return 0;
    // Pictures are decoded here, on the game's thread, before the list is shown.
    std::vector<TlgOverlay::SaveRow> rows;
    std::vector<TlgPng::Image> pictures;
    for (int i = 0; i < count; ++i) {
        rows.push_back(SaveRowOf(entries[i]));
        pictures.push_back(SavePicture(entries[i].icon));
    }
    TlgOverlay::BeginSaveList(kind, std::move(rows), std::move(pictures), focus);
    return 1;
}

extern "C" int tlggpu_save_list_poll(void) {
    if (TlgSplit::Remote()) return int(TlgUi::Call(TLGGPU_UI_SAVE_LIST_POLL));
    return TlgOverlay::PollSaveList();
}

extern "C" int tlggpu_save_message_begin(int kind, int code, uint64_t value, const char* text,
                                         int buttons, const TlgSaveEntry* entry) {
    if (TlgSplit::Remote()) {
        TlgUi::Blob b;
        b.Str(text);
        if (entry) b.Entry(*entry);
        return int(TlgUi::Call(TLGGPU_UI_SAVE_MESSAGE_BEGIN, {kind, code, buttons, entry ? 1 : 0},
                               value, {}, &b));
    }
    if (!g_window) return 0;
    TlgOverlay::SaveRow row;
    if (entry) row = SaveRowOf(*entry);
    TlgOverlay::BeginSaveMessage(kind, code, value, text ? text : "", buttons, entry ? &row : nullptr,
                                 SavePicture(entry ? entry->icon : nullptr));
    return 1;
}

extern "C" int tlggpu_save_message_poll(void) {
    if (TlgSplit::Remote()) return int(TlgUi::Call(TLGGPU_UI_SAVE_MESSAGE_POLL));
    return TlgOverlay::PollSaveMessage();
}

extern "C" void tlggpu_save_dialog_close(void) {
    if (TlgSplit::Remote()) {
        TlgUi::Call(TLGGPU_UI_SAVE_DIALOG_CLOSE);
        return;
    }
    TlgOverlay::CloseSaveDialog();
}

extern "C" void tlggpu_save_progress(int kind, int active, int percent, const char* text) {
    if (TlgSplit::Remote()) {
        TlgUi::Blob b;
        b.Str(text);
        TlgUi::Call(TLGGPU_UI_SAVE_PROGRESS, {kind, active, percent, text ? 1 : 0}, 0, {}, &b);
        return;
    }
    if (g_window) TlgOverlay::SaveProgress(kind, active, percent, text);
}

extern "C" float tlggpu_sensor_aim_scale(void) {
    if (TlgSplit::Remote()) {
        float scale = 1.0f;
        TlgUi::Call(TLGGPU_UI_SENSOR_AIM_SCALE, {}, 0, {}, nullptr, &scale, sizeof scale);
        return scale;
    }
    return float(TlgSettings::Get().sensor_aim.load()) / 100.0f;
}

extern "C" void tlggpu_graffiti_guide(int force) {
    if (TlgSplit::Remote()) {
        TlgUi::Call(TLGGPU_UI_GRAFFITI_GUIDE, {force});
        return;
    }
    if (g_window) TlgOverlay::ShowGraffitiGuide(force != 0);
}

extern "C" void tlggpu_save_toast(int kind) {
    if (TlgSplit::Remote()) {
        TlgUi::Call(TLGGPU_UI_SAVE_TOAST, {kind});
        return;
    }
    if (g_window) TlgOverlay::SaveToast(kind);
}

extern "C" int tlggpu_text_input_poll(char* out, uint64_t size) {
    if (TlgSplit::Remote()) {
        char text[sizeof(TlgGpuShared::ui_reply)];
        const int state = int(TlgUi::Call(TLGGPU_UI_TEXT_POLL, {}, 0, {}, nullptr, text, sizeof text));
        text[sizeof text - 1] = 0;
        if (size) {
            const size_t n = std::min<size_t>(std::strlen(text), size - 1);
            std::memcpy(out, text, n);
            out[n] = 0;
        }
        return state;
    }
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
