# ISS on macOS (Apple Silicon): plan

Status 2026-10-27: **M1 done** (everything builds for macOS, all unit tests pass under Rosetta 2); M2 next. Repository: [satperm/iss_mac_port](https://github.com/satperm/iss_mac_port),
a fork of [HayatoG/iss_pc](https://github.com/HayatoG/iss_pc) (macOS work on the `macos` branch).

## Goal

Run ISS (PS4, **CUSA00223** or **CUSA00004**, base 1.00 + update **1.07**; both
editions run the same `eboot.bin`) on Apple Silicon Macs
with the approach that already works for Bloodborne: the game's own x86-64 code runs directly on
the CPU (under Rosetta 2 at first), the PS4 system libraries are reimplemented, and the GPU work is
translated, first through Vulkan on MoltenVK, later by a native Metal renderer.

Starting point: [HayatoG/iss_pc](https://github.com/HayatoG/iss_pc) (Linux and Windows, playable,
GPL-2.0). HayatoG's answer to the macOS question ([iss_pc#717](https://github.com/HayatoG/iss_pc/issues/717),
2026-10-27): a fork is welcome, use whatever is needed from the core, credit appreciated; they
can't help with the port themselves, and recommend **GPU V2** (`port/gpu_v2/`), the most up-to-date
renderer, for performance.

## Ground rules (legal)

- No game content in the repository or releases: no `eboot.bin`, assets, art, keys or firmware.
  Players bring their own dump of a game they own (decrypted on their own PS4).
- No decryption code or keys. The runner loads an already-decrypted dump.
- GPL-2.0-or-later, source public, upstream credits and licence files kept (`UPSTREAM.md`).
- "ISS" is a the publisher trademark: descriptive names only, "not affiliated with the publisher Interactive
  Entertainment or the developer", no logos; the app icon comes from the player's own
  `sce_sys/icon0.png` at build time.
- Patches apply in memory at start; patched game files are never written or shared.

## Lineage (why the Bloodborne Mac work applies)

```
deadinside28/bloodborne_pc ─┬─ Mrsuss60 (Windows) ── Wooloomooloo2/bloodborne_mac_port (macOS, playable)
                            └─ Supermedo/bloodborne_pc ── FelipePrado0/gow3_pc ── HayatoG/iss_pc
                                                                          (same core: tlg_pc, gr2_pc)
```

Both sides keep the same shape: a loader for the flat game image, a C runtime in `port/src/`
(`runtime_*.c`, 19 of bloodborne_mac_port's 26 runtime files have counterparts in iss_pc), and a
shadPS4-derived video core in `port/gpu/`. iss_pc adds game hooks (camera, karma, HUD, mouse aim),
an ISA rewriter/emulator (`isa_rewrite.c`, `isa_emulate.c`: MOVBE/BMI1/LZCNT and SSE4A for CPUs
without them) and a second, experimental renderer (`gpu_v2/`, from Castlesea's Windows port).

## What carries over from bloodborne_mac_port

| Area | In bloodborne_mac_port | For iss_pc |
|---|---|---|
| Toolchain | `build_macos.sh`, x86-64 target `x86_64-apple-macos15.0` (Rosetta AVX2) | same flags in `port/build.sh`'s macOS branch |
| Dependencies | `scripts/macos/bootstrap_deps.sh` (pinned x86-64 + arm64 deps, MoltenVK) | reuse; add FreeType |
| Guest memory | `runtime_memory.c`: kernel hole [0xfc0000000, 0x7000000000) → guest `USER_MIN` 0x7000000000, whole range reserved up front, `mach_vm_remap` aliases | port the same changes |
| TLS | loader rewrites `fs:[0]` → `gs:[0xc00]`; pthread key 384 reserved (`runtime_thread.c`) | same (Rosetta has no usable `fs` base) |
| Faults | write faults arrive as SIGBUS; thread dumps via `task_threads` | same |
| Main thread | Cocoa needs it: game on a 64 MiB thread, `CFRunLoopRun()` in `main` | same |
| Timing | macOS timed waits are trylock + backoff; `CLOCK_MONOTONIC` | check iss_pc's waits |
| Vulkan on MoltenVK | no `robustBufferAccess2`/`nullDescriptor`/`geometryShader`/`logicOp`; BC images without Storage usage; no SPIR-V `NoContraction` in fragment/compute | same fixes in `port/gpu/` |
| Native GPU process | arm64 `bbgpu-host` sharing guest memory (Mach memory entry, rings, fault relays): `host/`, `docs/DESIGN_NATIVE_GPU_PROCESS.md` | later milestone |
| Native Metal renderer | `renderer_metal`, MetalFX, FSR 3.1 on Metal | later milestone |
| App | Swift launcher, `package_app.sh`, update check, session logs, crash reports | reuse, rebranded |

## Milestones

Each one ends with something that can be shown and measured.

**M0: setup.** Xcode, `bootstrap_deps.sh`, `git clone --recursive` of iss_pc, the player's dump
(CUSA00223 1.07) checked by iss_pc's own preparation step. *Done when* iss_pc's Linux build
scripts run on macOS far enough to show what fails.

**M1: it compiles.** `port/build.sh` gains a macOS branch: x86-64 runtime, loader and GPU library
for `x86_64-apple-macos15.0`, Linux-only APIs replaced (the Bloodborne port found few). *Done when*
the build is clean and iss_pc's `tests/` pass under Rosetta.

**M2: the CPU side boots.** Guest memory layout, TLS, signals and the main-thread change from
bloodborne_mac_port. Check what Rosetta lacks: Rosetta has MOVBE/BMI1/LZCNT/AVX2, but SSE4A is
an AMD extension that Rosetta (like Intel CPUs) does not have, so iss_pc's Intel path
(`isa_emulate.c`/`isa_rewrite.c`) must be on. *Done when* the game's threads run up to graphics
init with no unbound imports left.

**M3: first frame on MoltenVK.** Device creation without the missing features, BC textures,
shader fixes. *Done when* the title screen renders.

**M4: playable.** Input (SDL3 gamepads, keyboard/mouse from iss_pc), audio (AudioOut/Ajm over
CoreAudio through SDL), saves, the game's hooks. *Done when* a save loads and Seattle can be
explored; first FPS numbers on an M3 Max.

**M5: performance.** In order: the arm64 GPU process (removes Rosetta from the rendering CPU
work: 1.75× in the Bloodborne spike), then the native Metal renderer. *Done when* 1080p holds 60
FPS on an M-series Max chip.

**Renderer choice.** Target **`gpu_v2/`**, as HayatoG recommends: it records the engine's passes
natively and already runs on Linux through `gpu_v2/compat/` (the Win32 subset over POSIX), which
is the path macOS follows. Keep `gpu/` (v1) as the fallback if gpu_v2 blocks M3 (v1 is the closer
relative of bloodborne_mac_port's video core, so its MoltenVK fixes apply most directly).

**M6: the Mac app.** Swift launcher (game folder check, settings, logs, crash reports, update
check), `.app` packaging, CI build of releases.

## Progress

**M1 done (2026-10-27).** `bash port/scripts/macos/bootstrap_deps.sh` then
`bash port/build_macos.sh --test`: libtlggpu.dylib, tlg-probe and tlg-gpu-capabilities build as
x86-64; every unit test passes under Rosetta 2 (pad, mouse, runtime, semaphores, rwlocks, freeze
report, content, mp3, video decoding, services, ISA emulation and int3 trap sites, red zone, CPU
check). What M1 changed and learned:

- `src/compat_darwin.h` (from `runtime.h`, like `compat_win.h`): gettid, getrandom, timed mutex and
  rwlock acquires (trylock + backoff), clock_nanosleep, barriers, `st_*tim`.
- Guest TLS: `mov reg, fs:[0]` becomes `gs:[384*8]`; pthread key 384 holds each thread's TCB
  (`runtime_thread.c`, `scripts/link_libc.py`), as bloodborne_mac_port.
- Guest memory (`runtime_memory.c`): guest range from 0x7000000000 (above macOS's permanent hole),
  guest range and system window reserved up front, `mach_vm_remap` views of one anonymous pool,
  low maps stop at 0xfc0000000.
- Signals: registers through `UC_REG()`/`uc_gpr()`; the int3 ISA trap path works under Rosetta
  (register changes in the signal context are honoured, `isa-stub-test`).
- **Rosetta's CPUID under-reports:** it advertises only up to SSE4.2/AES, yet executes AVX, AVX2,
  FMA, F16C, MOVBE, LZCNT, BMI1, BMI2 and POPCNT (measured); SSE4A raises SIGILL. `cpu_check.c` adds
  the executed sets under Rosetta (`TLG_ROSETTA_CPUID=0` turns it off), so only SSE4A is emulated;
  `isa-emulate-test` then checks Rosetta's MOVBE/BMI1/LZCNT results against the emulation.
- Freeze report: threads suspended through Mach (`thread_get_state`), not signalled.

**M2 done (2026-10-27).** `TLG_GAME_DIR=... bash port/run_macos.sh --cpu-only` (CUSA00004 1.07):
the eboot links (3,616 fs loads to gs), 96 SSE4A sites rewritten, and the game's code runs under
Rosetta (TLS, 5 GB direct memory at 0x7000000000, 11 threads, FIOS, PlayGo, user service) until
`sceVideoOutOpen`, the first GPU import (expected without the GPU library). Next: M3.

**M3 in progress (2026-10-27): frames are presented through MoltenVK** (visual check pending).
Changes: the game on a 64 MiB thread and the Cocoa run loop in `main`, the window loop on the main
queue; a Metal view for the window; Vulkan portability enumeration (and `VK_DRIVER_FILES` no longer
overwritten); device without `robustBufferAccess2`/`nullDescriptor` (zero stand-in buffer for null
buffer bindings); ordinary (non-sparse) arenas for the guest range (16 GiB at 0x7000000000) and the
system window; no fragment-shader barycentrics on macOS (MoltenVK's MSL conversion rejects
`PerVertexKHR`). First start: ~23 s without frames while shaders compile.

**Open from M3** (not yet exercised with the game): the game must run off the main thread for
Cocoa/SDL (bloodborne_mac_port runs it on a 64 MiB thread and `CFRunLoopRun()` in `main`); host
heap objects the guest sees must come from `runtime_low_map` (macOS malloc lives above 1 TiB, which
the guest's 40-bit pointer packing cannot hold); the fibers use deprecated `swapcontext`; the perf
overlay's CPU/RAM figures read `/proc` (zero on macOS for now).

## Risks

- **GPU load.** ISS's particle-heavy compute work may stress MoltenVK more than Bloodborne
  did; expect M4 to be slow until M5.
- **Memory.** ~33 GB of game files and a large GPU working set: 16 GB Macs will be the hard case
  (Bloodborne's 16 GB fixes are the reference).
- **Rosetta translation of self-modifying/rewritten code:** code written through one alias and run
  from another keeps the stale translation until a protection change (learned on Bloodborne).
- **Upstream churn.** iss_pc moves fast (hundreds of PRs); a long-lived fork drifts. Hence
  "Upstream" below.
- **Rosetta's future.** Apple keeps it fully through macOS 27 only; long term, the same question
  as Bloodborne's static-recompilation research.

## Upstream

Decided (iss_pc#717): a fork, **satperm/iss_mac_port**, credited in the README and
`docs/macos/UPSTREAM.md`. macOS changes stay small and behind `#ifdef __APPLE__` where they touch
the shared core, with build and packaging in `port/scripts/macos/`, so merging iss_pc's
`developer` branch stays easy (see `docs/macos/UPSTREAM.md`). If HayatoG later wants pieces
upstream (the same core powers tlg_pc and gr2_pc), they can be offered as PRs.

## Prerequisites (owner)

- Xcode (full app, not only the Command Line Tools) with the Metal toolchain.
- A legally dumped copy with update 1.07, merged into one folder (~30 GB). The owner's copy:
  CUSA00004 (EU), APP_VER 01.07, kept outside the repository.
- About 40 GB free for Xcode plus 33 GB for the game (188 GB free on 2026-10-27).
