# Native arm64 GPU process (plan)

Why: at 43 FPS in the cannery the GPU is busy only 72% of the time (Metal System Trace, union of
encoder intervals); the rest is waiting for the CPU side, which records and encodes ~6000 draws a
frame as x86-64 code under Rosetta (draw recording, the Vulkan recorder, MoltenVK's queue thread).
Shaders are ~40% of GPU time with no hot shader (Shader Timeline). Running the renderer natively
(bloodborne_mac_port measured 1.75x on MoltenVK-style CPU work) leaves room up to ~60 FPS.

Design: bloodborne_mac_port's (docs/DESIGN_NATIVE_GPU_PROCESS.md there). The game, the PS4
runtime and audio stay in tlg-probe (x86-64); the GPU library runs in a native arm64 host process
that maps guest memory at the same addresses (Mach named memory entry) and talks to the game
through shared rings and messages.

| Milestone | Content | Check |
|---|---|---|
| M1 | Guest pool as a Mach named memory entry, handed to the host | unit test, game unchanged |
| M2 | arm64 deps (`ISS_DEPS_ARCH=arm64`) and libtlggpu built for arm64 | builds, unit tests |
| M3 | Host executable + game-side proxy: rings for submits/flips/events, HLE split | title screen renders |
| M4 | Page protection and fault relays, readbacks, zero-copy across processes | gameplay correct |
| M5 | Default mode, ISS.app, benchmark against single process | FPS and memory |

Interfaces (2026-10-28): the GPU library uses 29 `runtime_*` functions and exports 28 `tlggpu_*`
entry points (Bloodborne: 22 and 20).

## Later: ISS-specific ideas from shadPS4 forks (2026-10-28)

- axylpup/shadPS4 (ISS-tuned): `SHADPS4_PERF_MODE=low|office` switches to the game's own lower
  settings (+13% / +19% FPS in the heaviest city view): candidate for M1/M2 Airs. Native 4K needed
  the per-tile light list sized to the output. Under-24 GB RAM mode (-2.5 GB).
- weshayoub92/shadPS4-AYOUB1080p: patches (CUSA00004 1.00, need porting to CUSA00223 1.07):
  dynamic quality thresholds x2 (the game lowers sun shadows/resolution in busy scenes from GPU
  timing), collision-loop anti-freeze (200k iterations), bigger light list for 4K. Built in: GPU
  clock read "at console pace" so the dynamic quality keeps detail; constant buffer reuse,
  redundant bind / empty draw skipping, readback copies on a second queue.
