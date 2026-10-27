/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Temporal upscaling, the game side (CUSA00223 01.07): what the eboot hooks of iss_temporal_hook.h
 * hand to a renderer that runs a temporal upscaler (FSR 3.1, DLSS) in place of the game's own AA.
 *
 * This header is the whole contract. It is plain C so the C++ renderer includes it as is; the
 * functions are implemented in src/probe.c (the renderer is linked into the same executable).
 *
 * How it works, in the order a frame goes:
 *
 *   1. The game's per-frame AA setup (eboot 0x472810) picks FXAA or SMAA T2x and, for T2x, shifts
 *      the camera's projection window by +-1/4 pixel. With the hooks active (see "Gating") the
 *      window is shifted by a Halton jitter instead, always, and the game's AA mode is forced to
 *      "FXAA" for the rest of the frame (that is only so the game does not run T2x: the FXAA pass
 *      itself is skipped, see 3). One IssTemporalFrame is opened for the frame (a new `seq`) and
 *      the camera is snapshotted into it after the window was applied.
 *   2. The game builds the frame. The renderer reads the jitter and the camera with
 *      iss_temporal_get_frame(seq).
 *   3. In the post chain (eboot 0x1929c0), where the game would run FXAA or T2x, the hook does not
 *      call it. It records the AA's input and output images into the frame and writes a MARKER (with
 *      the frame in it) into the game's command buffer (below). The game then goes on with grain and the UI, drawing
 *      into the AA's output target as it always does.
 *   4. The renderer, when its command processor meets the marker, runs the upscaler: it reads the
 *      input image (the tonemapped scene) and writes the output image. iss_temporal_get_frame(seq)
 *      gives the same frame plus the rest of the camera, for diagnostics.
 *
 * Gating (nothing here changes the game unless all three hold):
 *   - `iss_temporal=1` in tlg.ini (or TLG_ISS_TEMPORAL=1) at start, game CUSA00223 01.07 with the
 *     expected code bytes: only then are the hooks written into the eboot image
 *     (iss_temporal_installed() == 1). With the key off the image is byte-identical to before.
 *   - the renderer calls iss_temporal_set_renderer_ready(1) once its temporal pass can consume
 *     markers. Until then (and after it calls it with 0) every hook runs the game's own code, so
 *     a renderer without the pass shows the game exactly as without the key.
 *   - the game's own "no AA" state (photo mode and the like, byte at eboot 0xf1bcd0+0xdf0f05) is
 *     left alone: no jitter, no marker.
 */
#ifndef ISS_TEMPORAL_H
#define ISS_TEMPORAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The marker is a PM4 NOP (opcode 0x10) of ISS_TEMPORAL_MARKER_DWORDS dwords: the header, then a
 * self-contained payload, so the renderer needs no other query to use it (the command stream carries
 * the frame with it, in step with the draws). The command processor ignores a NOP's payload, so a
 * renderer that does not know the marker is unaffected. Payload dwords:
 *
 *    0  ISS_TEMPORAL_MARKER_MAGIC | kind (low byte)      1  version
 *    2  frame id (the IssTemporalFrame.seq)              3  flags
 *    4  render width        5  render height
 *  6-7  jitter x, y in pixels, the displacement of the rendered content: a point that would sit at
 *       pixel q without jitter is rasterised at q + jitter (x to the right, y down).
 *       HYPOTHESIS, to confirm with the reprojection test: it is (-jitter[0], +jitter[1]) of the
 *       IssTemporalFrame, which is the shift of the projection window (see there).
 * 8-23  view-projection of this frame WITHOUT jitter, 16 floats in the order a GLSL mat4 has in memory
 *       (the game's row-vector matrices have the same bytes). From camera+0x470 (HYPOTHESIS).
 *   24  near   25 far   26 vertical fov (camera+0x584, +0x588, +0x590)
 * 27-28  guest address of the AA's input image (the tonemapped scene), low dword first
 * 29-30  guest address of the scene depth: 0 = not known by the hook, the renderer finds it
 * 31-32  guest address of the AA's output image
 */
#define ISS_TEMPORAL_MARKER_MAGIC 0x49545400u /* 'I' 'T' 'T' 0 */
#define ISS_TEMPORAL_MARKER_KIND_RESOLVE 1u   /* the game's AA pass would have run here */
#define ISS_TEMPORAL_MARKER_VERSION 1u
#define ISS_TEMPORAL_MARKER_PAYLOAD_DWORDS 33u
#define ISS_TEMPORAL_MARKER_DWORDS 34u         /* header + payload */
#define ISS_TEMPORAL_MARKER_HEADER 0xc0201000u /* type 3, count = 34 - 2, opcode 0x10 (NOP) */
#define ISS_TEMPORAL_FLAG_CAMERA_CUT (1u << 0)     /* no history (see IssTemporalFrame.reset) */
#define ISS_TEMPORAL_FLAG_REVERSED_DEPTH (1u << 1) /* HYPOTHESIS: the game's depth is reversed-Z */

#define ISS_TEMPORAL_RING 8u /* frames kept; iss_temporal_get_frame fails for older ones */
#define ISS_TEMPORAL_MAX_PHASES 32u

/* A texture the game binds: the T# (8 dwords) it passes to the AA, decoded. */
typedef struct IssTemporalImage {
    uint64_t descriptor; /* guest address of the 8-dword T# (the AA object's +0xb4); 0 = unknown */
    uint64_t base;       /* image base address from the T# (dword0 | (dword1 & 0x3f) << 32) << 8 */
    uint32_t width;      /* (dword2 & 0x3fff) + 1 */
    uint32_t height;     /* (dword2 >> 14 & 0x3fff) + 1 */
    uint32_t dwords[8];  /* the T# itself */
    uint32_t valid;      /* 1 when width and height are plausible and base is not 0 */
} IssTemporalImage;

/* The camera object the game passes to its AA (eboot global 0x7f2d2b8). Raw copies of the blocks
 * the temporal pass needs, taken after the jitter window was applied. */
typedef struct IssTemporalCamera {
    uint64_t address;
    float proj[16];   /* camera+0x1c0: projection (reversed Z) */
    float m200[16];   /* camera+0x200: copied to +0x240 by the game's camera update */
    float m240[16];   /* camera+0x240 */
    float vec420[4];  /* camera+0x420 */
    float m430[16];   /* camera+0x430..0x46f: the matrix the game copies to +0x470 before the jitter */
    float m470[16];   /* camera+0x470..0x4af: that copy (HYPOTHESIS: view-projection without jitter) */
    float raw580[16]; /* camera+0x580..0x5bf, scalars below read from it */
    float near_z;     /* camera+0x584 */
    float far_z;      /* camera+0x588 */
    float fov;        /* camera+0x590 */
    float aspect;     /* camera+0x59c */
    float window[4];  /* camera+0x5ac: the projection window now on the camera (jittered) */
} IssTemporalCamera;

typedef struct IssTemporalFrame {
    uint32_t seq;          /* frame number, from 1; 0 = empty slot */
    uint32_t phase;        /* index in the Halton sequence, 0 .. phases-1 */
    float jitter[2];       /* pixels. The window sent to the camera is base + (A, B, A, B) with
                            * A = 2 * jitter[0] / render_width, B = 2 * jitter[1] / render_height
                            * and base = (-1, -1, 1, 1). Which sign an upscaler wants is its own
                            * convention: FSR/DLSS take the offset applied to the projection. */
    float window[4];       /* the window that was applied */
    uint32_t render_width; /* the game's render size (eboot 0xe68a50) */
    uint32_t render_height;
    uint32_t reset;        /* 1: no history (first frame after the hooks became active, or the
                            * render size changed). Camera cuts are for the renderer to find. */
    IssTemporalCamera camera;
    uint32_t aa_replaced;  /* 1 once the AA hook ran for this frame and wrote the marker */
    IssTemporalImage aa_input;  /* the scene the AA reads (tonemapped, LDR) */
    IssTemporalImage aa_output; /* the target the AA writes and the UI is drawn on */
    uint64_t command_context;   /* the game's draw command buffer object that got the marker */
} IssTemporalFrame;

/* 1 when the eboot hooks are written into the image. */
int iss_temporal_installed(void);
/* The renderer says whether its temporal pass consumes markers. Safe from any thread. */
void iss_temporal_set_renderer_ready(int ready);
/* Halton phases, 1 .. ISS_TEMPORAL_MAX_PHASES (default 8). FSR 3.1 asks for 8 * (display / render)^2. */
void iss_temporal_set_phases(uint32_t phases);
/* The newest frame opened (0 before the first). */
uint32_t iss_temporal_latest_seq(void);
/* Copies frame `seq` into *out; 0 when it is too old or not opened yet. Safe from any thread. */
int iss_temporal_get_frame(uint32_t seq, IssTemporalFrame *out);

#ifdef __cplusplus
}
#endif

#endif
