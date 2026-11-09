# AGENTS.md — working on the ISS macOS port

For coding agents (Codex, Claude Code, others) and the people working with them. Read this first;
`CLAUDE.md` is the long engineering diary behind it, `README.md` the player-facing page.

## What this is

inFAMOUS Second Son (PS4, CUSA00223 / CUSA00004, version 01.07) running natively on Apple Silicon
Macs. Fork of HayatoG/iss_pc. There is no game source: the PS4 `eboot.bin` (x86-64 code) runs under
Rosetta 2 inside `tlg-probe` (loader + HLE runtime, `port/src`), and its GPU command stream goes to a
native arm64 process `tlggpu-host` (`port/host`) with a shadPS4-derived Vulkan renderer (`port/gpu`)
on a patched MoltenVK 1.4.2. Work happens on branch `macos`; releases are tags `mac-vX.Y.Z`.

## Hard rules

1. **No game files, keys or dumps** in commits, issues, PRs or releases (the repository is public).
   `.gitignore` and `scripts/macos/package_app.sh` guard this; the only save that ships is
   `port/benchmark/savedata` (without the game's icon). Every contributor uses their own legally
   dumped game (`TLG_GAME_DIR`).
2. **Never run tests against a player's real save.** Use a copied user folder (`TLG_USER_DIR`), as
   `scripts/macos/diag_run.sh` and `scripts/macos/fps_ab.sh` do.
3. **No FPS regressions.** A change that lowers the frame rate at the benchmark scene or the bridge
   does not ship, even if it improves the picture, unless the owner accepts the trade in the PR.
   Measure with `fps_ab.sh` (below) and put the table in the PR.
4. **Picture changes need human eyes.** Screenshots catch some things; flicker, ghosting and missing
   smoke need someone playing. Say in the PR what was checked by eye and by whom.
5. **Experiments go behind switches**, default off (`TLG_*` environment variables, read once with
   `std::getenv`), until measured and checked; flip the default in a separate, measured commit.
6. **Never attach a debugger to, or kill, a Rosetta (x86-64) process** (`tlg-probe`): it can wedge
   Rosetta for hours. Instruments: only `xctrace record --template 'Metal System Trace' --all-processes`
   while the game runs, or `--launch` with the game's own `--timeout`.
7. Do not stage `port/gpu/third_party/fsr-vulkan` (submodule drift) or anything under `port/deps`,
   `port/out`, `port/.venv`. Stage explicit paths.
8. In zsh, `$VAR` with spaces is not split: pass lists of settings as `${=VAR}` or quote each one.

## Layout

| Path | What |
|---|---|
| `port/src` | x86-64 loader, HLE runtime, eboot hooks (`probe.c`, `iss_*.h`) |
| `port/gpu/shadps4/video_core` | renderer: `amdgpu/liverpool.cpp` (PM4 command processor), `renderer_vulkan/` (rasterizer, scheduler, runtime barriers), `texture_cache/`, `buffer_cache/` |
| `port/gpu/shadps4/shader_recompiler` | GCN to SPIR-V; `backend/spirv/emit_spirv_context_get_set.cpp` has the macOS buffer bounds |
| `port/gpu/shim` | port glue: settings (`tlg_settings.*`), window/Metal HUD (`window.cpp`), overlay, GPU init (`tlggpu.cpp`) |
| `port/host` | arm64 GPU process (`main.c`), shared rings with the game process |
| `port/launcher` | Swift launcher (`App/`, `Model/`), writes `launch.conf` (environment for `run_macos.sh`) |
| `port/scripts/macos` | build, deps, run, measure, package; `patches/` MoltenVK patches |
| `port/benchmark/savedata` | benchmark save: truck crash, the heaviest scene of the tuning runs |

## Build and run

```sh
bash port/scripts/macos/bootstrap_deps.sh                  # once: x86-64 deps + patched MoltenVK
ISS_DEPS_ARCH=arm64 bash port/scripts/macos/bootstrap_deps.sh  # once: arm64 deps (GPU process)
python3 -m venv port/.venv && port/.venv/bin/pip install iced-x86
cd port && bash build_macos.sh                             # everything (LTO; ISS_LTO=OFF: faster build, ~25% slower game)
TLG_LTO=OFF bash scripts/macos/build_gpu_arm64.sh          # only the arm64 GPU library (renderer changes, ~4 min)
bash scripts/macos/build_launcher.sh                       # Swift launcher
bash build_macos.sh --test                                 # unit tests
bash scripts/macos/make_app.sh                             # ISS.app for this checkout
```

A run without anyone at the Mac (window, frame stats every 5 s, optional screenshots / pad script):

```sh
TLG_GAME_DIR=/path/to/game SHOT_SECONDS=5 bash scripts/macos/diag_run.sh <label> 90 \
    TLG_USER_DIR=$PWD/out/<copy-of-a-user-dir> TLG_HOST_INPUT=0 [VAR=value ...]
# -> out/diag/<time>-<label>/{game.log,report.txt,memory.csv,shots/}
```

The shader cache key hashes the recompiler sources: switches read at shader compile time need a
fresh cache, and the first run after a recompiler change compiles everything (slow, discard it).

## Measuring

```sh
TLG_GAME_DIR=/path/to/game bash port/scripts/macos/fps_ab.sh -n 3 -w 1 base "" fix "TLG_MY_SWITCH=1"
```

Alternates the variants at the benchmark scene and prints the median FPS per run and per variant.
Reference on an M3 Max (0.1.5, 1080p): benchmark 45-46 FPS, bridge 58-60. The launcher's Benchmark
button does the same for players and can open a filled-in issue (label `benchmark`).

- Measure on power: on battery macOS lowers clocks (benchmark 45 -> 39 FPS).
- Background processes (`mds`, `mediaanalysisd`, `fseventsd`, `textunderstandingd`) move results by
  up to 30%: check `ps -Ao pcpu,comm -r | head` and repeat noisy pairs.
- GPU busy per channel: Metal System Trace, export `metal-gpu-intervals` (`xcrun xctrace export`).
  Per compute shader: MoltenVK `MVK_TLG_SPLIT_DISPATCH=1` (diagnostic patch, see CLAUDE.md) +
  `TLG_DRAW_LABELS=1`.

Useful switches (all logged every 5 s unless noted): `TLG_FRAME_STATS=1` (on in the harness),
`TLG_PASS_STATS=1` (what ends render passes), `TLG_BARRIER_WHY=1`, `TLG_ACB_RUNS=1` (compute-queue
dispatch runs), `TLG_QUEUE_STATS=1`, `TLG_THREAD_PROFILE_MS=5` (where game threads wait),
`TLG_OCCLUSION_LOG=2`, `TLG_DISPATCH_TIMING=1`, `TLG_DRAW_TIMING=1`, `MTL_HUD_ENABLED=1`.

## Things already found (do not rediscover)

- The game's GPU particles run on its compute queue as runs of ~95 one-workgroup dispatches with no
  sync between them; they are now unordered between the game's own sync packets
  (`TLG_ACB_UNORDERED`, default on) and MoltenVK runs dispatches concurrently
  (`patches/moltenvk-1.4.2-concurrent-dispatch.patch`, `MVK_TLG_CONCURRENT_DISPATCH=1`).
  Truck 36 -> 45 FPS with correct smoke (`TLG_UNIFIED_COMPUTE_FENCES`, default on).
- Two Metal queues do not run compute concurrently, but a small job on a second queue overtakes work
  queued on the first. `TLG_ACB_QUEUE=1` (default on in `run_macos.sh`; `=0`: one queue) puts the game's compute queue on
  a second `VkQueue`: bridge 59.8 -> 65.3 FPS, truck unchanged, compute fence latency 9.6 -> 2.4 ms
  (`TLG_FENCE_LATENCY=1`). It needs all of: an untracked alias of guest memory, fences as polled
  `VkEvent`s, compute-shader memory copies (`acb_copy.comp`), images on that queue too, and an
  `MTLFence` chain between its encoders (`patches/moltenvk-1.4.2-compute-queue.patch`). Dropping any
  of them loses the gain (numbers in CLAUDE.md); making the whole arena untracked costs FPS.
- Software buffer bounds checks in compute shaders are required (without them: garbage loops, 2x
  slower). MoltenVK costs ~2 us per dispatch without argument buffers.
- Occlusion queries are answered "hidden" (mode 5): no glare through walls, no glare at all.
- The game is not pixel-bound on an M3 Max (67% render scale: +11%); upscaling buys little there.
  FSR 3 / MetalFX flicker until the game's jitter and camera feed them: the game-side hooks exist in
  upstream iss_pc (`developer`, commits 1facb8c, ffdbdfa, e453f86, 0c58e2f; they apply here and work
  on CUSA00004), the renderer side is still to do (see CLAUDE.md, "temporal upscaling").
- Dynamic quality floor, skipping particle draws, KosmicKrisp (2x GPU time), concurrent
  graphics-queue dispatches, GPU-written fences, lossless textures, mutex spinning: tried, no gain
  or visible loss (details and numbers in CLAUDE.md).
- The GPU process window loop must yield the main dispatch queue (Metal HUD needs it).

## Priorities

0. **CPU cost per draw in `tlggpu-host` is frame time**: the game keeps one frame at the GPU and waits for it
   before submitting the next (CLAUDE.md, 2026-11-09 night), so the command processor and draw recording
   threads are on the critical path. Profile with `xctrace record --template 'Time Profiler'
   --all-processes` during a run; measure with alternating library builds and the `us/draw` frame stat.

1. Fewer main-queue command buffers (~20 a frame, ~0.5 ms GPU idle each). Tried, no gain: graphics
   waiting for the compute queue only at the game's sync points (truck 45.0 -> 43.9, bridge 64.0 -> 64.6).
2. CPU work per draw in busy streets (~7,500 draws a frame, 25-30 FPS): profile `tlggpu-host`
   (arm64, safe to sample) with a save there.
3. Launcher: automatic updates (port bloodborne_mac_port's `Updater.swift` / `Update.swift`), key
   remapping, mods window.
4. Missing HUD objective text; skipping cutscenes.

## Working unattended (hours without anyone at the Mac)

- Every wait has a deadline. Wait for the game with `scripts/macos/wait_game.sh <max s>` (exact
  process names); never `pgrep -f tlg-probe` in a loop: it matches the loop's own shell and never ends.
- One run with a summary: `scripts/macos/agent_run.sh <truck|bridge> <label> <seconds> [VAR=value ...]`;
  A/B: `fps_ab.sh`. Both are bounded by `timeout`. Run anything longer than ~8 minutes in the
  background and wait for its completion notice; never end a turn waiting on an unbounded command.
- `out/bridge-fresh` (a save on the bridge) is local, not in git; copy any save before use.
- Pushes and PRs into this repository are fine when the plan says so; releases (tags) and anything
  in other people's repositories wait for the owner.

## Working together

- Pick an open issue or a priority above and say so in the issue (or open one) before starting, so
  two agents do not do the same work. Branch `agent/<topic>` from `macos`, PR into `macos`.
- One topic per PR. Fill in the PR template: what and why, the switch name, the measurement table
  (variants x runs, Mac model, scene), what was checked by eye.
- Update this file when you learn something the next agent must know (a new rule, a dead end with its
  numbers, a changed priority), and CLAUDE.md for the long story.
- Releases: the owner merges and tags `mac-vX.Y.Z` on `macos`; `.github/workflows/macos-release.yml`
  builds `ISS-macOS-X.Y.Z.zip` (rebuilds MoltenVK with `patches/`) and publishes the release.
- Commit messages: a short summary line, then what changed, why, and the numbers. Credit agents with
  a `Co-Authored-By:` line.
