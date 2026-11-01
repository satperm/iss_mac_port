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
