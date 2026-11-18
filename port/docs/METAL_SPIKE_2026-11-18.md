# Native Metal renderer: spike, day 1 (2026-11-18)

Question: is replacing MoltenVK with a native Metal renderer (as bloodborne_mac_port 0.5.0 did,
`gpu/shadps4/video_core/renderer_metal/`, 59 files) worth 4-6 weeks for ISS? Day 1 was meant to run
Bloodborne's SPIR-V -> MSL path over ISS's shaders offline. That turned out to be the wrong first
question (below); measuring where MoltenVK's time goes in ISS answers more.

## Shader conversion is not the risk

MoltenVK already converts every ISS shader to MSL with SPIRV-Cross, the same library Bloodborne's
renderer uses (`mtl_shader.mm`, `CompilerMSL`); ISS runs all of them today. Bloodborne's renderer
adds SPIR-V passes for what Metal lacks (rect lists, geometry shaders as kernels, per-vertex
inputs). What remains is not conversion but the parts ISS has and Bloodborne's renderer does not.

## What ISS needs that Bloodborne's Metal renderer lacks

| ISS feature | Bloodborne Metal renderer | Port cost |
|---|---|---|
| Quad lists (particles, UI) | skipped ("quad lists have no Metal pipeline") | low: ISS already draws them as indexed triangle lists on macOS (`TLG_MAC_QUAD_INDEX`), that path moves over |
| Occlusion queries (`vk_occlusion.cpp`) | none | medium: Metal visibility result buffers |
| Async compute queue (`TLG_ACB_QUEUE`) | single queue | medium: second MTLCommandQueue + events |
| Barrier tracker, frame record, precompile, fb fetch, particle readbacks, governor, temporal upscaler | own equivalents or none | most of the 4-6 weeks: ~35k lines of ISS renderer code are written against Vulkan |

## Where MoltenVK's time goes in ISS (Time Profiler, M3 Max, 1080p, today's macos)

| Thread | Truck (81 FPS) | Village (74 FPS) |
|---|---|---|
| `tlg:VkRecorder` busy | 80% (MoltenVK 70% of it; Apple's Metal/AGX driver ~36% of it) | 76% (MoltenVK 64%, driver ~30%) |
| `tlg:DrawRec` busy | 69% (MoltenVK 9%) | 76% (MoltenVK 13%) |
| Recorder drains (TLG_SYNC_STATS) | ~17% of wall time, mostly on DrawRec before submits (SubmitIfWorkPiledUp, every 64 draws) | |

Bloodborne before its Metal renderer (FINDINGS 2026-10-15): recorder 47-54% busy, GPU-bound; the
renderer then gave +2% at 1080p, +12-18% at 1440p. ISS draws ~4750 draws a frame and its recorder
is at 80%: at ~100 FPS it becomes the limit by itself. MoltenVK's own share (MoltenVK minus the
driver below it) is ~1/3 of that thread, ~3 ms of a 12 ms frame; native encoding keeps the driver part.

## Where the GPU's time goes (Metal System Trace, truck, 82 FPS, today's macos)

`tools/gpu_probes/trace_idle.py out/profile/truck1118.mst.xml`: the GPU idles 1837 of 4607 ms (40%):

| Idle | Share of time | Native Metal would |
|---|---|---|
| Gaps inside command buffers (~130 a frame, ~18 us each: encoder switches) | 20% | shrink them: own encoders, merged passes (Bloodborne: MoltenVK's in-buffer stalls ~3 ms a frame) |
| Starved: the next command buffer was committed during the gap | 14% | not change it (the CPU side is late) |
| Committed earlier, started late | 6% | partly |

GPU work is ~13 ms a frame (overlaps counted twice) in a 12.2 ms frame: the GPU is ~60% busy. The frame is
set by the CPU chain (game under Rosetta -> GPU command thread, idle 21% waiting for the game -> draw
recording -> MoltenVK encoding), not by the GPU.

## Verdict (day 1)

**Not now.** At 1080p on the M3 Max a native Metal renderer would remove MoltenVK's share of the encoding
thread (~3 ms of a 12 ms frame, on a thread that is not saturated) and most in-buffer GPU gaps (useful
only when GPU-bound). Expected +5-10% here, more at 1440p/4K and on GPU-bound Macs (Bloodborne +12-18% at
1440p), for 4-6 weeks plus the ISS-only features above. Cheaper first: the GPU is starved 14% of the time
(submission timing), and the game's own CPU time under Rosetta sets the pace. Revisit when 1440p+ or weak
Macs (GPU-bound) become the target, or if MoltenVK stops being maintained. Days 2-3 of the spike are not
needed for this decision.
