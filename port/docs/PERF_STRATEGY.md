# Performance strategy

How we find and remove bottlenecks from 2026-11-26 on: measurements, methods and a report any player can send.
Until now most work raised 1080p FPS on one M3 Max. Most players have an M1/M2 (often base or Pro) with
16 GB, which are GPU- and memory-bound. The goal is a method that names the limit of any frame on any Mac, and
an upper bound for any idea before we build it.

## Principles

- **Method before tools** (Brendan Gregg). Start from a question ("what limits this frame?") and a checklist of
  resources, not from whatever profiler is at hand. Avoid the anti-methods: the *streetlight* method (looking
  where a tool happens to shine: the Time Profiler showed busy threads for weeks, not the waits that set the
  frame), *random change* (tuning switches until a number moves) and *blame someone else* ("it's the Apple
  driver") without a measurement.
- **Upper bound first.** Before implementing an idea, measure its ideal: drop the pass, zero the stage's cost,
  skip the copy (picture broken is fine). Under 3% upper bound: don't build it. Example: depth store elision
  upper bound 1% (2026-11-25), closed in an hour.
- **Distributions, not averages.** Median FPS hides stutter. Report frame-time percentiles (p50, p99, p99.9),
  held frames and a frame-time heat map over the session.
- **Active benchmarking.** Profile while benchmarking, to confirm the benchmark measures what we think (the
  released 0.1.18 measured a different MoltenVK for three weeks; the CI bundle is now the thing measured).
- **One ledger.** Every experiment goes to FINDINGS with its upper bound, result and branch, so nothing is
  tried twice.

## Resources (the USE inventory)

For every resource: **U**tilization (busy time), **S**aturation (queued work, waits) and **E**rrors.

| Resource | Utilization | Saturation | Errors |
|---|---|---|---|
| P-cores / E-cores | per-thread CPU time (`thread_info`), core type | runnable but not running (System Trace) | - |
| Game main thread (guest, Rosetta) | own work share | waits on GPU labels, locks (`TLG_WAIT_PROBE`) | guest faults |
| Stage A (`tlg:DrawScan`/`DrawPrep`) | busy % per frame | queue depth to stage B | - |
| Stage B (`tlg:DrawRec`, `TexBind`) | busy % per frame | queue depth to the recorder | - |
| Vulkan recorder (`tlg:VkRecorder`) | busy % per frame | commands queued | `vkBegin...` NotReady, device lost |
| Probe -> host rings | bytes / s | ring full waits | protocol errors |
| GPU (vertex / fragment / compute) | GPU busy % (command buffer GPU start/end) | frames ahead, present waits | GPU timeouts, page faults |
| GPU passes | ms per pass, limiter (ALU / texture / memory / occupancy) | tile memory spills, pass splits | - |
| Unified memory capacity | footprint of both processes | compressor, swap, memory pressure | Jetsam, allocation failures |
| Memory bandwidth | GPU and CPU bytes (counters) | limiter = memory | - |
| Page-protection tracking | protect/unprotect calls, faults / s | fault handler time | wrong data (a missed write) |
| Shader / pipeline compiler threads | busy % | pipelines waiting, held frames | failed records |
| Caches (texture, buffer, pipeline, MSL) | size against budget | evictions, re-creations | cache errors |
| Disk | bytes read / s | read latency | I/O errors |
| Power / thermal | GPU and CPU W, frequency | thermal state, throttling | - |

Each row needs a counter in the log (below). Rows nobody measures are where the next surprise hides.

## Methods

- **USE sweep** over the table above, per scene. First pass on every new Mac or big change.
- **Workload characterization:** what the game asks for per frame: draws, render passes, dispatches,
  pipelines bound, textures uploaded, bytes copied, guest memory written. A scene that is slower may simply
  ask for more; compare per draw, not per frame.
- **Thread state analysis (TSA):** for each port thread, time on CPU, runnable, blocked on a lock, on the GPU,
  on I/O. System Trace gives it; a cheaper in-process version samples thread states (suspend + read, never
  signals: see CLAUDE.md gotchas).
- **Off-CPU analysis:** where blocked threads wait, with stacks. The 1080p frame is a serial chain; the stage
  that waits is not the stage that limits.
- **Flame graphs:** on-CPU (`time_profile.py` -> folded stacks -> SVG) and off-CPU (System Trace blocking
  stacks), per thread, for differential comparison between two builds (red = grew).
- **Critical path / causal profiling** (Coz-style): add a fixed delay to one stage (`TLG_STAGE_DELAY_*_NS`) and
  measure the FPS loss. The stage with the highest sensitivity limits the frame, whatever its busy time. Make
  it a standard sweep: every stage x every scene, 3 x 60 s each.
- **Latency analysis:** frame-time heat map over the session (x = time, y = frame-time bucket). Shows warm-up,
  thermal decline and periodic hitches (a cache save every 60 s shows as a stripe).
- **Drill-down:** frame -> stage -> function, or frame -> GPU pass -> shader -> source line (Xcode GPU capture
  via MoltenVK, `MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE`).
- **The 60-second checklist (Mac):** `top -o cpu`, `vm_stat 1`, `memory_pressure`, `footprint` of both
  processes, `powermetrics --samplers gpu_power,cpu_power,thermal` (needs root: our Mac only), the game's
  last Perf lines. Before any deeper look.
- **Static tuning:** check the configuration first: power mode, Game Mode, display refresh, render scale,
  upscaler, Low Power Mode, external display. A third of odd reports are a setting.

The performance mantras, in order of preference, with our examples:

1. *Don't do it*: passes with no target (0.1.9), dead depth stores.
2. *Do it, but don't do it again*: driver pipeline cache (0.1.19), learned jobs.
3. *Do it less*: half-resolution particles, fewer render passes, push constant dedupe.
4. *Do it later*: lazy MSL libraries (0.1.19).
5. *Do it when they're not looking*: precompile before the save loads, background pipeline warm-up.
6. *Do it concurrently*: concurrent compute dispatch, two recorders (stage B then limits).
7. *Do it cheaper*: framebuffer fetch for decals, fast shader math, cheaper wave64 lowering.

## Scene matrix

| Load | Scene | Mode | Main metric |
|---|---|---|---|
| CPU | truck, first Seattle street | 1080p uncapped | FPS, stage sensitivity |
| GPU | burning village, bridge | 1440p and 4K | GPU ms per pass, limiter |
| Traversal | Seattle run, recorded route | 60 cap, warm and cold cache | held frames, p99.9 |
| Soak | Seattle 60 min | 60 cap | footprint slope, heat map, W |
| Small Mac | the above | render scale 50-67%, memory limit as 16 GB | FPS, swap |

One script runs the matrix on the CI-built bundle (not the local build) and appends one JSON line per run
to `out/perf/history.jsonl`. Noise band: +-1% for 3 x 60 s medians; worse than that is a regression.

## Player report ("Detailed report")

Today the benchmark report has FPS and the worst frame per 5 s window. It cannot tell a GPU-bound M1 Pro from a
CPU-bound one without a round of questions. The new report gives each tester's Mac the USE table.

**Game side: one `Perf:` line per 5 s window** in the normal log (always on, cheap: counters already exist or
are atomic adds), for example:

```
Perf: fps 59.8 ft p50 16.6 p99 21.0 p999 34.2 max 41.0 held 2 | cpu main 48% drawscan 31% drawrec 62% vkrec 55% \
  compile 3% | gpu busy 71% ahead 1.4 | draws 9.8k passes 162 dispatch 1.1k pipes+ 0 | faults 1.2k/s copy 38 MB/s \
  | mem probe 2.9 GB host 4.6 GB pressure 0 swap 0 | therm 0
```

- Frame times from the flip timestamps (already in Frame stats), as percentiles.
- Per-thread CPU % from `thread_info(THREAD_BASIC_INFO)` deltas for the named port threads (both processes).
- GPU busy % from Metal command buffer `GPUStartTime`/`GPUEndTime` (union of intervals per window) in the
  host; `TLG_GPU_PROFILE` timestamps don't work on MoltenVK, these do.
- Workload counts from the renderer (draws, passes, dispatches, new pipelines).
- Page-protection faults and copied bytes from the existing Unified memory counters.
- Memory: `phys_footprint` of both processes (`task_info`), `host_statistics64` compressor/swap, the
  dispatch memory pressure level.
- Thermal state from `ProcessInfo.thermalState`. Power needs root: not in the player report.

**Launcher side: "Detailed benchmark"** (next to Benchmark): runs the benchmark scene for 60 s at the
player's settings, then 30 s at half render scale. Comparing full
and half scale is a cheap GPU-bound test any player can run: FPS nearly doubles = GPU-bound, unchanged = CPU.
It writes `report.json` (all Perf windows, settings, Mac model, chip, memory, macOS, display, power source,
Low Power Mode) plus a short text summary with an automatic verdict:

```
Verdict: GPU-bound (GPU busy 97%, half scale +88% FPS). Largest CPU stage: drawrec 41%.
Memory: 11.2 of 16 GB, pressure normal, no swap. Thermal: nominal -> fair after 4 min.
```

Privacy: no user name, paths, host name or serial; the zip is shown before it is attached to an issue.

## Order of work

1. **Perf lines** in the game log (per-thread CPU, GPU busy, percentiles, workload, memory). Check the cost:
   FPS with and without, within the noise band.
2. **Matrix script + history** on the CI bundle; **causal sweep** of all stages.
3. **Detailed benchmark** in the launcher with the verdict, then ask the M1 Pro / M2 testers for runs.
4. From the data: GPU drill-down at 4K (Xcode capture, limiter template), wave64 lowering cost (4,400
   `LowerWave64` warnings a session), render pass merging, held frames after a load (shader module release),
   16 GB simulation.
