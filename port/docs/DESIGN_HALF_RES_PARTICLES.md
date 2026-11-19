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
