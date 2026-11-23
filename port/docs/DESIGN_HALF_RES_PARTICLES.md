# Half-resolution particles for GPU-bound Macs (proposal, 2026-11-19)

Owner's decision needed: a picture-changing quality option ("Particle resolution: Full / Half") for weaker Macs.

## Why

On GPU-bound Macs (M1/M2, Pro chips at 1080p; an M3 Max at 4K) fire and smoke cost a lot of GPU time:

- Village at 4K (M3 Max, GPU-bound): the particle pass that starts with vs bbd2d927 / ps 24dad4be takes 6.3 of 39 ms
  a frame (1.9 ms at 1080p). With GPU particles off the village runs 31.0 -> 35.8 FPS (+15%), which also removes
  their simulation, so it is an upper bound.
- Issue #5 (M1 Pro): GPU-bound at ~25 FPS in front of the fire.

Particles are soft and blurred anyway; many engines draw them at half resolution and composite them.

## How (sketch)

1. Recognize the particle passes: colour target = the HDR scene (RGBA16F, full size), blend One / Src1Color
   (dual source), depth test without depth writes. They are already identified in captures (#271-like passes with
   ps dc01c60a, 96f2b35d, 4d065c0f, and 24dad4be).
2. Redirect those draws to half-size targets with a half-size copy of the depth buffer (max or min depth, downsampled
   once per pass). Dual-source blending allows one colour attachment, so the composite needs two quantities per
   pixel: accumulated colour A (start 0, blend One/Src1Color) and transmittance T (start 1, blend Zero/Src1Color),
   drawn as two passes or with the second output redirected.
3. Composite into the full-size scene: scene = scene * T + A, bilinear (or depth-aware) upsampling.
4. Behind `TLG_PARTICLES_HALF=1`, then a launcher option for the "Slower Mac" preset only after the owner has looked
   at fire, smoke and sparks.

## Cost and risk

2-3 days. The gain is roughly half to three quarters of the particle fill cost in fire scenes on GPU-bound Macs
(estimated 5-10% FPS there), nothing on an M3 Max at 1080p (not GPU-bound). Risks: halos at depth edges, particles
that rely on reading the scene (refraction/heat haze) need to stay full size, extra memory for the half-size targets.

## Implemented (2026-11-23): `TLG_PARTICLES_HALF=1`, launcher switch, off by default

The pass turned out simpler than the sketch above: `TLG_BLEND_PROBE=1` shows the fire/smoke pass draws with
ps 24dad4be into the RGBA16F scene **without a depth attachment** (soft particles read depth as a texture, with
coordinates that don't depend on the target size), with the blend **One / OneMinusSrcAlpha** (premultiplied over),
alpha Zero / One, and a single colour output (no dual source). So no depth downsample is needed:

- `vk_particles_half.cpp`: draws of the listed pixel shaders (`TLG_PARTICLES_HALF_PS`, default `24dad4be`) whose
  blend is over or additive go to a half-size RGBA16F image cleared to (0, 0, 0, 1). The pipeline variant blends
  alpha as Zero / OneMinusSrcAlpha (or Zero / One for additive), so rgb = accumulated colour and a = transmittance.
  `target_scale` 0.5 scales the viewport and scissor, as for the upscaler's output-size draws.
- When that render pass ends (scheduler end-rendering hook), a full-screen pass composites it:
  scene = scene * a + rgb (blend One / SrcAlpha, bilinear). Before any other draw the run is composited first, so
  the composite never overwrites a guest draw's viewport. Partial descriptor pushes send every binding again after it.
- One half-size image per target size (passes of 1920x1080 and 960x540 alternate in some scenes).

Results (M3 Max; lib_ab, medians; village = burning village save):

| Scene | Off | Half |
|---|---|---|
| Village 4K (3 x 60 s) | 31.1 | **35.2 (+13%)** |
| Village 1080p (3 x 60 s) | 76.9 | 80.1 (+4%) |
| Truck 4K / 1080p (2 x 60 s) | 37.5 / 83.8 | 39.1 / 85.3 |
| Bridge 4K / 1080p | 50.9 / 110.8 | 50.8 / 111.2 |
| Seattle 4K / 1080p | 42.1 / 81.1 | 42.0 / 82.9 |

Chase, smoke and coals saves 120 s each with it on: no crash lines. Screenshots (4K PNG): same fires, slightly softer
flame edges, no blocks, seams or halos seen. Not yet in the "Slower Mac" preset: the owner looks at it first.
