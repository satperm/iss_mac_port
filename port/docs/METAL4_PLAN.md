# Metal 4 / MetalFX plan (owner-approved 2026-11-14)

The port renders through MoltenVK (Vulkan on Metal 3). Metal 4 features either attach at the edge
(MetalFX through `VK_EXT_metal_objects`) or need a renderer of our own instead of MoltenVK. Every feature
is opt-in and gated on availability (Metal 4 = macOS 26 and Apple M1 or later); the MoltenVK path stays
the default and the fallback.

## What helps ISS (measured 2026-11-14, M3 Max, High Power, 1080p)

| Feature | Expected for ISS | Basis |
|---|---|---|
| MetalFX temporal upscaler | ~+10% in fire and close-up cutscenes, ~0 in calm scenes | burning village: 75% render scale +15%, 50% +23% (before upscaler cost); truck/bridge 0-7%, Seattle 0% |
| MetalFX frame interpolation (macOS 26) | 120 presented frames from 60 on ProMotion; a little more input latency | same inputs as the upscaler |
| MTL4Compiler, flexible pipeline states, archives | fewer first-encounter compile hitches, faster first start | needs our own renderer |
| Metal 4 command model (MTL4CommandBuffer/Allocator, argument tables, residency sets, explicit barriers, unified compute encoder) | less CPU per draw, fewer encoder switches (80-100 a frame); mostly for CPU-bound (weaker) Macs | needs our own renderer; Bloodborne's own Metal renderer gained ~2% on an M4 Max |
| Ray tracing, MetalFX denoiser, ML encoders/tensors, sparse placement | nothing: the game has no RT or ML, memory is not the limit | not planned |

## Stages

0. **Inputs for MetalFX (3-5 days).** Game-side hooks exist on branch `perf-quality-view` (Halton jitter of the
   projection window, camera snapshot, a PM4 NOP marker after the game's AA with the frame's jitter, matrices,
   near/far and the AA output image: `src/iss_temporal.h`, `src/iss_temporal_hook.h`, tests). Missing: the renderer
   side - consume the marker on the GPU command thread, camera motion vectors by depth reprojection
   (prevVP * inverse(curVP)), then object motion (characters, cars, particles); MTLSharedEvent interop.
   Check: a debug view of the motion vectors in fire, a cutscene and while running.
1. **MetalFX temporal upscaler (~1 week).** Render at 75-85% and upscale to 1080p in place of the game's FXAA.
   Port the MoltenVK interop from bloodborne_mac_port (`gpu/shim/bbport_metalfx.mm`). Launcher option, off by
   default; the owner judges the picture (native vs MetalFX: fire, cutscene, running, camera turns).
2. **MetalFX frame interpolation (~1-2 weeks).** Main risk: the game draws HUD and subtitles into the finished
   frame; they must be kept out of interpolation (the marker sits after AA, before grain/UI) or accepted as
   artifacts. Off by default; owner judges picture and input latency.
3. **Own Metal 4 renderer instead of MoltenVK (months).** Argument tables for descriptors, residency sets,
   reusable command buffers, explicit barriers (our BarrierTracker maps onto them), one compute encoder for
   particles and copies; MTL4Compiler with flexible pipelines and harvested archives. Start only if data from
   weaker Macs (issue #5) shows them CPU-bound; MoltenVK stays as the fallback until the new path is as stable.

Each stage: the four scenes (truck, bridge, Seattle, burning village `out/owner-copy-20261114`), 9-minute soaks,
the stability suite, and no FPS change with the feature off. Verify Metal 4 API names against the macOS 26 SDK.
