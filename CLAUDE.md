> **Agents: read `AGENTS.md` first** (rules, build, measuring, what is already known, priorities).
> This file is the long engineering diary behind it.

# CLAUDE.md — ISS macOS port (branch `macos`)

Fork of HayatoG/iss_pc. x86-64 loader + HLE runtime (`port/src`) + shadPS4-derived Vulkan core
(`port/gpu`) running the PS4 eboot under Rosetta 2, Vulkan via MoltenVK 1.4.2 (prebuilt, no source).
Plan/progress: `docs/macos/PLAN.md`. Keep answers and tool output short.

## Build / run
- Build: `cd port && ISS_LTO=OFF bash build_macos.sh [--gpu-only|--test]` (x86-64; deps in `port/deps`).
- Game: `TLG_GAME_DIR=<game dir>`. Never run tests on the user's save;
  use a copied user dir via `TLG_USER_DIR`.
- Unattended run (no user needed):
  `cd ~/iss_mac_port && PAD_SCRIPT=port/scripts/macos/pad/graffiti_walk.txt SHOT_SECONDS=3 TLG_GAME_DIR=... bash port/scripts/macos/diag_run.sh <label> 100 TLG_USER_DIR=<copy> [VAR=val...]`
  → `port/out/diag/<time>-<label>/{report.txt,game.log,memory.csv,shots/*.jpg}`. Pad scripts: `port/scripts/macos/pad/`.
- Shader cache is keyed on the recompiler source hash; env switches read at compile time need a fresh user dir.

## Diagnostics (env)
`TLG_FRAME_STATS=1` (default in harness), `TLG_MEM_REPORT=1`, `TLG_DISPATCH_TIMING=1` (GPU ms per compute
shader), `TLG_DISPATCH_LOG=<n>` / `TLG_DISPATCH_LOG_CS=<hash>`, `TLG_DRAW_LABELS=1` + `MVK_CONFIG_DEBUG=1`
(names the hung encoder), `TLG_LOOP_CAP`, `TLG_SHOT_DIR`. Instruments is safe only as
`xctrace record --template 'Game Memory'|'Metal System Trace' --launch -- out/tlg-probe ... --timeout N`
(the game exits itself; never attach to or kill a Rosetta process: it can wedge Rosetta).

## macOS-specific fixes (don't redo)
- Guest arena = direct memory + 1 GiB (Metal wires it fully); other arenas on first use.
- Buffer loads read zero past the range the renderer bound: `PushData::buf_sizes` (macOS push
  constants, 4 KiB). MoltenVK's OpArrayLength is unreliable (too short → black faces/blotches;
  a floor let garbage through → 73 ms loops). Small dark spots on faces remain (other load types?).
- Vertex-like stages' loops capped (`TLG_MAC_VS_LOOP_CAP`, 4096): a tree-walking VS hung the GPU
  while loading the cannery area.
- No `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` (driver crash in occlusion setup); 512 active CBs.
- Rect/quad lists use auxiliary tessellation (no geometry shaders); MoltenVK gives each indirect one
  64-128 MiB scratch from a pool that never shrinks → `TLG_MAC_TESS_PER_SUBMIT=4`, `TLG_MAC_TESS_IN_FLIGHT=8`.
- Texture collector counts its own images on macOS (MoltenVK reports Metal's whole allocation).
- No Metal argument buffers; no NoContraction in fragment/compute.

## Open (2026-10-27)
- ~15-20 FPS in the first area: GPU-bound; cs `d7fe6238` (3 groups, long serial loop) takes ~28 ms per
  frame and everything runs on one queue. Likely fix: run guest async-compute work on a second queue.
  Skipping it (test only) lifts the first area 13-16 → 23-24 FPS; later areas stay ~10 FPS
  (rendering, ~5600 draws/frame). Tried without gain: loop unroll hint, 1 MiB fault window,
  720p + FSR3 (same FPS, and a GPU hang). 5-minute soak: no crash/hang, memory flat 20-22 GB.
  Cannery area with powers (2026-10-27 night): 19-34 FPS after the bounds fix. No gain:
  tess limits 8/16 (+6 GB), MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0 (helps since submit-every-N).
- Renderer v2 (`TLG_RENDERER_V2=1`, `TLG_PROBE=out/tlg-probe-v2`, env from gpu_v2/defaults.env):
  builds and reaches gameplay but 0.5-3 FPS (submissions take seconds); needs port/gpu's macOS
  fixes (tess limits, compute bounds) before it can be compared.
- Cannery interior (~12 FPS) is bound by render passes/draws, not pixels or compute: Metal trace
  fragment 499 + vertex 332 + compute 129 ms/s; ~3,200 passes/s (`TLG_PASS_STATS=1`), 2,870 ended by
  FlushBarriers (deferred-lighting depth/attachment transitions). Tried without gain: keeping the
  pass for barriers on non-attachment images (passes just end at the next state change instead),
  720p+FSR3 (+1 FPS), BP10 off, async compute in v2, STORE_OP_NONE for read-only depth, smaller staging ring.
  2026-10-28: invariant positions + no NoContraction anywhere: +10% (interior 14-15, outdoors 20-23).
  No gain: TLG_NO_MSAA=1. Mid-pass uploads (SynchronizeMemory <1 MB, ~225/s) worth ~6% at most.
  Next: Game Performance trace (shader profiler) to find which shaders own the ~38 ms/frame of
  fragment work; Bloodborne's native Metal backend only beat MoltenVK by ~2%, so do that first.
  Benchmark (`scripts/macos/bench.sh <label> [VAR=val]`, 2026-10-28 baseline): outdoor 29.0, cutscene
  20.7 (noisy ±4), interior 17.8 FPS, 17 GB. TLG_FRAMES_AHEAD=2: no gain. ~200 rect/quad-list draws/s
  go through MoltenVK tessellation (each ends a Metal pass): a few % at most. Swapchain has 3 images.
  `TLG_DRAW_TIMING=1` (draws one at a time): no heavy draw; all ~0.27 ms = submit overhead. GPU time
  is spread over ~6000 small draws/frame, 2/3 with ps 1318ddd4 (depth-only shadow draws) -> per-draw
  GPU overhead, not shader cost. Bigger FPS needs fewer/cheaper draws (batching, native encoding).
  ISS.app (`scripts/macos/make_app.sh`, Game Mode) works; Game Mode needs full screen with the user present.
  Memory: 17 GB with tess limits 1/2 (MoltenVK scratch pool was 5.2 GB). No FPS decay over 5 min idle. Next real step: pass merging / reordering.
  2026-10-28 day (interior idle, median of last 60 s): submit-every-128 + render-thread QoS 23.5;
  clang PGO (`scripts/macos/pgo_train.sh`, used when `port/pgo/tlggpu.profdata` exists) 24.0;
  MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0 (now default; vkQueueSubmit encoding was 40% of DrawRec) 27.0;
  TLG_SUBMIT_DRAWS 64 28.2 (32 same). Now: GPU ~72% busy (`ioreg -r -d 1 -c IOAccelerator`), DrawRec
  35% in readback Finish (~5 per frame, ~24% of time), 6% tess in-flight limit. 960x540: 32 FPS only.
  TLG_VBLANK_HZ=0: interior 26.7, cutscene 37 (kept 60). Sample threads with `sample <pid> 10`.
  KosmicKrisp (Mesa 26.2.5, cross-built x86-64: `scripts/macos/build_kosmickrisp.sh`,
  `TLG_VULKAN_DRIVER=kosmickrisp`): runs, correct picture (hero shadow visible), 19.2 FPS / 20 GB:
  its per-draw recording (kk_draw, render encoders) under Rosetta is the limit. Needs arm64 GPU code.
  Direct readbacks (TLG_MAC_DIRECT_READBACK, default on): ~430/432 buffer readbacks per 5 s read data
  whose GPU writer had finished, yet each drained the GPU queue (Finish). The guest arena is now in
  shared memory and read directly: interior 28.5 -> 34.3, cutscene 34 -> 41 FPS. Arena 5 GiB and
  staging ring trimming: peak 18 -> 16-17 GB. Render passes: ~250/frame, mostly tiny (median 9 us);
  skipping read->read depth barriers cut 10% of passes with no FPS change (reverted).
  TLG_DEFERRED_READBACK=1 (run_macos.sh default, as the Windows launcher): one 512x512 RGBA16F image
  (0x70f9522000) was downloaded ~330x/s with a full GPU drain each, never read by the CPU:
  interior 34 -> 43, cutscene 41 -> 50 FPS. TLG_STALE_READBACK adds nothing. zsh: "$v" is not
  word-split; pass bench.sh variables literally.
  After that (43 FPS): GPU ~93% busy (ioreg), not pixel-bound (FSR3 at 720p: 43.6), ~250 passes and
  ~6000 draws a frame. Tried without gain: TLG_VBLANK_HZ=0, TLG_FRAMES_AHEAD=2, tess in flight 4,
  stale readbacks, keeping passes open across barriers for other images / read-only depth (passes
  -18%, same FPS; reverted). Remaining splits are real feedback (General->General colour, attached
  colour sampled). Per-shader GPU time needs Instruments' Shader Timeline (CLI template has none).
  Next for 60: fewer draws (shadow depth draws are 2/3), MoltenVK per-draw cost, arm64 GPU process.
  bench.sh waits for the nominal thermal state (scripts/macos/thermal_state.swift).
  Also tried, no gain: no fragment stage for depth-only pipelines (shadows), TLG_GPU_PROFILE (MoltenVK
  timestamps only at submit boundaries: useless), no empty passes exist (pass log work=0 is an artifact).
  GPU runs at maximum performance state. Memory (exact: diag_run memory.csv footprint_mib): no
  BDA page table/fault buffer without TLG_DIRECT_MEMORY_ACCESS (-0.5 GB), now ~15.3-16.0 GB: arena
  5 GB (shared, copy of guest memory) + guest 4.5 + private GPU 3.2 + malloc 1.2 + staging 0.7.
  For 16 GB Macs the next step is zero-copy (import guest memory as the arena, -5 GB) which needs
  EOP fences signalled at GPU completion (bloodborne_mac_port FINDINGS: reuse hazard).
  DONE 2026-10-28: zero-copy = the existing TLG_UNIFIED_MEMORY (Castlesea ISS port; fences deferred to
  GPU completion) over all direct memory on macOS (0x7000000000+0x13bff0000; maps 1152/512/3392 MiB,
  heap at 0x7048000000), no guest arena then. Default on Macs <= 24 GB RAM. Cannery 7.4 GB at 37 FPS
  (vs 15.8 GB / 43). Needed: a non-sparse stand-in buffer, and copying tessellated indirect draws'
  index ranges into staging (MoltenVK sized its scratch from the index buffer's remaining length:
  four 4 GiB buffers; found with xctrace Game Memory --launch, allocation backtraces).
  Shader Timeline capture (user, Instruments GUI): shaders are only ~40% of GPU time, no hot shader;
  FPS is capped by CPU-side encoding (MoltenVK queue thread + recorder) under Rosetta -> arm64 process.
- Native render size is 1920x1080; lower sizes need TLG_UPSCALER (fsr3/taa) + TLG_RENDER_RES.
- Small black patches near faces in cutscenes (the hero, his brother).
- Wrap long unattended runs in `caffeinate -dims` (lid closed without a display = sleep).
- Windows launcher defaults (`ISS_*`) make the Mac ~1 FPS: don't adopt blindly.

## Native arm64 GPU process (branch gpu-process, 2026-10-28)
docs/macos/GPU_PROCESS_PLAN.md. M1 (shared guest memory entry) and M2 (arm64 GPU library,
portable SRT walker) are on macos. M3 on gpu-process: TLG_GPU_PROCESS=1 runs the renderer in
out/tlggpu-host (from bloodborne_mac_port's host/, src/gpu_process.c, shim/tlg_split.*; VideoOut/
GnmDriver remote branches). Cannery 40-44 FPS (single 42.5), zero-copy 40.8 (single 37).
In split mode the GPU is busy ~67% (Metal trace union); the game's main thread is ~72% busy, 11%
of it relaying GPU page faults to the host. 2026-10-29: UI/input calls forwarded (TLGGPU_REC_UI_CALL:
save dialogs, toasts, notices, graffiti guide, text entry, mouse, pad HUD, input settings; keyboard
via tlggpu_split_keyboard); diag_run counts the host's footprint: split 16.2 GB / zero-copy 7.7 GB
(single 16 / 7.4). Route clean. TODO before default: Karma switcher and native-resolution status in
the host (game-side state), cheaper fault relays (11% of the game's main thread), a real save /
load through the dialogs, ISS.app, a long gameplay session.
GpuIdle/SubmitDone: calls are numbered; the command processor takes the flag with its number
before the flush (lost SubmitDone hung the game).
2026-10-29: MoltenVK prefill (MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2) no longer crashes (occlusion
query segments span whole passes). Interleaved A/B, single process: prefill 39.6-40.0 vs off 41.3-43.0
(keep off); split mode one pair 46.2 vs 43.2 (re-test with more runs). Frames sit at the 16.7 ms
vblank boundary (median 16.67, p99 33.3): single runs vary +-4 FPS, compare means of several runs.
Next for 60 on weaker Macs: the game's own lower settings (axylpup PERF_MODE, weshayoub dynamic
quality thresholds), GR2fork's readback lane (separate queue) as a later idea.
2026-10-29 evening (quiet Mac): partial descriptor pushes on (+2.5%); vblank uncapped by default
(44 FPS, frame-time spread 2.5 ms instead of 16.7/33.3 judder); ISS patches from axylpup/shadPS4
opt-in (TLG_PERF_MODE=low +5%, TLG_PARTICLE_LATE_READ no gain here). Uncapped frame time in the
cannery is ~22.7 ms: 60 needs ~26% less on the critical path. Slow frames wait ~23 ms on GPU ticks
(TLG_PERF_DIAG=1 "Wait profile": tessellation in-flight cap vk_rasterizer.cpp:1850 90 ms/5 s,
readback writer waits buffer_cache.cpp:654 58 ms/5 s). Raising the tess cap: +1 FPS for +5 GB
(MoltenVK scratch for non-indexed tess draws is still large: next idea, bound it like indices).
Benchmarks: run with the Mac idle (user's activity changed results by +-4 FPS).

**Launcher (2026-10-29):** `port/launcher` (Swift, adapted from bloodborne_mac_port's, no updater,
benchmark or mods) is ISS.app's main executable (`make_app.sh` builds it via `build_launcher.sh`;
Info.plist `ISSRepo`). Settings in `port/launch.conf` (TLG_* lines, passed to run_macos.sh; the
launcher's own env wins); `TLG_SHOW_LAUNCHER=0` starts the game at once (hold Option to show the
window); session logs + crash zips in `~/Library/Logs/ISS`. Test runs: `open -n --env
TLG_LAUNCH_CONF=<scratch conf> --env TLG_USER_DIR=<copy> --env TLG_TIMEOUT=60 ISS.app`.

**GPU particles (2026-10-29):** smoke/sparks/neon/rain are GPU-simulated; the game reads survivor
counts back each frame. With the old default (Relaxed readbacks, copies) it never saw them: powers
had no smoke in gameplay (cutscenes fine). Now `TLG_PARTICLES=shared` (run_macos.sh default):
unified memory + TLG_PARTICLE_LATE_READ (+ LIGHT_ONE_SUBMIT); smoke-shot tutorial M3 Max 50 FPS,
6.8 GB. `precise` (TLG_READBACKS=2, Windows default): 36 FPS and intermittent hang at the intro
(swapchain acquire fence never signals, guest main thread spinning at guest+0x53b340; also with
TLG_WRITE_MERGE=1). `off`: 62 FPS without GPU particles. Earlier FPS figures were measured without
GPU particles. Quick test save: `port/out/smoke-save` (smoke-shot tutorial),
`scripts/macos/pad/smoke_quick.txt` (fires from 40 s). Relaxed mode disables the readback tricks
(deferred readback, write merge, lagged reads) entirely: they only act with TLG_READBACKS=2.

**Performance with GPU particles (2026-10-29, smoke-shot tutorial, M3 Max ~55 FPS, 17.7 ms):**
not GPU-bound (TLG_PERF_MODE=low/office, LOAD_BOUNDS=compute: no change). Metal System Trace:
GPU busy 74% (union), idle in ~1.5 gaps >1 ms a frame. `TLG_THREAD_PROFILE_MS=3` (new, samples
every thread, 10 s summary): main thread 80% in guest+0x53b340 = usleep(<=500 us) rdtsc spin,
called from guest 0x1695e0: `while (labels[ctx->idx] < ctx->target) usleep(50)` = waiting for a
GPU fence. With shared memory those fences are written at GPU completion (correct; writing them
early hung the GPU: the game reuses memory still in use). So the frame is a CPU<->GPU chain.
No gain: TLG_MAC_KEEP_PASS=1 (in-pass barriers keep the pass, opt-in), TLG_UNIFIED_FENCE_WAITS=1
(opt-in), TLG_SUBMIT_DRAWS 16/32 (worse), TLG_FRAMES_AHEAD 2 (same) / 0 (worse), queries in pass.
Diagnostics: TLG_PASS_STATS (now also same-attachment restarts + barrier kinds), TLG_FILL_STATS.
Startup crash ~1 in 7 runs, before any frame: objc_msgSend in Metal -[_MTLCommandBuffer
didScheduleWithStartTime:...] (MoltenVK's scheduled handler; predates the particle changes).
Next idea: shorten fence-to-guest latency (completion notification -> label write) and the
time from the guest's submit to the first GPU work.

**GPU hang in the burning building (2026-10-29, first bio-terrorist chase; save in
port/out/chase-save, reproduces after ~2.5 min standing, no input):** happens whenever (nearly) all
particle systems are alive: with TLG_PARTICLE_LATE_READ=1 or with TLG_UNIFIED_COMPUTE_FENCES=1
(compute RELEASE_MEM deferred to GPU completion, which makes the particle counts correct without
the late read: smoke-shot tutorial 253 vs 310 vs 135 indirect particle draws/s). With all particles
the scene issues ~2550 indirect rect-list draws/s (~120 a frame, 21 FPS), each through MoltenVK's
tessellation emulation. Defaults now: late read off, compute fences early (fewer particles, no
hang, fire scene ~49 FPS). Real fix: rect lists without MoltenVK tessellation (expand in the vertex
stage or a batched compute pass), then re-enable TLG_UNIFIED_COMPUTE_FENCES. Not a compute shader
loop: TLG_MAC_CS_LOOP_CAP (default 1M back-edges) did not prevent it.

**2026-10-30 night: broken frames, particles, startup hang.**
- Startup hang (~1 in 5: guest in sceGnmSubmitAndFlip -> WaitGpuIdle forever, GPU thread idle):
  IrqC inserted into its unordered_map from several threads without a lock and lost a GpuIdle.
  Fixed in core/platform.h (Context() under a mutex): 6/6 starts OK.
- Bridge after the car crash (save port/out/bridge-save, scripts/macos/pad/camera_sweep.txt):
  whole frames broken (scenery gone, black blocks; ~5 of 24 shots) whenever GPU particles draw,
  shared or copied memory; none with copied memory and no GPU particles (Relaxed). Suspect:
  MoltenVK's tessellation emulation of rect lists (also the burning-building hang). Real fix:
  rect lists without MoltenVK tessellation. Defaults meanwhile: TLG_PARTICLES=off above 24 GB,
  shared at 24 GB or less; precise = all particles (Precise readbacks + TLG_WRITE_MERGE +
  TLG_NOWAIT_WRITES=43e1fb,43e23a,43e26e,43e280,43e286; smoke tutorial 34 FPS, fire 50, bridge 42-52).
- Rejected: lagged reads 1717c0 (exposure) freeze loading on the smoke save; 3f3eff (particle
  count) loses most particles. TLG_FAULT_PROFILE=1 now works on macOS (top waits by guest site).
- CONFIRMED (2026-10-30): TLG_PARTICLES=precise + TLG_MAC_SKIP_RECT_INDIRECT=1 (drops indirect
  rect/quad-list draws, diagnostic) -> 0 broken frames in 24 on the bridge sweep (5/24 with them).
  The indirect rect-list draws through MoltenVK's tessellation emulation (aux TCS/TES,
  vk_graphics_pipeline.cpp AuxShaderType::RectListTCS, emit_spirv_quad_rect.cpp) break frames.
  Next session: draw rect lists without host tessellation. Each rect needs the 3 corners' outputs
  for the 4th (v1+v2-v0), so a per-vertex VS cannot do it alone. Options: (a) run the translated
  VS as a function 3x in a wrapper VS that draws 6 vertices a rect (needs vertex fetch by index,
  check how the fetch shader reads attributes); (b) a compute pre-pass that runs the VS outputs
  into a buffer, then a pass-through VS; (c) the doubled triangle v0, v0+2(v1-v0), v0+2(v2-v0)
  (attributes are affine, so they come out right) plus a fragment discard outside the rect,
  but that still needs v0's outputs at the other two vertices. Indirect args are GPU-written:
  rewrite them with a small compute shader (vertexCount/3*6), batched per submission.
- STEP 1 DONE (2026-10-30, TLG_RECT_INFO=1, smoke-shot tutorial, all particles): the particle
  pipelines are QUAD lists, not rect lists (6 pipelines: 2 read buffers by VertexId with no fetch
  shader = GPU particles; 4 use fetch-shader attributes). A quad has all 4 vertices, so no derived
  vertex: draw quad lists as triangle lists with an index pattern (0,1,2, 0,2,3 per quad; same
  winding; gl_VertexIndex stays the original id, so VertexId-fetching shaders work unchanged).
  Plan: topology TriangleList and no aux TCS/TES for quad lists (liverpool_to_vk.cpp PrimitiveType,
  vk_graphics_pipeline.cpp); direct non-indexed: a static pattern buffer, drawIndexed(count/4*6,
  vertexOffset = firstVertex); indirect non-indexed: a compute pass rewrites the GPU-written args
  {vertexCount, instanceCount, firstVertex, firstInstance} into DrawIndexedIndirect args with the
  pattern buffer; indexed quad lists: keep tessellation at first (check if they occur). Check the
  bridge and the burning building with TLG_RECT_INFO too (rect lists there?).
- STEP 4 DONE (2026-10-30): bridge and burning building (all particles) also use only quad lists,
  same 4 vertex shader kinds; no rect lists in any test scene. Indirect quad-list draws: bridge
  ~2000/s, burning building ~1840/s (~40 a frame), all through MoltenVK tessellation now: the
  quad-list index path should also raise FPS. Remaining: steps 1-3 (pipeline, direct pattern
  buffer, indirect args rewrite by compute) and 5 (default on + verification), ~1.5 h.
- STEPS 1-2 DONE (2026-10-30, opt-in TLG_MAC_QUAD_INDEX=1): quad-list pipelines are triangle
  lists without aux TCS/TES (vertex shader compiled without tess_emulated_primitive); direct
  non-indexed quad draws bind a pattern index buffer (Rasterizer::QuadIndexBuffer, 2^18 quads)
  and drawIndexed(quads*6, vertexOffset = first vertex). Smoke tutorial: runs, no errors, picture
  clean, ~75 direct quad draws/s; no indexed quad lists seen (dropped and counted if any). Indirect
  quad draws (~226/s there) are DROPPED in this mode until step 3 (compute rewrite of the args into
  DrawIndexedIndirect with the pattern buffer, vertexOffset = firstVertex).
- STEP 3 (2026-10-30 night, opt-in TLG_MAC_QUAD_INDEX=1): non-indexed indirect quad lists draw
  through host_shaders/quad_indirect.comp (records rewritten into DrawIndexedIndirect for the
  pattern; Rasterizer::QuadIndirectArgs, one dispatch per draw, outside the pass). Smoke tutorial:
  all particles (~215 indirect/s), clean, 35.6 FPS. Bridge: INDEXED indirect quad lists ~3000/s
  (dropped in this mode; need index expansion), 26-30 FPS (per-draw dispatch + pass end + barriers:
  batch per submission), and STILL ~4/24 broken frames although no quad draw uses tessellation.
  So MoltenVK's tessellation is not the whole cause: the non-indexed indirect particle draws break
  frames by their content (positions/counts?). Next: find the breaking draw (frame capture of a
  broken frame, TLG_FRAME_DUMP_TRIGGER / per-pipeline skip by vs hash), check the particle data the
  CPU writes (no-wait writes, write merge) vs Precise alone.
- FIXED (2026-10-31): the bridge's broken frames were the vertex-stage LOAD bounds
  (TLG_MAC_LOAD_BOUNDS) zeroing data of particle vertex shader 0xbbd2d927 (found with
  TLG_MAC_SKIP_VS). Loads are now bounded in compute shaders only, stores everywhere (meta version
  12). All particles + clean frames: bridge 53-65 FPS (0/24 broken), burning building 47 and no
  hang, smoke tutorial 31. Defaults: TLG_PARTICLES=precise above 24 GB, shared at or below (bridge
  clean, 42-51; fewer particles). Shared + TLG_UNIFIED_COMPUTE_FENCES=1 still reads inflated counts
  (fire scene 2559 indirect quad draws/s vs 169 precise, 21 FPS): stays opt-in.
- CORRECTION (2026-10-31): several runs passed settings through an unquoted $A in zsh, which does
  not word-split: TLG_PARTICLES got the whole string and fell to "off". Invalid: the bridge results
  of TLG_MAC_SKIP_VS, LOAD_BOUNDS=compute, "fix" (53-65 FPS) and ch-fix2. Valid: precise on the
  bridge ~16-17 FPS with ~2/24 broken frames (live run); shared after the bounds change 0/24,
  42-51 FPS. Default is now shared on every Mac. Always type VAR=val arguments out (or use arrays).
- 2026-10-31 later: live default-shared bridge run had 5/24 broken frames (the clean shared run
  only missed the problem direction). Default back to TLG_PARTICLES=off. Broken frames happen
  whenever GPU particles draw (shared or precise); off is clean (copies2: 0/24). Next session:
  find the breaking draw with settings typed out (TLG_MAC_SKIP_VS per vertex shader, particles on).
- 2026-10-31 (settings typed out, verified in each log): shared mode, bridge sweep: skip VS
  0x332f3508 -> 0/24 broken; skip 0xbbd2d927 -> 8/24. CULPRIT: particle VS 0x332f3508 (VertexId,
  8 buffers, indirect, non-indexed quad list). Its records look sane on the CPU (1 record, 72-1336
  vertices, 1 instance); its particle data alternates between 0x6d558000 / 0x6f358000 (V# in user
  data 0-3, 0x20000 records). TLG_FULL_BARRIERS=1 does NOT fix it (3/24): not a missing GPU
  barrier. Next: what it draws (render target, blend) and its buffers' contents in a broken frame
  (TLG_MAC_INDIRECT_LOG=<vs> logs records + user data; TLG_FRAME_DUMP_TRIGGER for the targets).
  Also reported: part of the HUD (the task/objective text) missing.
- 2026-10-31 CORRECTION 2: the bridge's broken frames happen with GPU particles OFF too (br-A:
  5/24, sk1: 1/24, both particles off), matching the user's first report; the per-shader "skip ->
  0/24" results were chance (24 one-second shots are too few). Not a particle bug. Default back to
  off, no shader skipped. Next: an in-engine broken-frame detector (NaN / black-block count on the
  final image per frame) + automatic frame record on detection, then fix the first bad pass.
- Broken-frame detector: port/scripts/macos/broken_frames.py (upper-third brightness > 185 of the
  1-second shots). All ~28 bridge runs re-scored (~138 shots each): every setting has broken
  frames; particles off ~1-5 (~2%), shared/precise ~3-17 (~5-7%); no switch tried (full barriers,
  shader skips, load bounds, RFL_ALL, queries) stands out from the run-to-run noise. Intermittent,
  ~8x8-tile blocks: suspect an LDS race in 64-thread compute workgroups split into two 32-lane
  host subgroups (the barrier pass inserts no barriers in divergent flow); upstream lists the same
  "flickering fog/water/fire and a fine dark grid" as open on Windows/Linux. Next: per-dispatch
  identification (capture a broken frame's compute passes), then barriers / LDS fix there.

- 2026-10-31: the LDS barrier pass never runs on MoltenVK (`needs_lds_barriers` is NVIDIA/KosmicKrisp
  only), so TLG_LDS_* do nothing on the Mac. Forcing it on (all groups) once hung the GPU at the intro
  video; per group (64 only / +multi / +small) it ran but did NOT fix the bridge broken frames
  (13.7% / 10.1% / 3.6% vs 2-12% baseline with particles=shared: the detector rate is very noisy per
  run). LDS race hypothesis dropped; switch reverted.
- 2026-10-31: broken_frames.py also flags the bright startup logo (shots 6-7) and the level
  fade-in (~shot 35-37): ignore those. Feedback loops exist on the bridge (6 targets incl. RT2
  0x706bd70000; MoltenVK lacks VK_EXT_attachment_feedback_loop_layout), but ending the pass before
  each feedback draw (+ full barrier) did NOT fix it (fbon3 shot 133: blocks on the wet road) and
  one run hit "Unknown data_format=6" (new). Reverted. Blocks sit on the wet road (reflections):
  next suspect = a tiled compute pass (SSR / tile classification) and its DispatchIndirect args.
- 2026-10-31 (bridge, cont.): no indirect dispatches on the bridge (only indirect draws 332f3508 and
  3ab1ca58; skipping 3ab1ca58 doesn't help). TLG_OCCLUSION_MODE=2 (all visible) doesn't help.
  Broken shots repeat every 6 s = same phase of camera_sweep.txt (deterministic, view-dependent).
  Full-size look (occ2a shot 59 vs 58): the broken frame is NOT missing objects: the whole image
  is a blurry reflection-like render (wet road, mirrored lights, fog) with blocks, HUD/minimap fine.
  => the final pass samples the wrong image: texture-cache aliasing at a reused guest address
  (0x706bd70000 is RT2, then the LDR scene). Next: log the image the final pass (ps eac4ed47 /
  529286b5) binds per frame, flag frames where it differs.
- 2026-10-31: intro-video freeze (~1 start in 5 today) = Swapchain::AcquireNextImage waiting forever
  on the acquire fence (MoltenVK never returns a drawable); flip (IrqC -> GetRenderFrame) and
  Liverpool (OnSubmit -> DrainDrawPipe) wait behind it. Now a 1 s bounded wait, then the
  swapchain is rebuilt ("GPU: swapchain image not returned within 1 s"). 6 test starts: no freeze,
  0 rebuilds (not reproduced yet), 2 startup crashes (use-after-free: AGX setTexture in
  MVKCmdDispatch encode; objc_msgSend in didSchedule) = the known startup crash, now ~1 in 3.
  TLG_SHOT_SECONDS accepts fractions (0.2 = 5 shots/s; min 0.05).
- 2026-10-31: startup crash fixed by MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES=1 (run_macos.sh default):
  without it 2/6 starts crashed (use-after-free in MoltenVK encode / didSchedule), with it 14/14
  clean; bridge FPS unchanged (~60-65, particles off, M3 Max). Root cause (renderer destroying an
  image view still referenced by a recorded command buffer) still to find; the check hides it.
- 2026-10-31 CORRECTION: the live check does not cover the didSchedule variant (objc_msgSend in
  -[_MTLCommandBuffer didScheduleWithStartTime:...]): it crashed 2 runs in a row with 5 shots/s
  (TLG_SHOT_SECONDS=0.2) + TLG_ADDR_TEX_LOG, then 0 in 6 plain starts and 0 in a 150 s 1 shot/s run.
  The AGX setTexture variant has not been seen since. TLG_ADDR_TEX_LOG=706bd70000 (with the
  texture-set memo off, toggle bit 1<<45): every pass reads image 9 there all the time, also in
  broken frames: no aliasing swap. 5 shots/s: a bad view lasts 0.4-0.6 s; opaque geometry is not
  drawn at all (hair, fog, reflections, HUD remain) => suspect the game's visibility culling fed
  by stale GPU readbacks (relaxed readbacks / deferred image readbacks).
- 2026-10-31: readbacks are NOT the bridge cause: TLG_READBACKS=2 -> 5 real broken shots,
  TLG_DEFERRED_READBACK=0 -> 7 / 21 (and slower). Startup crash variants (didSchedule / Block_copy
  in MTLDispatchListAppendBlock / AGX setTexture) all hit right after "SystemService: splash screen
  hidden" (our hide_splash is a no-op: it is only the moment the game starts loading for real);
  no swapchain recreation involved. Rate with the live check still ~1 in 6-8.
- 2026-10-31 evening: START-UP CRASH = MoltenVK asynchronous submits. MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1
  is now the run_macos.sh default (0/4 crashes vs 3/4 at 2 shots/s, plus 0 in all later runs).
  Bridge: TLG_DRAW_COUNT_LOG=1 (per-flip draws, targets of >3000-draw frames, "GPU shot N at" times)
  shows the broken frames are HEAVY frames: ~3,750 draws of vs beb7156 / ps 6353e48b into the HDR
  scene target 0x70fcdf0000 (normal ~270) and the G-buffer pass (5 cb at 0x70f9e10000) doubles.
  Heavy frames ~7% = the broken rate. TLG_SKIP_DRAW_PS=6353e48b (new diagnostic) does NOT remove the
  broken frames (5 and 3 hits). Next: what else changes in heavy frames (G-buffer draws x2:
  which shaders), compare with Windows iss_pc on the same save.
- 2026-10-31 night: TLG_CAPTURE_HEAVY=1 (+ TLG_DIAG_IMAGES=1, TLG_DIAG_DIR, TLG_CAPTURE_DIR) captures a
  normal frame, then the frame after a heavy one, with every pass's targets (.raw; convert with
  scratchpad raw2jpg.py logic: f37 RGBA8, f91 RGBA16 unorm, f97 RGBA16F). Result: in the broken frame
  the G-buffer (70f9e1 RT0, 70fa61 normals, 706bd7 RT2) and the lighting compute ce1675e0 outputs
  (70fbe0 diffuse) all contain the bridge and the character, same as in a normal frame. The HDR
  target 70fcdf only ever holds sky/fog/water/forward stuff (in both captures), and 706bd7 late in
  the frame looks "broken" in BOTH captures; the display buffer of the normal capture is fine. So
  the opaque scene is merged into the final image in a later pass (not yet identified), and the
  capture spans more than one frame (display buffer 706cd6 shows a different camera angle).
  Next: find the pass that merges the lit opaque scene (reads 70fbe0) and dump its inputs/outputs.
  Heavy frames = huge forward pass (vs beb7156 / ps 6353e48b, 1.4k-5.9k draws) facing the sea.
- 2026-10-31 late: the opaque scene reaches the screen only through pass #79 (material pass: ~300
  draws normally, 1.4k-5.9k when facing the sea; vs b94242a8..., color 70fcdf HDR, depth 706b20
  read-only + sampled, stencil test on, samples the lighting 70fbe0/70fdde). Game depth funcs:
  Always, NotEqual (read-only), GreaterEqual (reversed Z); EQUAL is never used. Not the cause:
  TLG_SPIRV_PRECISE=vertex, TLG_MAC_DEPTH_EQUAL=lequal (no EQUAL anyway), TLG_MAC_STENCIL_OFF=1.
  TLG_MAC_DEPTH_EQUAL=always (read-only GEQUAL -> ALWAYS, diagnostic) brings the bridge and the
  character back in the broken phase (overexposed: light volumes pass too) => in broken frames the
  material pass FAILS ITS DEPTH TEST. Next: what is in depth 706b20 when #79 runs in a broken
  frame (dump depth before/after #79; feedback: 706b20 is attachment and sampled in #79).
- 2026-10-31 night 2: other repos: iss_pc #215 (Windows RTX 4070, open, no fix) = same family
  (lit scene drops out for single frames, particles/HUD intact; reporter ruled out the port's
  additions, suspects depth/stencil). shadPS4 has no matching fix (#4814/#4869 = readbacks;
  #5246 = RTZ rounding on NVIDIA); shadPS4-Infamous-FixBuild has no depth fix. TLG_DIAG_DEPTH=1
  dumps depth in captures: 706b20 is filled by the pre-pass + G-buffer, ZERO at lighting
  (ce1675e0) and rebuilt by a second pre-pass before the material pass, in normal frames too
  (by design, not the bug). Decal passes #31-#36 attach depth read-only AND sample it (feedback);
  TLG_MAC_DEPTH_FEEDBACK=detach: run 1 had no broken frames, run 2 had them again -> not the cause.
  Next: an in-engine broken-frame trigger (e.g. material pass draws vs samples passed via an
  occlusion query on #79) so a capture lands exactly on a broken frame.
- 2026-10-31 night 3: TLG_CAPTURE_BROKEN=1 (with shots) captures the frame after a shot that
  looks broken: confirmed broken frame (shot 40). In it the second depth pre-pass is normal (22%
  empty, same as good frames) but the material pass output (HDR 70fcdf at p030) has no scenery at
  all, only water/sky/hair. TLG_EOP_FINISH=1 (GPU finishes before every EOP fence; ~33 FPS):
  broken frames REMAIN (no blocks, scene still missing) -> not CPU/GPU timing or constant reuse.
  Caveat: the earlier TLG_MAC_DEPTH_EQUAL=always and =detach results came from single runs whose
  camera phase may have missed the broken view; treat them as unproven.
  Next: per-draw check inside #79 in a captured broken frame (vertex positions / draw count /
  which draws pass), e.g. dump #79's depth-only output or count samples with an occlusion query.
- 2026-10-31 night 4: TLG_MAC_DEPTH_EQUAL=material (read-only GEQUAL -> ALWAYS only in passes into
  HDR 70fcdf with stencil on): broken frames REMAIN (2 runs), scenery still missing, scattered
  fragments appear -> NOT a depth-test failure. Broken capture (TLG_CAPTURE_BROKEN) vs others:
  the broken frame's G-buffer pass has 1090 draws led by vs ca1ea758/9ef8490f (normal: ~500 led by
  40b18d54) and a 5,925-draw forward pass (vs 44248dd8, no stencil, HDR) -> the GAME submits a
  different, much larger object set in broken frames (as if its camera/LOD/culling were elsewhere).
  Next: compare the camera constants (VS user data / constant buffer of the G-buffer pass) between a
  broken and a good frame; check guest-side culling inputs (GPU readbacks the game consumes).
- 2026-10-31 FIXED: bridge broken frames. Cause: ISS fast-clears the HTILE of depth 0x706b200000,
  then draws a full-screen colour pass (#47, 3 colour targets, samples that depth); the texture
  cache's IsMetaCleared turned it into a pass-wide load-op CLEAR, wiping depth before the lighting
  compute ce1675e0, which then lit every pixel as sky/fog (scenery gone, hair/water/HUD left, 8x8
  blocks where the timing let real depth through; in capture mode every frame). Fix
  (vk_rasterizer.cpp, default): register/HTILE depth clears become load-op clears only in
  depth-only passes; TLG_DEPTH_CLEAR_COLOR=1 restores the old behaviour. Bridge: 0 broken frames in
  4 runs (shared and off). Likely also iss_pc #215 (Windows grey flashes): worth upstreaming.
  Sync submits cost ~25% FPS -> default back to async (start-up crash returns, ~1 in 6-8).
- 2026-10-31 night: PARTICLES ON BY DEFAULT: TLG_PARTICLES=shared + TLG_PARTICLE_LATE_READ=1 +
  LIGHT_ONE_SUBMIT=1 (the late read is what makes smoke/rain/neon appear; without it shared mode
  showed no smoke). No hang in the burning building in 240 s (the old hang predates the day's
  fixes). FPS: bridge 28-37 (clean), smoke tutorial 44-54, burning building 21-25, GPU-bound
  (stall frames: ~28 of 45 ms waiting on GPU ticks; no errors in the logs).
  FPS lever found: TLG_MAC_QUAD_INDEX=1 (quad lists without MoltenVK tessellation): burning
  building 28-42 FPS, stalls 423 -> 154, memory 9.1 -> 6.4 GB, picture matches; smoke tutorial ~52.
  BUT it drops indexed indirect quad-list draws (~3,200/s in the fire, ~200/s in the smoke scene).
  Next: expand indexed quad lists (index rewrite in quad_indirect.comp or a CPU/GPU index
  expansion), then make TLG_MAC_QUAD_INDEX the default.
- 2026-10-31 night: TLG_MAC_QUAD_INDEX=1 is the run_macos.sh default. Indexed indirect quad lists
  (were dropped: ~3,200/s in the fire) are now expanded on the GPU: host_shaders/
  quad_index_expand.comp writes each record's quads as triangles into its slot of a staging index
  buffer (<= 64 MiB) and rewrites the records (Rasterizer::QuadIndexedIndirect); nothing dropped.
  Burning building 21-25 -> 30-34 FPS (stall frames 423 -> 177, 9.1 -> 6.5 GB), bridge 28-37 ->
  35-43 (clean), smoke tutorial 46-50 (same), pictures match.
  Start-up crash stack seen once in full: objc exception in MVKPresentableSwapchainImage::
  presentCAMetalDrawable from -[_MTLCommandBuffer didSchedule...] (freed drawable/object), no
  swapchain rebuild involved. MVK_CONFIG_PRESENT_WITH_COMMAND_BUFFER=0 did not avoid it (3/4).
- 2026-10-31 night (late): start-up crash: vkSetMoltenVKConfigurationMVK changes nothing at run
  time (config fixed at device creation; sync-at-start 2/4 crashes like baseline);
  TLG_PRESENT_ACQUIRE_FENCE=0 no change (2/4); MoltenVK caps swapchain images at 3. Workaround in
  the launcher (72dadaa): a crash within 2 min whose fault frames are MoltenVK's Metal callbacks is
  retried automatically, twice at most. Stress test: scratchpad stress.sh (4 runs, 2 shots/s, ~50%).
  FPS (burning building, quad index on, 27-31 FPS): ~7,000 render passes/s; QuadIndexedIndirect's
  compute ends a pass ~2,900/s (one dispatch per indexed quad draw) and FlushBarriers ~3,600/s.
  Next for FPS: batch the indexed-quad expansions (defer runs of quad draws, record their computes
  before the pass), or pull indices in the vertex shader (no compute at all).
- 2026-10-31 late night: START-UP CRASH FIXED by a patched MoltenVK (MoltenVK issue #2791:
  MVKPresentableSwapchainImage::_mtlDrawable released from two threads without a lock).
  scripts/macos/build_moltenvk.sh builds v1.4.2 + scripts/macos/patches/moltenvk-1.4.2-drawable-race.patch
  (lock around _mtlDrawable, release outside it, keep-alive retain taken with the read) and installs
  it over deps/x86_64/lib (original kept as .orig-1.4.2); bootstrap_deps.sh runs it when Xcode exists.
  Stress test (2 shots/s, was ~50% crashes): 0/8, no "presented once" warnings, same FPS.
  MoltenVK itself is deprecated (2026-10-13; Khronos recommends KosmicKrisp on Apple Silicon).
- 2026-10-31 late night: ISS's indexed indirect quad draws are single quads (1 record, 4-index
  buffer, ~3,000/s in the fire). They now draw as a triangle strip a,b,d,c with the game's own
  record (dynamic primitive topology on quad-index pipelines; indices read on the CPU into a
  16-byte upload slice): no expansion compute, no pass break. Fire scene 30-34 -> 32-39 FPS,
  passes ended by QuadIndexedIndirect 2,900/s -> 0; bridge clean 33-40. Larger ones still use
  QuadIndexedIndirect. Remaining pass enders: FlushBarriers ~4,800/s (layout transitions).
- 2026-11-01 (60 FPS hunt, fire scene, median ~36): main thread ~97% CPU spinning on GPU fences
  (~23 deferred fences/frame, each submitted at once: ~54 submissions/frame); MoltenVK/GPU threads
  ~30-35%. No gain: MVK_CONFIG_FAST_MATH_ENABLED=1 (+2, 37.8), MVK_CONFIG_USE_MTLHEAP=1 (worse),
  TLG_SUBMIT_DRAWS=256 (37.6), holding fences until the recording thread idles (submissions stay
  ~53: the guest waits on almost every fence at once), spinning on the timeline counter instead
  of vkWaitSemaphores (33.6). Submitting only every 4th fence FROZE the game (guest waits on an
  unsubmitted fence) - never do that. The frame is a CPU<->GPU round-trip chain; the next real
  lever is latency per round trip: the arm64 GPU process (MoltenVK encoding natively, branch
  gpu-process) rather than settings.
- 2026-11-01: ALWAYS ship/test the LTO build (plain build_macos.sh): the ISS_LTO=OFF quick builds
  used all of 2026-10-31 were ~25% slower on the bridge (33-40 -> 45-51 FPS, particles on); fire
  scene unchanged (fence-bound). TLG_LOD_SCALE=0.5: bridge 45-51 -> 48-56 (~+8%), fire unchanged
  (indoor); not default (distant detail). PREFILL=1/2, fast math, heaps, SUBMIT_DRAWS=256: noise.
- 2026-11-01 native GPU process (TLG_GPU_PROCESS=1, rebuilt with the day's fixes; arm64 MoltenVK
  replaced by the patched one too): smoke tutorial 60-64 FPS (single 45-50), burning building
  36-44 (single ~36), bridge 45-52 clean. First run after a rebuild compiles everything again
  (own shader cache: 10 s freeze, 630 compiles). ARTIFACT (user-reported, confirmed): in the
  burning building the smoke shows hard-edged rectangles and faint large glyphs/digits ("0 0"),
  i.e. smoke sprites sample a stale text/font texture; single process is clean at the same spot
  (SHOT_SIZE=1920 SHOT_SECONDS=0.5 diag runs, crop x 38-68%, y 18-48%). Not the quad index path
  (TLG_MAC_QUAD_INDEX=0: glyphs still there). TLG_GPU_PROCESS_VERIFY_MS samples 0 bytes (useless
  as is). One start hung: "GPU split: request 19 unanswered for 30 s". Next: find the texture the
  smoke draw samples in the host vs single (TLG_ADDR_TEX_LOG in the host) and why the host's
  texture cache misses its invalidation.
- 2026-11-01 late morning: START-UP HANG FIX: TLG_BP08=0 default (run_macos.sh). Loading the ~80 MB
  driver VkPipelineCache blob hung about half the starts at "driver pipeline cache loaded" in BOTH
  modes (0 of 4 hangs without it, alternating modes; no FPS change seen). Native GPU process made
  the default (run_macos.sh TLG_GPU_PROCESS=1, launcher toggle default on, writes 0 when off).
  Smoke glyph artifact in native mode: INTERMITTENT (user saw it in the 10:58 run hsbb, not in hsba;
  my 0.5 s full-res crops rarely catch it). Not the quad path, not the driver cache blob. Frame
  captures of both modes differ by timing, so per-pass texture diffs are inconclusive. Next: a
  deterministic trigger (e.g. log in the host when a draw samples an image whose guest range was
  written since its last upload without an invalidation), or compare the host's invalidation events
  with tlg-probe's write faults.
- 2026-11-01 midday: smoke glyph artifact came back with the texture-set memo off (one clean run
  was luck). Likely cause: TextureCache::RefreshImage judged a MaybeCpuDirty image (under a page,
  untracked since MarkAsMaybeDirty) unchanged by hashing only its first 8x8 pixels; a texture
  streamed into the same memory with equal corners stayed stale. Now the whole image is hashed.
  Unverified (user had to stop); the memo-off default in the host stays for now. Native baseline
  (memo off): fire 36.8, smoke 55.4 FPS. Also reported in native mode: rain streaks in mid-air
  missing in the smoke tutorial (only ground splashes). Next: verify the glyph fix, rain streaks,
  then native perf (MVK prefill 2 / sync submits A/B were interrupted).
- 2026-11-01 evening, particles by mode (smoke tutorial, crops of the same area):
  normal + shared + late read: rain streaks in mid-air, few ground splashes.
  native + shared + late read: no streaks, many white splash blobs ("too big droplets", user).
  native, TLG_MAC_QUAD_INDEX=0: same as native (not the quad path). Normal + quad path: streaks.
  native + late read off: no rain at all (as normal without it).
  native + TLG_PARTICLES=precise: splashes, real smoke, blue shards, still no streaks, 34 FPS.
  So each mode draws a different SUBSET of particle systems; streaks need something only the
  single process has. Shaders are not the difference: SPIR-V of 578 common shaders identical except
  7 vertex permutations (b4a8ad8e_3, b94242a8_8-13: numbering?). GPU timestamps: same ns clock.
  Next: compare the streak system's data between modes (TLG_MAC_INDIRECT_LOG / capture of its
  draw: VS, counts, buffers), start from which draw renders streaks in normal mode (TLG_SKIP_DRAW_PS).
- 2026-11-01 evening: default back to normal mode (native optional). Start-up time (warm cache, first
  presented frame, was 7.3 s): run_macos.sh keeps the ISA rewrite table until the image, script or
  CPU features change (isa_rewrite.py took 2 s each start). TLG_START_TRACE=1 prints wall-clock
  marks; everything until the pipeline cache constructor (scripts, game load, window, Vulkan device
  under Rosetta) is the remaining time, nothing after it is measurable. With TLG_BP08=1 no frame
  within 25 s: the blob stays off. Rain streaks: capture diff by shader was inconclusive (captures
  differ by camera); parked while native is optional. After a rebuild the first start is slower.
- 2026-11-01 night, native mode: MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE=0 default when
  TLG_GPU_PROCESS=1 (fire 37.8 -> 43.9, smoke same; prefill 2: no gain). The whole-image hash did NOT
  end the smoke glyphs (user saw them again). Rain/glyphs are not arm64 ordering: an x86-64 host under
  Rosetta (build by hand into out/x86host with -arch x86_64 against out/gpu, TLG_GPU_HOST=<path>
  TLG_GPU_HOST_X86=1) shows the same picture, so the SPLIT itself is the cause. Not the cause:
  command buffer arena copies (reading guest buffers directly: same), frames ahead, late read
  (needed in both modes), shaders (SPIR-V same), semaphores. TLG_INDIRECT_STATS=1: per-VS indirect
  draws and counts every 5 s: same five particle VS in all modes (3ab1ca58, 5e6e49c1, 6df3789c,
  bbd2d927 plain quads 12-40k verts, e90701f9 single quads), counts differ. Next: skip each VS in
  normal mode (TLG_MAC_SKIP_VS) to find the streak system; then diff its inputs; also what else the
  split changes: DingDong/compute queue order, fault relays, stubs in host_iss.c.
- 2026-11-01 late: ROOT CAUSE AREA of the native artifacts PROVEN with TLG_STALE_CHECK=1 (image's
  guest memory hash at upload vs at bind; =2 also re-uploads, 3 FPS): normal mode 0 stale, native:
  0x7071278500 (512x512, 16 layers), 0x70717d4500 (256x256, 64 layers), 0x7071d44500 (128x128, 256
  layers), all BC fmt 137, sampled by the rain PS 24dad4be and stage f8eafe6c; and 0x70ee252000 (2048
  layers, 174 MB, fmt 143) by lighting ce1675e0. Reported fully tracked and clean, yet 6-48 of their
  64 KiB chunks changed, scattered, no write fault reached the texture cache. VS bbd2d927 draws the
  streaks (TLG_MAC_SKIP_VS removes them); its args show half the verts in native. With repair the
  ground lighting matched normal mode and the white blobs vanished (too slow to judge streaks).
  Ruled out as the writer's path: guest mprotect (protects=0), protection function (same one),
  tlggpu_cpu_write, command buffer copies, x86 vs arm64. Note: runtime_memory_set_gpu_hooks seems never
  called in the host (TLG_TOGGLE_FILE watcher never starts): check MAP/UNMAP/INVALIDATE hooks there.
  Next: identify the writer: who writes those arrays (log write faults in that range on both
  sides; are pages really protected in tlg-probe at that moment: vm_region; GPU-side writers).
  User also reports new big black dots in native mode (not analysed).
- **Native stale textures fixed (2026-11-01):** the writer is the renderer itself: `Rasterizer::CopyBuffer`'s
  memcpy branch (DMA copies of texture data, e.g. 0x70717d4500 <- 0x70e31e2000, ~21k per smoke run).
  In one process that copy faults on the protected page, which invalidates; the host's view of guest
  memory is never protected, so nothing happened. CopyBuffer and FillBuffer now call `InvalidateMemory` by
  hand when `TlgSplit::Host()` (`TLG_HOST_WRITE_LOG=1` logs copies >= 4 KiB). Burning building stale check:
  native 9 -> 0. Smoke, native: ground luminance 11.5 = normal, 60-64 FPS (normal 52-57); fire native
  ~40 FPS, 240 s without a hang. `TLG_STALE_CHECK` runs (~4 FPS) in the fire scene end in a Metal GPU hang
  in both modes, also before this fix: an effect of the check. Liverpool's WriteData memcpys are still
  not invalidated in the host. To check by eye: glyph smoke, rain streaks, big droplets, black dots.
- **Native is the default (2026-11-01 night, for the owner's 1-2 h play test):** run_macos.sh
  TLG_GPU_PROCESS=1, launcher toggle on (writes 0 when off). README's FPS table is still normal mode.
- **First-start precompile wired in:** run_macos.sh -> scripts/macos/prepare_shader_jobs.py (46 s, once;
  `<user>/cache/shader-jobs.bin` + .stamp) -> vk_precompile (13 threads). Metal cache cold 127 s, warm
  19 s, 25175 records, 42 fail harmlessly (MarkReadConstBufferSharpSources assert, caught). Cache ~800 MB.
  Bridge: in-game compile time 25.8 s cold without it -> 1.3 s, worst frame 13 s -> ~1 s. diag_run sets
  TLG_PRECOMPILE=0 unless asked, as test saves are copied. Not tried yet from ISS.app with port/user.
- **Test caches are empty:** the cache folder name changes with the build key and bridge/smoke/chase-save
  hold an old one, so tests compile from nothing (warm-up never ran there). A rebuild did NOT reset the
  Metal cache (19 s after two rebuilds): why the bridge was cold on 2026-11-01 evening is unknown.
- **Storage hang:** cache_storage.cpp num_requests was a plain u32 (compile threads ++, I/O thread --): a
  lost update left the I/O thread spinning at 100% and WaitIdle waiting forever (3 of 4 first starts).
  Atomic now: 0 of 5 first starts since. Worth a look: FPS may gain from one core not spinning in normal play.
- Precompile's merge into the main VkPipelineCache only when BP08 is on: first-start footprint 12 -> 7.7 GB.
  Second start 8.2 GB vs 7.2 GB without precompile (warm-up's shader stages; matters on 16 GB Macs).
- SurfaceFormat: 10_11_11 unorm (fatal in 2 of 726 runs, mid-game) now maps to the float format with a
  warning. Who asks for it is unknown. 1 s frames with 0 compiles are the harness's screenshots
  (4 per 150 s with SHOT_SECONDS=1, none with 0). Bridge end-of-script view: 10k draws, steady 29 FPS.
- **Plan for 2026-11-02:** (1) owner's 1-2 h play test on native + precompile: logs in ~/Library/Logs/ISS,
  first-start timing, artifacts, crashes, 10_11_11 warnings. (2) Fix what it finds. (3) Measure: normal vs
  native at the open city view (29 FPS), FPS gain from the storage fix, README's table for native.
  (4) Second-start memory (warm-up stages) for 16 GB Macs. (5) Self-contained ISS.app + GitHub release:
  bundle Python and scripts/build_jobs.py + tools/xpps, first-start progress UI in the launcher.
- **2026-11-02 measurements (M3 Max, no screenshots, which cost FPS):** native vs normal: bridge 50-55 vs
  40-45, open city view 41-46 vs 25-27. Native smoke 60-64, burning building 29-49. README table is native.
- TLG_ON_DEMAND=1 (no warm-up): 770 MB less, but 19.6 s of compiling and an 8 s frame: not the default.
  The second-start memory stays at 8.2 GB.
- **Release app:** scripts/macos/package_app.sh [version] -> port/dist/ISS.app + zip (49 MB). Layout finds
  Resources/port, data in Application Support/ISS. iced-x86 1.22.0 there (1.21 has no wheel for 3.12):
  ISA table byte-identical. Metal's cache is per app: first start in a new bundle 127.7 s, second start
  7 s to the first frame. Not tried on another Mac or with the quarantine flag. gh is not installed here.
- SDL's signal handlers swallowed SIGTERM, so Stop did nothing: SDL_NO_SIGNAL_HANDLERS=1 in run_macos.sh.
- **2026-11-02 night, black rounded squares (both modes, opening forest, top left of the sky):** blobs are not
  tile-aligned: one bad pixel spread by a blur window. Bit-tested NaN/Inf guard on float render targets and
  storage images (NanGuardF32, meta version 14): 0 of 281 frames with black, was 7-50 per run. Porting
  bloodborne 2c2da89 alone (bit tests, mac/mad legacy) did not fix it. Test: FPS ~42 -> ~38.5, maybe heat
  (menu 112 instead of 120): measure. Owner has not confirmed by eye; dark smoke flicker not checked.
  Also: SDL_UpdateGamepads in the game process (split had no pump; owner: controller works, gyro unconfirmed).
  **All this is uncommitted, and the zip predates it.** Reproducer: out/latest-save (copy of the owner's save, no
  route file), TLG_PAD_REPLAY_AUTO=1 taps Cross forever because gameplay_started() never fires in the split
  (tlggpu_total_draws is the host's): also why TLG_PAD_RECORD_SECONDS records nothing. To fix.
- 2026-11-03: NaN guard on/off (TLG_NAN_GUARD, own cache): route 39.1/39.0 vs 39.4/39.6, smoke 54 vs 57, fire 36.6 vs
  33.6: no cost beyond noise. All runs are below yesterday's (route 42, smoke 60-64), guard off too: unexplained.
  F8 / PS button: PNG in ~/Desktop/ISS Screenshots (tlggpu_screenshot); PS button unconfirmed on macOS.
- 2026-11-04 smoke flicker (truck crash, copy in port/out/truck-save; tools in port/out/smoke-tools, ignored by git).
  ALL translucent smoke+fire: one indirect draw a frame, VS 0xbbd2d927, PS 0x24dad4be (soft particles against depth, sun
  shadow via Dref, array textures). Whole big patches vanish for one frame. Ruled out: depth read, textures, CPU vertex
  count, TLG_PARTICLES=precise, compute fences, vblank 60, fixed GPU timestamps. Count path: GDS 0x404 -> copy to memory
  (0x706ff58000 ring) -> CS 0xa16fa93a (thread 0 writes args = count*4 and 8-byte lists). 64 extra quads reduced dips
  (4,3,2,11 vs ~15 of 400 shots) but owner saw artifacts: not a fix. Dip metric noise is huge (4-35, same settings): do not
  trust one run; owner's BLIND A/B decided: TLG_RFL_ALL=1 (default on for ISS; less flicker, FPS same) and a GDS barrier.
  GDS writes were not tracked, so no barrier before the copy. Barrier at every GDS-binding dispatch: owner preferred it blind, but bridge
  FPS 61/49 vs 67/63. Now only in Rasterizer::CopyBuffer (TLG_GDS_BARRIER=0 off): bridge 67.3/67.6 vs 67.3;
  THIS placement has NOT had a blind test. Flicker remains. Open: 5 wave64 CS with ReadFirstLane left on 32 lanes
  (ce1675e0 e33305e5 a6931a4b a859c9f6 bfe009da, not in the dispatch timing top list, inconclusive), 30 shaders binding GDS.
  Diagnostics kept: TLG_MAC_INDIRECT_TRACE, TLG_CS_WATCH, TLG_GDS_LOG. Owner's real save is the early one
  ("Guests", 2%); Seattle is in port/out/save-backups/seattle-run-20261103. Columns before the tunnel = game effect (owner
  checked PS4 video). Owner's play: bridge 25-30 FPS, not reproduced standing (67).
  Release: .github/workflows/macos-release.yml, tag mac-v* (pre-release). Next: owner wants a gameplay video + Reddit post.
- 2026-11-05: controller dead in the release app although "gamepad connected": tlg-probe sits inside ISS.app there (in the
  local app it is a symlink to out/), macOS treats it as a background app and Game Controller withholds input; the window is
  the GPU host's. SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 (run_macos.sh) fixes it, confirmed on the downloaded zip by the
  owner including gyro in the graffiti scene. Side effect: pad input also while another app is in front.
  Quick smoke test scene (owner): the very first intro screen with text.
- 2026-11-07 SMOKE FLICKER FIXED: WaveMemoryBarrierPass (shared_memory_barrier_pass.cpp, ported from iss_pc renderer 2,
  analysis in iss_pc #557 gist by nickospalatianos-blip): the particle sort exchanges data through buffer memory within a
  GCN wave without barriers; 32-lane hosts race (duplicated/dropped particles). TLG_WAVE_MEM_BARRIERS (ISS default on,
  shader salt bit 256). Test without user: first launch (empty save + cache copy), X at 30 s, the intro text crawl smoke;
  port/out/smoke-tools/crawl_dump.sh <label> [VAR=val] dumps 180 consecutive frames at 50 s (TLG_FRAME_DUMP_TRIGGER/COUNT)
  and rawdip.py counts one-frame dips: before 3-10 dip frames / 0.6-1.0% cells, with the pass 0 / 0.00% (twice), =0 back
  to 0.65%. Owner confirmed by eye in the crawl and at the truck. Not tried yet: the LDS barrier pass never runs on
  MoltenVK (needs_lds_barriers), the gist also enabled lds_barriers_multi_wave.

## Session 2026-11-07/08: smoke, flares, compute queue, launcher (local paths are the owner's Mac)


State: master of this work = branch `macos` at e16ab52 (pushed). Working copy has UNCOMMITTED experiments, all default
off (keep or drop; never stage the fsr-vulkan submodule):
- TLG_LDS_BARRIERS=1 (force the LDS barrier pass on MoltenVK; salt bit 512): no visible/FPS change.
- TLG_LOSSLESS=1 / TLG_LOSSLESS_PROBE=1 (image.cpp, vk_runtime.cpp UpgradeUsage, image_view.cpp stats): textures without
  ShaderWrite/PixelFormatView, upgraded once on need (5-9 of ~2,000 images). Real mode: +0-3%, noise. Probe's +10% came
  from broken rendering. Depth views count depth writes as is_storage (don't upgrade depth for that).
- TLG_ACB_FENCE_WAITS=1 (liverpool.cpp): compute-queue WaitRegMem uses pending/deferred fence values; compute ReleaseMem
  labels noted. No gain alone; needed with unified compute fences (GPU thread idle 0.7% -> 69%, 41 -> 44 FPS).
- RunComputeReleaseMem passes its label to DeferUnifiedFence (only used when TLG_UNIFIED_FENCE_WAITS=1).
- TLG_LAZY_COMPUTE_FLUSH=1 (DrawPipe::SetIdleHandler + Rasterizer::FlushLazyFences): deferred compute fences submit when
  stage B idles: Vulkan submissions 64 -> 37/frame, FPS unchanged.
- TLG_MUTEX_SPIN_US=N / TLG_MUTEX_STATS=1 (src/runtime_mutex.c): ~16k contended guest locks/s, spin wins almost all,
  no FPS gain.
- Diagnostics worth keeping: TLG_SKIP_CS=<hex,...> (drop compute dispatches), TLG_OCCLUSION_LOG=2 (draws counted by
  occlusion queries: pass size, depth view, depth test/func).

Measurement notes: always alternate variants, >=3-4 runs each (bridge spread 10-15% run to run); fps.sh <save> <label>
<secs> [VAR=val] in port/out/smoke-tools (now syncs the cache back with rsync --delete: ditto merged stale cache files and
the next warm-up crashed in MoltenVK with "cannot reserve buffer(0)"). Save copies: port/out/bridge-fresh (consistent
cache; bridge after the crash), truck-save. Camera sweep: PAD_SCRIPT=scripts/macos/pad/camera_sweep.txt. Builds for tests:
TLG_LTO=OFF bash scripts/macos/build_gpu_arm64.sh (~4 min); releases use LTO (CI). Owner plays with run sessions via
diag_run.sh play-<x> 3600 TLG_USER_DIR=$PWD/out/testplay (copy of bridge-fresh), host input on.

Profile (Metal System Trace, bridge ~61 FPS): GPU 81% busy (fragment 56%, compute 28.5%, vertex 19%), ~10.6k encoders/s
(~175/frame, median 13 us). MoltenVK CPU share small. Pass enders: FlushBarriers ~10k/s; colour General->General
feedback ~2,000/s can't stay in-pass (MoltenVK restarts Metal passes on General barriers: Apple GPUs can't read an
attachment written in the same pass; real fix = framebuffer fetch, days). ColorAttachment->ShaderRead ~2,600/s is a real
transition. TLG_MAC_KEEP_PASS=1: -3-12% passes, no FPS. Fast math: already effectively on (no NoContraction emitted,
MoltenVK ON_DEMAND). TLG_GPU_PROFILE timestamps read 0 on MoltenVK (useless). Owner's heavy spot near the police car:
~7,500 draws/frame, GPU thread idle 0.5%, ~5 us/draw -> ~25-30 FPS: CPU-bound draw processing (future target).
TLG_LOD_SCALE=0.5 lever exists live in the ⌘, menu ("LOD distance"); owner was judging quality, no verdict yet.

Open bug 1 (do first): HEADLIGHT/LAMP FLARES SHOW THROUGH WALLS and from far away (owner screenshots
~/Desktop/ISS Screenshots/ISS 2026-11-07 20-50-50.png). Confirmed driven by occlusion queries: TLG_OCCLUSION_MODE=3 (all
zero) hides them. Mode 4 counts every query (0 dropped, ~2,900/s); results written correctly (copyQueryPoolResults,
eWait, valid bits). TLG_OCCLUSION_LOG=2 shows the test object (vs 3279e8b8 / ps ebb91ff5, 24 indices) drawn in three
passes: 480x270 with a 512x384 D32 downsampled depth, test on, func 6; a no-attachment pass (reported 16384x16384), depth
test off (likely the total-area reference); 1920x1080 with the full 2048x1152 depth, test on. Hypothesis: the 512x384
downsampled depth is wrong (e.g. all far for reverse-Z GEQUAL) so every flare passes. Next: dump that depth image at the
query (TLG_DIAG_DEPTH / frame dump tools) and check who writes it; also check the no-attachment pass clips to the guest
viewport/scissor. Any fix must not cost FPS.

Open bug 2: SMOKE VANISHES AT SOME CAMERA ANGLES (police car hood, bridge). Not occlusion-related. Fixed by
TLG_UNIFIED_COMPUTE_FENCES=1 (compute fences written at GPU completion; particle survivor counts correct), owner
confirmed by eye, but 60 -> 44 FPS (with TLG_ACB_FENCE_WAITS=1; 41 without). Cost = real extra particle compute
(4.7 -> 9.7 ms/frame GPU, writes in place 2x), spread over many shaders (TLG_SKIP_CS of 5b1e2375, cde8e3d8, 8a4ea184,
a4edc92b, 19100b24, 773bab89: each <= +1.6 FPS), not barriers (TLG_WAVE_MEM_BARRIERS=0 same), not submissions. Main guest
thread waits ~75% on GPU fences (guest+0x53b340) either way. Not shippable until the FPS is back. Ideas: find why the
early-fence path skips systems (which fences/labels, maybe only some need GPU-completion order), shader timeline in
Instruments (Shader Timeline option) for per-shader cost.

### 2026-11-08 session results (branch `perf-quality-view` = macos + 7 cherry-picked upstream View commits; rest uncommitted)
- Flares (bug 1): occlusion queries verified correct. TLG_OCCLUSION_LOG=2 now also prints each counted query's sample
  count (read back later) with its pass; TLG_DEPTH_TRACE=<addr> logs draws into a depth target; TLG_IMAGE_TRACE=<addr>
  logs texture-cache lookups. The 480x270 depth (0x7107e00000) is cleared then filled each frame by proxy occluder meshes
  (vs 0fc0b59e/c01162f7/10d037fb) before the query draws; walls give 0 samples. Not reproduced by script: needs the owner's
  camera spot. Possibly the owner's "fixed" control run was mode 3. Parked.
- Upstream iss_pc View commits (quality_floor / dyn_cull / draw_budget / quality_target_fps) cherry-picked, build OK. Truck
  scene: floor 0 36.5 FPS / 4,776 draws; 4: 39.5 (smoke over the fire gone); 8: 42 (background trees and bushes gone); 11: 46;
  14: 77.5 / 539 draws ("potato graphics", owner). Not a default; at most a manual option.
- Truck scene is GPU-bound: Metal trace 84% busy (fragment 53%, compute 32%, vertex 15%), ~190 passes/frame.
- Particles through MoltenVK tessellation (indirect rect lists ~108/frame): dropping them all (TLG_MAC_SKIP_RECT_INDIRECT=1)
  gives only +2.7% at the truck and +2% on the bridge with correct smoke. Raising TESS_PER_SUBMIT/IN_FLIGHT: 0.
- Correct smoke (TLG_UNIFIED_COMPUTE_FENCES=1): dispatches double (~400 -> ~1,000 a frame), compute 43% of GPU time.
  - MoltenVK patch MVK_TLG_CONCURRENT_DISPATCH=1 (deps/src/MoltenVK-src, uncommitted; concurrent compute encoders + memory
    barrier at each Vulkan barrier): 0 gain (barriers between nearly every dispatch). Installed lib = old one
    (deps/*/lib/libMoltenVK.dylib.pre-concurrent; patched build saved as .concurrent).
  - TLG_MAC_LOAD_BOUNDS=0 (no software bounds checks): 41 -> 23 FPS (garbage loops): checks are required.
  - TLG_GPU_FENCE_WRITE=1 (compute fence label written by the GPU via vkCmdUpdateBuffer, IRQ only after completion): 0 gain.
  - Remaining idea: per-dispatch fixed cost; cheaper bounds checks (one compare per vector load); find why ~1,000 tiny
    dispatches cost ~10 ms (Xcode GPU capture of one frame, needs the owner at the Mac).
- Spotlight (mds) was busy during later runs; FPS drifted 44 -> 37 over the afternoon: compare only alternated runs.
- 2026-11-08 later: TLG_MAC_LOAD_BOUNDS=stores (loads unbounded) 39 -> 22 FPS: load bounds needed too.
  Metal GPU capture trigger (TLG_METAL_CAPTURE_TRIGGER, shim/tlg_metal_capture.cpp): unusable, a device capture writes the
  16 GB guest buffers (31 GB in 24 s, frame never ended).
  FSR3 at TLG_RENDER_RES=1280x720 on the bridge: ~54 FPS (below native ~69) with flicker and artifacts on characters
  (owner saw it): the v1 temporal path has no game jitter/motion calibration for ISS. MetalFX (temporal, Metal 4, frame
  interpolation) needs the same inputs: first port upstream's game-side temporal hooks (1facb8c, 0d2ff89, ffdbdfa...,
  written for renderer v2). Denoised scaler is for ray tracing (not in ISS); spatial MetalFX ~ FSR1 (already there).
  Other ports surveyed: uncharted_pc small compute readback (not our problem: unified memory), SotC GENERAL-sync and
  depth-RO barriers: implemented as TLG_GENERAL_SYNC=1 / TLG_RO_READS=1 (image.cpp), truck: passes -4%, FPS unchanged.
- 2026-11-08 temporal upscaling, stage 1 done, stage 2 stopped (owner: Full HD looks fine on the Mac screen):
  - Branch perf-quality-view has upstream's game-side hooks (1facb8c, ffdbdfa, e453f86, 0c58e2f; probe.c conflict: _WIN32
    block dropped). CUSA00004 01.07 = same code as CUSA00223 01.07 (all signatures at +0x6f30 in the SELF), allowed in
    probe.c install_iss_temporal and iss_native_resolution.h (uncommitted). Hook test passes against our eboot
    (tests/test_iss_temporal_hook.cpp: _ symbol prefix on macOS, SELF segment offset found by signature;
    port/gpu_v2/gpu/iss_temporal_math.h copied from upstream/developer).
  - TLG_ISS_TEMPORAL=1 + TLG_ISS_TEMPORAL_READY=1 (test flag, probe side): markers every frame (liverpool.cpp logs
    "ISS temporal marker"), 8-phase Halton jitter, reversed depth, near 10 far 1e7, FPS unchanged.
  - Why FSR3 flickered: CameraMotion::OnConstants looks for GoW3's scene constants (far 3000): never found in ISS, so no
    motion vectors and RunScaled never ran (spatial stretch only); TLG_RENDER_RES alone does not lower the game's render
    size (needs iss_native_output/percent in the ini).
  - Ceiling: iss_native_percent=67 (1286x724, no upscaler) at the truck 37.2 -> 41.2 FPS (+11%): the game is not
    pixel-bound on an M3 Max; after FSR/MetalFX cost ~0-8%. Owner plays at 1920x1080 (window, or app full screen
    unstretched on the 3456x2234 Retina). Resume stage 2 only for weaker Macs or for sharpness.
  - Stage 2 plan if resumed: marker -> RunInOrder -> TemporalUpscaler::OnIssMarker (color by marker address, jitter from
    the marker, Jitter() 0 for viewports), CameraMotion ISS mode with upstream iss_camera_motion.comp (reproject =
    prevVP * inverse(curVP), IssTemporal::Reproject), run RunScaled at the marker; then MetalFX via MTLSharedEvent
    imported into a timeline VkSemaphore and its own MTLCommandQueue (MoltenVK async submits break commit order).
- 2026-11-08 compute measured per shader: MoltenVK patch MVK_TLG_SPLIT_DISPATCH=1 (each dispatch its own encoder, labelled
  from vkCmdInsertDebugUtilsLabelEXT; lib deps/*/lib/libMoltenVK.dylib.split, swap in only for the trace) + TLG_DRAW_LABELS=1
  (dispatch labels "cs vs <hash> ps 0 <groups>") + Metal System Trace; parser scratch cs.py. Result (bridge, correct smoke):
  nearly every dispatch is 1 workgroup of 64 threads (cde8e3d8, 8a4ea184, a4edc92b, 19100b24, 773bab89, ...; 5b1e2375
  1-9 groups, ~210 a frame), 6-20 us each = launch latency, run strictly one after another. Heavy single dispatches:
  64ad2129 ~0.9 ms, ce1675e0 ~0.54 ms (once a frame).
  TLG_QUEUE_STATS=1 (liverpool.cpp): per frame graphics ~316 dispatches either way; async compute (ACB) 158 a frame by
  default, ~530 with correct smoke (TLG_UNIFIED_COMPUTE_FENCES=1). The extra smoke cost is the game's async compute queue,
  which on PS4 overlaps graphics; here it runs serialized in the one Vulkan queue. => Next: async compute (ACB work on a
  second VkQueue / MTLCommandQueue, synchronized at the game's own fences / WaitRegMem).
- 2026-11-08 RELEASE PREP (worktree ../iss-rel, branch macos, NOT pushed yet): 650f779 occlusion mode 5 default (flares
  never through walls; owner checked on the bridge; FPS = mode 4 within 1-2.5%), 28f41fc compute queue unordered
  (TLG_ACB_UNORDERED, default on) + MoltenVK concurrent compute encoders (patches/moltenvk-1.4.2-concurrent-dispatch.patch,
  MVK_TLG_CONCURRENT_DISPATCH=1 in run_macos.sh, build_moltenvk.sh applies all patches) + TLG_UNIFIED_COMPUTE_FENCES and
  TLG_ACB_FENCE_WAITS default on. Release build: truck 36 -> 43-45 FPS with correct smoke (owner: smoke perfect, no
  artifacts), bridge 69 -> 59 (owner chose this, "B"). Push + tag mac-v0.1.4 waits for the owner's explicit OK.
  Key finding: ACB runs of ~95 dispatches end only at RELEASE_MEM (TLG_ACB_RUNS=1); barriers were 99.9% WAR/WAW
  (TLG_BARRIER_WHY=1). Two Metal queues do NOT overlap (pure Metal probe scratch q2/mq.mm); MoltenVK per-dispatch cost
  ~2 us without argument buffers. Bridge with B: GPU 62% busy, main waits on compute fences queued behind graphics:
  next = a separate VkQueue for the game's compute queue (latency, not concurrency). zsh: pass env lists as ${=B}.
  KosmicKrisp arm64 (deps/kosmickrisp-arm64): reaches gameplay after its own cache warms, but ~2x GPU time (36 vs 69).
- 2026-11-08 late: released mac-v0.1.4 (flares mode 5, unordered compute queue + concurrent MoltenVK encoders, correct
  smoke) and mac-v0.1.5 (launcher FPS cap / render scale, Benchmark at the truck crash with Share on GitHub (label
  benchmark), Metal HUD fixed: window loop yields the main queue). Release branch work lives in worktree ~/iss-rel.
  Uncommitted there (diagnostics/experiments, default off): TLG_GFX_UNORDERED (graphics-queue dispatch runs, ~110-146
  dispatches each, no gain: bridge +1%, truck noise), TLG_GFX_RUNS, TLG_FENCE_LATENCY.
- SEPARATE COMPUTE QUEUE = next big FPS step, feasibility confirmed: TLG_FENCE_LATENCY=1 on the bridge: compute
  fences wait median 9.6 ms (p90 10.1) behind ~30 unfinished submissions (graphics 3.6 ms); truck 2.5-3.5 ms. Pure
  Metal probe (scratch q2/mq2.mm): a tiny compute on a second MTLCommandQueue finishes in 0.25 ms while 130 ms of work
  is queued on the first (it overtakes; the two do not run concurrently but are interleaved). Plan: the game's compute
  queue (ACB) records into a second Scheduler/VkQueue (MoltenVK queue family 1), cross-queue sync with timeline
  semaphores at the game's own WAIT_REG_MEM / RELEASE_MEM / EVENT_WRITE_EOP points; shared buffer/texture caches need
  barrier/ownership handling. Days of work; expected: bridge back toward ~69 with correct smoke.
- 2026-11-09 SECOND COMPUTE QUEUE WORKS (branch agent/compute-queue, TLG_ACB_QUEUE=1, default on in run_macos.sh after the
  owner checked smoke at the truck by eye, 2026-11-09): bridge 59.8 -> 65.3 FPS, truck 45.3 -> 45.2 (M3 Max, AC, fps_ab 3x60 s), compute fence latency on the
  bridge 9.6 -> 2.4 ms, footprint unchanged. What it took, in the order the regressions fell:
  (1) second Scheduler on a second VkQueue, MoltenVK exposes more families when TLG_ACB_QUEUE=1 (patch compute-queue);
  (2) guest memory imported twice: tracked MTLBuffer for main, untracked alias for the second queue, otherwise Metal
  hazard tracking makes the second queue wait for the first (making the whole arena untracked instead costs FPS: truck
  45 -> 41, bridge 59 -> 53); (3) fences as VkEvents polled by a thread (encodeSignalEvent is seen mid command buffer,
  writes are visible by then; command buffer completion costs ~0.5 ms each); (4) the compute queue's memory DMA as a
  batched compute dispatch (acb_copy.comp): 190 copies a frame, each used to fall back to main and stall the chain;
  (5) the five compute shaders with images on the second queue too (TLG_ACB_IMAGES, default on with the queue):
  bridge 55.8 -> 65.2; they were 780 waits for main per 5 s; (6) an MTLFence chain between the second queue's encoders
  (MVK_TLG_CHAIN_QUEUE_FAMILY, set by vk_instance; TLG_ACB_CHAIN=0: a semaphore per command buffer instead): the semaphore
  cost ~0.44 ms GPU idle per command buffer: truck 44.2 -> 45.5. Untracked encoders need the chain for order: do not
  remove it for FPS. Metal System Trace of idle gaps: /tmp scripts, classify any-busy gaps by (queue, channel, same cb).
- 2026-11-09 night: WHY THE FRAME IS SERIAL. The game keeps one frame at the GPU: before submitting frame N its main
  thread waits (guest 0x1f2700 called from 0x196cb1: while ((s64)labels[[r12+0x124]] < [r12+0x130]) usleep(50);
  labels at guest [0xE17E80]) for label 0, a frame counter (+1 a frame), to reach N-1: lag always 1.0
  (TLG_WAIT_PROBE=1 with TLG_THREAD_PROFILE_MS=2, runtime_freeze.c). A second, rarer wait (guest 0x1695e0, ctx
  idx 2, lag ~3.5) is ~20% of it. guest usleep (0x53b2b0) is an rdtsc busy spin. So each frame = game submits ->
  our command processor + draw recording turn ~4,800 draws into Metal (2-2.5 us a draw) -> GPU; the GPU idles
  between chunks. CPU cost per draw in tlggpu-host therefore is frame time: Time Profiler with
  `xctrace record --template 'Time Profiler' --all-processes` (`sample` cannot read the host's threads) found
  PostDraw testing 1664 dirty bits per draw (libc++ has no bitset _Find_next): word walk = truck 43.1 -> 46.8.
  Next per-draw hot spots (truck, 6 s, samples of ~6000 per thread): command processor CollectRingBindings 703,
  ApplyGraphicsRegisterPacket ~340; DrawRec BindResources 779, BindTextures 225, BarrierTracker::InsertNode 191,
  MVKCommandBuffer::reset 164, memmove 243; VkRecorder mostly spins.
- 2026-11-09 late (after PR #3): fresh Time Profiler at the truck: DrawRec is now the busier stage (command
  processor waits in DrawPipe::Drain ~20%). DrawRec: SubmitExecution 12% (vkBeginCommandBuffer ->
  MVKCommandBuffer::reset 10%), almost all from the game's graphics fences (RunEventWriteEos -> DeferUnifiedFence ->
  Flush, ~24 a frame); TextureCache::RetireReadbacks memmove 5.5%; BindResources 23%; BarrierTracker 8%.
  VkRecorder: PushChanged 25% (MoltenVK MVKCmdPushDescriptorSet allocates per call), the rest spin. MoltenVK encode
  on its dispatch queue ~36% of a core. Measured, no clear gain: PendingWriteOverlaps reading DrawPipe::Consumed()
  once (46.8 vs 47.3, kept as cleanup); TLG_GFX_FENCE_BATCH=8 (fences leave with the next submission / when stage B
  idles; default 0 = off): truck 47.3 -> 48.3, bridge 66.1 -> 65.6; =32: truck 47.5, bridge 64.8.
  TLG_SUBMIT_DRAWS=128 with it: nothing. Day's standing numbers: bridge 59.8 -> 66.1, truck 45.2 -> 47.3.
  Lesson: a session restart kills background A/B scripts; keep results in a file (they were) and never leave the
  tree stashed for longer than a build.
- 2026-11-10 morning, quiet Mac (everything closed; identical runs now agree within +-0.2 FPS on the bridge),
  lib_ab.sh, 5 alternated runs x 60 s, one LTO build, switches only:
    truck   head 47.4 | TLG_GFX_FENCE_BATCH=8 48.3 | TLG_IDLE_FLUSH=16 outside passes 47.7 | both 48.1
    bridge  head 66.6 | 66.7 | 66.7 | 66.7
    Seattle head 46.4 | 47.5 | 46.3 (runs 31.4 and 43.9 in it) | 48.0 (one run 39.5)
  => TLG_GFX_FENCE_BATCH default 8 (0 = a submission per fence). TLG_IDLE_FLUSH stays off: no gain and
  single-run dips in Seattle; do not retry. PR #4 was merged before its second commit (the unattended kit) was
  pushed: check `git log origin/macos` contains what you think before branching.
- 2026-11-10: host span cache (host_runtime.c: runtime_memory_clamp / host_view_covered answer from a per-thread
  remembered span + table generation, no rwlock; TLG_HOST_SPAN_CACHE=0 = old): truck 48.3 -> 49.2, bridge
  66.5 -> 68.0 (4 alternated runs, +-0.2). TLG_CMD_PRERESET=1 (a pool per command buffer, a thread resets it at GPU
  completion): no gain (truck 49.1, bridge 67.8), default off. NOTE lib_ab.sh swaps only libtlggpu.dylib:
  host/ changes need an env switch. SEATTLE DROP: in ~1 of 3 runs of seattle-run-20261103 FPS falls 49 -> 28 at
  ~40 s and stays, in every variant; the owner sees it when a person walks past. Same draws/dispatches per frame,
  GPU busy 10 -> 27 ms a frame (Metal trace: new 1-5 ms intervals: ~1.7 graphics-queue compute dispatches, ~3
  render encoders with long vertex and fragment parts a frame). It did not appear in 5 runs with
  TLG_DRAW_TIMING/TLG_DISPATCH_TIMING on. Likely the 25-30 FPS of busy streets. Open.
  The session stopped for 8 h after a tool call at ~11:10 (no hung wait): unattended turns can simply end.
- 2026-11-10 evening: THE SEATTLE DROP IS THE GPU CLOCKING DOWN, not the scene. Metal trace: every GPU interval,
  even the smallest, is ~3x longer in the slow state (compute median 7.9 -> 23.7 us, vertex p10 4.0 -> 12.5 us);
  light count of the tiled culling dispatch (ce1675e0, 120x68) unchanged (~400). tools/gpu_probes/gpu_pstate.c
  (IOReport, no root): GPUPH at P13 of 15 while 49 FPS, then P13 -> P7 -> P3 -> P1 within 3 s and stuck at P1 with
  93-95% active = 28 FPS. No thermal or power limit (pmset -g therm clean, 140 W). In harness runs the game window
  is NOT frontmost (the owner's Chrome/Telegram is): macOS throttles the GPU for it. Ruled out on the way: a new
  full-screen pass (ps 93efbd54, skipping it changes nothing), geometry (+3.6% indices), per-draw timing.
  New diagnostics: TLG_NEW_SHADERS=1, TLG_TIMING_AFTER_S=N, TLG_CS_DUMP=<hash>, TLG_HOST_ACTIVITY (host asserts
  NSActivityUserInitiated|LatencyCritical). First M1 Pro benchmark (issue #5): 21.2 FPS at the truck, defaults.
- 2026-11-10 late: MoltenVK patch push-descriptor-storage (MVKCmdPushDescriptorSet keeps its image/buffer/view
  arrays in vectors reused with the pooled command instead of new[]/delete[] per write): 6 alternated pairs, all
  faster: truck 47.3 -> 48.6, bridge 66.0 -> 67.6 (owner was using the Mac: absolute numbers low, pairs consistent).
  LOCAL ISS.app NEVER GOT GAME MODE FOR THE GPU PROCESS: make_app.sh linked tlg-probe into out/ and tlg-probe
  resolves the link, so tlggpu-host ran from out/, an anonymous process outside the bundle (bloodborne_mac_port
  found the same, MACOS_BACKLOG 2026-10-26). Now refresh_app.sh copies the three executables into the bundle
  (make_app.sh and after every build_macos.sh). Release apps always had them inside. TO VERIFY with the owner:
  full screen from the app, gpu_pstate running: does the GPU stay clocked up? Harness runs keep the window in
  the background, so the P13 -> P1 drop can hit any long harness run: check with gpu_pstate when a run looks slow.
  Upstream check 2026-11-10: iss_pc developer has nothing new for us since the fork point except pad work
  (30c3f7a) and the temporal upscaler (needs the renderer side here); shadPS4: #5572 submit every N draws (we
  have it), #5591 PopPendingOperations fast path (we have it), #5571 one wait for all pending image downloads and
  #5599 refresh images stored after a GPU-written image (worth reading), mbcnt generalisation #5669/#5689.
- 2026-11-11: REAL PLAY IS GPU-BOUND. Owner, 6.5 min in Seattle, full screen from the fixed local app, with
  gpu_pstate + TLG_FRAME_STATS: GPU at P12.8-13.0 the whole time (no clock-down in front, full screen), 90-99%
  active, median 44 FPS, p10 34, min 25, GPU thread idle 50-68%; every frame > 150 ms had shader compiles (1662
  in 6 min). Harness runs keep the window behind other apps and the clock-down still hits them (also from the
  bundle): agent_run.sh now logs it ("GPU clocked down N s", lib_ab.sh shows it per run); TLG_RAISE_WINDOW=1
  brings the window to the front when nobody is at the Mac.
  PASS PROFILER: TLG_PASS_LABELS=1 + patches/moltenvk-1.4.2-pass-labels.patch name each Metal render encoder
  "tlgpass WxH [formats] d:depth vs ps"; tools/gpu_probes/trace_passes.py gives GPU ms per kind of pass a frame.
  First use found ~5.6 ms a frame at the truck in passes with NO attachment and a 16384x16384 render area
  (shadPS4 #5452 had just fixed the same). TLG_BOUND_EMPTY_PASS (default on; 0 = before): the area is bounded
  by the scissors. lib_ab 3 x 60 s: truck 47.5 -> 61.9, bridge 67.0 -> 75.3, Seattle 47.4 -> 50.5. Same
  picture (screenshots; the owner watched the runs).
- 2026-11-12: ISSUE #11 GPU HANG = FPS CAP. M1 Max, Hank's fight, "Slowest" preset. Reproduced with
  out/chase-save (burning building) + TLG_FPS_LIMIT=30 (also 45): Metal GPU hang in 40-80 s, never uncapped;
  particles off / scale 50 / office alone don't matter; old builds hang too. MVK_CONFIG_DEBUG=1 +
  TLG_DRAW_LABELS=1 (now dispatches too) + TLG_PASS_LABELS=1 name the faulted encoder: the gbuffer pass with
  67 indirect draws of vs 5e6e49c1 (lit particles), whose loop walks a 43523-node tree (ud10 = 0xaa03; same as
  vs 3f30e238). Fix: TLG_MAC_VS_LOOP_CAP default 1024 (ShaderMetaVersion 15: the cap is NOT in the cache key,
  test such switches with AGENT_CACHE=<fresh dir> in agent_run.sh). 280 s at a 30 cap, no hang; picture same
  even at 64; lib_ab 3 x 60 s neutral (truck 64.2/64.1, bridge 78.0/77.8, Seattle 52.5/52.0). Root cause
  (why the walk runs long only when the GPU idles between frames) still open.
- 2026-11-12: harness runs from inside ISS.app by default now (agent_run.sh: TLG_PROBE in the bundle, full screen,
  raise; AGENT_APP=0 = out/): gamepolicyd "Game mode status is now on" for the whole run; truck 65.0 vs 65.5
  windowed (no effect standing). The Seattle CLOCK-DOWN (P13 -> P1 at 96% busy, 54 -> 30 FPS, 40-90 s into a
  run) happens in full screen, in front, Game Mode on, any input state: it follows the SoC power loop (kernel
  ApplePPMPolicyCPMS setDetailedThermalPowerBudget fell to 4-10 W during back-to-back runs; no "thermal
  pressure" state). Not window, not input idle (a synthetic mouse event every 5-20 s did not prevent it). A/B
  stays fair by alternating; lib_ab shows "GPU clocked down N s" per run, discard runs with many seconds.
  Rejected today: MVK_CONFIG_FAST_MATH_ENABLED=1 (all 775 shaders "MathMode Fast"; truck 64.5/63.2, bridge
  77.7/77.4, Seattle 52.9/51.9). Seattle compute: cs 5b1e2375 is ~650 dispatches a frame of 1-3 groups (tree walk
  bound by ud6), on the ACB queue; TLG_MAC_SKIP_CS=5b1e2375 (new diagnostic) 53.9 -> 53.7: free, overlapped.
  Seattle pass restarts (TLG_PASS_STATS): ~100 a frame on the same attachments: ~51 colour General->General
  feedback (Apple GPUs have no render-pass barriers: needs framebuffer fetch), ~34 DepthStencilReadOnly->same
  (TLG_MAC_KEEP_PASS=1 keeps those passes open).
- 2026-11-12: TLG_MAC_KEEP_PASS ON BY DEFAULT (lib_ab 3 x 60 s: Seattle 53.6 -> 56.4, bridge 78.0 -> 81.2,
  truck 65.5 -> 65.7; same picture). Remaining Seattle restarts are colour feedback: gbuffer decal shaders
  (b4565337, 27f242c2, bfe1af9, 9ce8b6db, ...) write part of cb0/1/2 (masks 7/3/c) and texelFetch the same
  targets at their own pixel (fragcoord x 1920/render width). TLG_FEEDBACK_STATS=1 counts them (all overlap the
  target's level/layer). Upper bound: TLG_MAC_KEEP_FEEDBACK=1 (diagnostic, stale reads) Seattle 56.0 -> 61.8.
  Real fix: framebuffer fetch (VK_KHR_dynamic_rendering_local_read is in MoltenVK; SPIR-V SubpassData +
  InputAttachmentIndex -> [[color(n)]]), only at 100% render scale.
- 2026-11-12: FRAMEBUFFER FETCH FOR COLOUR FEEDBACK (default on; TLG_MAC_FB_FETCH=0 off; vk_fb_fetch.h). Gbuffer
  decals load their own targets at their own pixel; ImageResource::fb_fetchable (IR: ImageRead whose coords are
  ConvertS32F32(FragCoord.x/y [x scale]), level 0) + the image is one of the draw's colour targets (same address
  and format; FbFetchTarget in specialization.h, ImageSpecialization::fb_cb) -> SPIR-V SubpassData +
  InputAttachmentIndex cb + OpImageRead + the T#'s channel permutation; patches/moltenvk-1.4.2-framebuffer-fetch.patch
  sets SPIRV-Cross use_framebuffer_fetch_subpasses on Apple GPUs ([[color(n)]]). The runtime skips the feedback
  transition/barrier for those bindings, so the Metal pass stays open. Only at 100% render scale (TLG_RENDER_SCALE
  unset). lib_ab 3 x 60 s: Seattle 55.6 -> 62.1, bridge 81.0 -> 85.0, truck 65.0 -> 65.4; Seattle restarts 3690
  -> 408/s; screenshots Seattle/bridge/burning building identical. Not converted: shaders that SAMPLE their target
  (9215a6e8, 961c93ba). ShaderMetaVersion 16, PipelineKeyVersion 6. Installed MoltenVK backups:
  deps/{arm64,x86_64}/lib/libMoltenVK.dylib.before-fbfetch.
- 2026-11-12: AFTER FB FETCH SEATTLE IS NO LONGER GPU-BOUND (~61 FPS, GPU idle ~53%, mostly starved: the
  next command buffer arrives during the gap). The game keeps one frame at the GPU, so the chain game ->
  stage A (2.1 us/draw x ~4800 draws) -> stage B -> MoltenVK -> GPU sets the frame. 40% of draws reached
  stage A unprepared, all "not ready yet" (the stats line now says why): the DrawPrep workers ran at QoS
  BACKGROUND (E-cores, low clocks) and waited ~25% on libc++ std::shared_mutex (its lock_shared takes a
  mutex). Fixes: TlgRwLock (gpu/shim/tlg_rwlock.h) for programs_mutex; workers and scanner user-initiated on
  Macs with >= 8 performance cores, background otherwise (TLG_PREP_QOS overrides): 47% -> 99% prepared.
  lib_ab: bridge 85.5 -> 87.2, truck (utility vs user) 65.6 -> 68.3. The graphics WaitRegMem drains (~60/s)
  are the VO label (flip) wait, not compute fences (TLG_WAIT_LOG=1 lists waits not met at once).
  gpu_pstate now prints GPU watts too (IOReport Energy Model; no CPU energy there on an M3 Max).
- 2026-11-12: THE SEATTLE CLOCK-DOWN, REVISED. It is not power or heat (16" M3 Max, Mac15,11: GPU ~18-19 W at
  P13 in Seattle, 4 W at P1; no budget messages while it happened), not input, not the window, not the app
  launch (scripts/macos/app_run.sh starts through ISS.app with its own launch.conf: same), not the compute queue
  (TLG_ACB_QUEUE=0: same), not a 30 FPS cap (frame times 31.3 ms, not 33.3; the game never calls
  sceVideoOutSetFlipRate: logged now). ~40 s into a standing Seattle run the GPU goes P13 -> P1 and stays there
  at 96% busy (every pass ~4.5x slower). Seattle standing is now CPU/latency-bound (GPU idle ~53% at P13):
  macOS's GPU performance controller seems to drop a GPU it sees underused and then holds the slow state. The
  owner's moving play was GPU-bound (90-99%) and never dropped. Measure Seattle only before the drop (lib_ab's
  60 s runs mostly are), and treat "GPU clocked down" runs as tainted.
  Precompile (players' first start, never in agent_run: TLG_PRECOMPILE=0) fails 20 records with
  resource_discover_pass.cpp:257 MarkReadConstBufferSharpSources assertions: the same with the 0.1.9 library.
- 2026-11-12: tried TLG_EARLY_VO_LABEL (release the previous display buffer's label at the next flip's submission
  instead of on screen): WaitRegMem drains 60 -> 32/s, FPS the same (Seattle 62-64 before the drop). The VO label
  is not on the critical path; the game's own frame-counter wait (one frame at the GPU) is. Reverted.
- 2026-11-12: rejected TLG_SUBMIT_IDLE (submit after N draws when the GPU has caught up): truck 68.2 -> 68.5,
  bridge 86.6 -> 87.3 (noise). After today's PRs all three scenes are CPU/latency-bound (GPU idle 40-53%):
  the next lever is per-draw CPU on stage A (~1.9 us), stage B (BindTextures/BindBuffers/AccessBuffer,
  MVKCommandBuffer::reset in vkBeginCommandBuffer ~10%) and MoltenVK encoding on the host's main queue.
- 2026-11-12 evening (0.1.10 tagged: #12-#18): WHERE THE TRUCK FRAME GOES. Readbacks: the 512x512 RGBA16F
  linear image went back ~10 times a frame; copies a later queued copy covers are not published
  (TLG_READBACK_SUPERSEDE=0: off), ObtainBuffer's ResolveReadbacks answers from the queue's page span without the
  lock (TLG_READBACK_SPAN=0): truck 69.2 -> 69.5. TLG_DRAW_RUNS=1: 85% of draws repeat the previous pipeline (60%
  in runs of 16+), 57% are shadow draws (ps 1318ddd4, 2 buffers, no images: 0.6-0.7 us each in DrawRecord; average
  1.12 us, 4850 draws a frame). SENSITIVITY (TLG_STAGE_DELAY_A_NS / _B_NS = busy wait per draw): +400 ns a draw
  on stage A costs 0.55 ms of the frame (28% of the added time), on stage B 1.03 ms (53%): 69.9 -> 67.3 / 65.2
  FPS. So no stage owns the frame: B about half, A a quarter, the rest MoltenVK/GPU/guest. Halving DrawRecord
  would give ~+7 FPS. time_profile.py --thread <name>. Stacked PRs: #16 landed in its base branch because #15
  was merged first; retarget children to macos before merging the base.
- 2026-11-12 night: WHAT THE TRUCK/BRIDGE FRAME REALLY IS (TLG_WAIT_PROBE=1 TLG_THREAD_PROFILE_MS=2): the game's
  main thread works ~48% of the frame (~7 ms under Rosetta), waits ~36% (truck 5.2 ms, bridge 3.8 ms) in the
  end-of-frame particle join (guest 0x16961a < 0x29cd5b, label idx 2 behind by ~5 fences when the wait starts)
  and ~16% for the frame counter. So draws are NOT most of the frame: TLG_SHADOW_CASCADES=-3 (new, src/
  iss_perf_patches.h: sun-shadow cascade offset only) halves the draws (4776 -> 2395) for 68.8 -> 75.1 FPS;
  TLG_RENDER_SCALE=50: 77.2; both: 81.0 (frames ~12 ms: the floor). Frame intervals are not snapped to the
  display (histogram now printed with TLG_FRAME_STATS). True draw merging is not there: only 14% of repeat draws
  share the mesh. No gain: TLG_COMPUTE_FAIRNESS=4/16 (69.4 all), TLG_ACB_FLUSH_FENCES 1/2/4 (69.2-69.8 vs 70.1 at
  8; 16: 70.5, 64: 70.4, noise). NotePendingGpuWrite pruned every 32 notes (it was 42% of a dispatch on stage A;
  TLG_PENDING_PRUNE=0): 69.9 -> 70.3. Dispatch path costs ~3 us per dispatch on each stage (530 a frame);
  stage B's share is the barrier tracker (31%). MoltenVK barriers are NOT no-ops for us: the patched concurrent
  compute encoder orders dispatches by them. Next: a timeline of the particle chain (where its ~5 ms go).
- 2026-11-13: THE PARTICLE JOIN IS GPU TIME, AND EVERY FENCE ENDED A COMPUTE ENCODER. TLG_CHAIN_LOG=1 (per
  compute-queue fence: seen by the command thread, run by the recording thread, event recorded, submitted, GPU
  done): the chain's 28 fences a frame pass our threads in ~1 ms and are submitted at once; the GPU then needs
  3.5-4 ms (3 ms with half the draws and a quarter of the pixels: ~1 ms is contention with graphics). MoltenVK
  ends the Metal encoder at every vkCmdSetEvent (MVKCommandEncoder::signalEvent), so the chain restarted 28
  compute encoders a frame. Now the fences recorded since the compute queue's last flush share one event
  recorded at the flush (Rasterizer::AcbFlush; runs stay ordered by their memory barriers; TLG_ACB_FENCE_GROUP=0:
  an event per fence) and TLG_ACB_FLUSH_FENCES defaults to 64 with it: lib_ab truck 70.5 -> 73.1, bridge
  88.0 -> 91.6, Seattle same. chase-save 240 s at a 30 cap, smoke-save, chase-save clean; truck screenshot same.
  ACB copies already go as compute dispatches (no blit encoders). The rest of the chain (~3 ms) is ~530
  one-workgroup dispatches; next idea there: Metal System Trace of the compute queue with grouped fences.
- 2026-11-13: SHADOW DETAIL (launcher Advanced > Quality: Full / Lower / Lowest -> TLG_SHADOW_CASCADES unset / -2 /
  -3; also honoured with TLG_PERF_MODE; default Full, presets keep Full). Only scenes with sun-shadow cascades
  gain: truck 72.8 -> 75.2 (-2) -> 80.0 (-3; 4781 -> 2400 draws), bridge (1467 draws) and Seattle (4519 draws)
  do not change at all. Screenshots out/agent/shots-cascades/ (truck at -3 looks the same from the benchmark
  spot). MVK_TLG_CONCURRENT_DISPATCH=0: truck 72.6 -> 52.8 (the concurrent encoder matters a lot);
  TLG_ACB_UNORDERED=0: no change with grouped fences. Launcher part written by a Sonnet subagent in its own
  worktree (no builds while measuring), reviewed and built here.
- 2026-11-13: MOLTENVK PREFILL=2 IS NOW A WIN (run_macos.sh default; 0: off). MVK_CONFIG_PERFORMANCE_TRACKING=1
  (+ MVK_CONFIG_ACTIVITY_PERFORMANCE_LOGGING_STYLE=0, MVK_CONFIG_PERFORMANCE_LOGGING_FRAME_COUNT=900) showed ~14
  command buffers a frame encoded for ~0.45 ms each on MoltenVK's queue thread AFTER vkQueueSubmit: a serial
  stage behind the recording thread (graphics fence latency 4.2 ms with ~5 unfinished submissions). With
  MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2 the encoding happens as the recorder thread records. lib_ab:
  Seattle 65.0 -> 73.1, bridge 92.3 -> 97.5, truck 72.6 -> 73.4, burning building (chase-save) 78.9 -> 81.0;
  =1 is slower (56.7 in Seattle). chase-save 240 s at a 30 cap, smoke-save (92.7 -> 97.7), chase-save clean;
  memory the same. It gave nothing on 2026-10-29 (GPU-bound then). Seattle's frame (TLG_WAIT_PROBE): particle
  join 26%, frame counter 24%, the main thread's own work ~36% + 8% waiting for its job workers.
  DAY TOTAL vs the 0.1.10 state (3 x 60 s, switches off vs defaults): truck 70.2 -> 73.4, bridge 87.1 -> 97.2,
  Seattle 63.3 -> 70.7.
- 2026-11-13: THE SEATTLE CLOCK-DOWN EXPLAINED AND FIXED (host/gpu_keepalive.m). macOS's GPU performance
  controller clocks the GPU down when it is in use but not full: ~30-40 s at ~94% active (a CPU-bound scene
  leaves gaps) -> P13 -> P1 within seconds, and it STAYS at P1 at 98% (Seattle 72 -> 33 FPS). Reproduced at the
  truck with TLG_STAGE_DELAY_B_NS=1500 (GPU 93-95% active: 52 -> 29.5 FPS at ~50 s); scenes at 99-100% never
  drop. Not thermal (kernel "Thermal Budget" keeps rising), not power (AC, powermode 0), Game Mode on throughout.
  With a 30 FPS cap the controller also goes to P1 and the cap is still met (fine, less heat). Fix: one small
  compute dispatch at a time on a Metal queue of its own in tlggpu-host fills the gaps (GPU reads 100%, stays at
  P13): Seattle 120 s at 72-74 FPS. Always on it cost FPS where the GPU is full anyway (truck 74.5 -> 72.9), so
  a thread reads GPUPH residency twice a second (IOReport, as gpu_pstate.c) and fills only while the GPU is
  50..98% active for 2 s, pausing 1 s every 10 s to look at the game alone: truck 73.8 / 73.7, bridge 97.7 /
  97.1 (off / on, 3 x 50 s). TLG_GPU_KEEPALIVE=0 off, =2 always, _STEPS (20000), _LOG=1. Test tool:
  tools/gpu_probes/gpu_fill.swift. Open: whether the filler pulls a GPU that is already at P1 back up.
- 2026-11-13: GPU-BOUND AGAIN, PASS PROFILE (scripts/macos/pass_profile.sh <scene> <label>: 4 s of Metal System
  Trace at second 28 with TLG_PASS_LABELS=1, table by trace_passes.py). After prefill + grouped fences the frame
  counter wait is nearly gone and the particle join is the queue for a full GPU (truck 43%, Seattle 44%, bridge
  20% of the frame; GPU 99-100% active). Seattle: ~88 Metal encoders and 13.7 ms of GPU work a frame: compute
  encoders 2.45 ms (19 a frame), main gbuffer pass 1.44, 11 vkCmdCopyImageToBuffer blits 0.39 (the 512x512
  RGBA16F linear image, copied at every graphics fence). ONE DEFERRED DOWNLOAD PER IMAGE PER FRAME
  (TLG_READBACK_PER_FRAME=0: at every fence): the first fence after an image's first write of a frame still
  copies it, later writes go with the next frame's first fence. lib_ab 3 x 50 s: bridge 97.0 -> 98.3, truck
  74.5 -> 75.1, Seattle 72.4 -> 72.7; chase-save 180 s at a 30 cap, smoke-save clean (readback waits there:
  21 per 5 s, were 0).
- 2026-11-13: FPS DECAY OVER A SESSION = vkWaitSemaphores (a 0.1.10 regression from TLG_CMD_PRERESET). A 10 min
  Seattle soak: 73.7 -> 62 FPS, GPU at P13 all along, no heat warning, GPU process footprint flat (4.0 GB).
  Two Time Profiler recordings in one run (out/agent/soak_profile.sh, 45 s and 500 s): tlg:CmdReset 510 -> 1871
  samples in vkWaitSemaphores -> MVKTimelineSemaphoreMTLEvent::registerWait -> [MTLSharedEventListener new] /
  notifyListener, MoltenVK's queue thread 3988 -> 6635 (CFArray scans, kevent_id), DrawRec waiting in
  MVKQueue::submit 0 -> 270. MoltenVK builds a listener and a notification on the queue's MTLSharedEvent for
  EVERY vkWaitSemaphores call; CommandPool::ResetThread made ~4,000 a second (one per submission) and they pile
  up inside Metal. Now it polls the counter with 250 us sleeps (TLG_CMD_RESET_POLL=0: waits), and Semaphore::Wait
  polls too on macOS (yield x64, then 20 us sleeps; TLG_SEMAPHORE_POLL=0). Soak after: 73.9 -> 70.5 at 8.5 min
  (was 70.8 -> 63.2), thread loads flat. lib_ab 3 x 50 s: truck 75.3 -> 76.4, bridge 98.8 -> 99.5, Seattle
  73.1 -> 72.4. LESSON: 60 s benchmarks hide session decay; soak 8-10 min before a release. Never call
  vkWaitSemaphores per submission on MoltenVK. The game process's "memory" line in agent_run grows with
  streaming (388 -> 2150 MiB in 10 min), not a leak in the host.
- 2026-11-14: THE SESSION SLOPE IS THERMAL, FROM THE GAME ALONE. Fair heat A/B (out/agent/heat_ab.sh: bridge 9 min,
  6 min idle cool-down before each, owner away, 2 rounds alternating): every run holds 100-102 FPS for ~3.5 min,
  then within a minute the P-cores go from state 21 to ~5 (of 21) and the GPU from 13 to ~10.5 (of 15), and FPS
  settles at ~83 (kernel "Thermal Budget" in the log falls the whole run; pmset shows no thermal warning;
  16" M3 Max on AC). Less busy-waiting (branch agent/less-spin: game usleep sleeps, recorder spin 20 us,
  targeted drain wake-ups; ~0.5 core less) changes nothing: 83.3/82.9 vs 82.7/83.6 after the drop, same timing.
  Not merged (no measurable gain here; might help fanless Airs, unmeasurable). tools/gpu_probes/cpu_pstate.c:
  CPU cluster P-state residency. The game's usleep (eboot 0x53b2b0) spins on rdtsc below 501 us
  (TLG_GUEST_USLEEP_SPIN_US patches the threshold); TLG_DUMP_CODE=<hex>,<n> prints loaded eboot bytes.
- 2026-11-14: A FRAME CAP DOES NOT AVOID THE THROTTLE (bridge 9 min, cool-down first): cap 90 and 85 drop at the
  same ~3.5 min to ~83/79-84; cap 75 dips once to 71.5 at ~4 min, then holds 75 with the clocks recovering. The
  P-cores sit at state ~20 even at a 75 cap: at the cap the game's main thread spends ~62% of a core in its
  rdtsc usleep spin (eboot 0x53b2b0) and five JobWorkers ~16% each, VkRecorder ~49%. Sending the spin to sleep
  (TLG_GUEST_USLEEP_SPIN_US=0) did not move the throttle uncapped. Thermal equilibrium on the bridge uncapped
  is ~83 FPS on a 16" M3 Max; the launcher's quality presets (TLG_PERF_MODE) already cover weaker Macs. Untested
  (a system setting, the owner's to switch): Energy Mode "High Power" (fans earlier, sustained clocks).
- 2026-11-14: SUSTAINED NUMBERS (9 min, cool-down first, owner away): truck 75 -> 60-61, bridge 101 -> 83,
  Seattle 74 -> 63, all from ~3.5-4 min on. After the drop the scenes are CPU-BOUND: P-cores ~7-9 of 21, GPU
  stays at P13 (its "active" residency stays 100%, power 25 -> 20 W), the GPU command thread's cost per draw
  rises 1.9 -> 2.4 us (+27%) for -19% FPS. The keep-alive filler is not involved (truck: filler off all run;
  TLG_GPU_KEEPALIVE=0 gives the same 60.8 vs 61.3). So for sustained FPS on a 16" M3 Max what counts is CPU work
  per frame (ours per draw and the game's own under Rosetta), not GPU time and not spinning.
