/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Temporal upscaling, game side: the eboot hooks of CUSA00223 01.07 (contract: iss_temporal.h).
 *
 * Two code hooks and the host functions they call. Nothing is written into the image unless the
 * setting is on and every byte the hooks depend on is the expected one (view_hook.h pattern):
 *
 *   JITTER  0x472944 (inside FUN_00472810, once a frame): the instruction pair
 *             cmp byte [rcx],0 ; jz 0x4729e0          (rcx = &AA mode, 0 = FXAA, else SMAA T2x)
 *           becomes a jump to a stub. Not active: the pair runs as before. Active: the stub asks
 *           iss_temporal_begin_frame for the jitter window, calls the game's own
 *           FUN_00581a50(camera, window) to apply it, asks iss_temporal_end_frame to snapshot the
 *           camera, sets the AA mode to 0 (so the game does not run T2x) and goes to 0x4729e0.
 *   AA      0x192c4c (inside FUN_001929c0, the post chain): the 13 bytes that start the AA's
 *           skip test
 *             lea rax,[0xf1bcd0] ; mov al,[rax+0xdf0f05]
 *           become a jump to a stub. Not active: the two instructions run as before. Active and
 *           the game would run its AA: iss_temporal_aa_replaced(ctx, input, output) records the
 *           images and writes the marker; if it succeeds the stub jumps to 0x192cff (past the
 *           FXAA / T2x call), if not the game's own AA runs.
 *
 * The stubs keep every register the game's code uses afterwards: the host functions follow the
 * System V ABI (the game's) and the call sites are 16-byte aligned like the game's own calls;
 * r11 and rax are the only scratch registers, and both are dead at both sites. */
#ifndef ISS_TEMPORAL_HOOK_H
#define ISS_TEMPORAL_HOOK_H

#include <stdint.h>
#include <string.h>

#include "iss_temporal.h"

#ifndef ISS_TEMPORAL_ABI
#define ISS_TEMPORAL_ABI __attribute__((sysv_abi))
#endif

/* ----------------------------------------------------------------------------- eboot offsets */
enum {
    ISS_TEMPORAL_JITTER_SITE = 0x472944,    /* cmp byte [rcx],0 ; jz 0x4729e0 */
    ISS_TEMPORAL_JITTER_LENGTH = 9,
    ISS_TEMPORAL_JITTER_FALL = 0x47294d,    /* the instruction after the pair */
    ISS_TEMPORAL_JITTER_TARGET = 0x4729e0,  /* the jz target (past the window call) */
    ISS_TEMPORAL_AA_SITE = 0x192c4c,        /* lea rax,[0xf1bcd0] ; mov al,[rax+0xdf0f05] */
    ISS_TEMPORAL_AA_LENGTH = 13,
    ISS_TEMPORAL_AA_NEXT = 0x192c59,        /* or al,[r14+0x1eb8d] ; jnz 0x192cff */
    ISS_TEMPORAL_AA_SKIP = 0x192cff,        /* after the FXAA / T2x call */
    ISS_TEMPORAL_SET_WINDOW = 0x581a50,     /* FUN_00581a50(camera, float window[4]) */
    ISS_TEMPORAL_AA_MODE = 0xda07f9,        /* byte: 0 FXAA, else SMAA T2x */
    ISS_TEMPORAL_CAMERA_POINTER = 0x7f2d2b8, /* qword: the camera object */
    ISS_TEMPORAL_RENDER_SIZE = 0xe68a50,    /* two dwords: render width, height */
    ISS_TEMPORAL_FRAME_OBJECT = 0xf1bcd0,   /* the frame object (also the quality level's) */
    ISS_TEMPORAL_NO_AA_FLAG = 0xdf0f05,     /* byte at frame object + this: the game runs no AA */
    ISS_TEMPORAL_BASE_WINDOW = 0xaaefc0,    /* float[4] = (-1, -1, 1, 1) */
    ISS_TEMPORAL_AA_IMAGE_OFFSET = 0xb4     /* T# inside the object the AA reads / writes */
};

/* ------------------------------------------------------------------------------- signatures */
typedef struct IssTemporalSignature {
    uint32_t at;
    uint32_t length;
    const unsigned char *bytes;
} IssTemporalSignature;

/* FUN_00472810 entry, then the site and the code the stub resumes at */
static const unsigned char iss_temporal_sig_jitter_entry[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18};
static const unsigned char iss_temporal_sig_jitter_before[] = {
    0x41, 0x88, 0x9c, 0x24, 0x2c, 0xdc, 0x01, 0x00}; /* mov [r12+0x1dc2c],bl: rcx is &AA mode here */
static const unsigned char iss_temporal_sig_jitter_site[ISS_TEMPORAL_JITTER_LENGTH] = {
    0x80, 0x39, 0x00, 0x0f, 0x84, 0x93, 0x00, 0x00, 0x00};
static const unsigned char iss_temporal_sig_jitter_fall[] = {
    0x41, 0xc7, 0x84, 0x24, 0x10, 0xcd, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}; /* mov dword [r12+0x1cd10],0 */
static const unsigned char iss_temporal_sig_jitter_target[] = {0x49, 0x8b, 0x06, 0x48, 0x3b, 0x45, 0xd0};
static const unsigned char iss_temporal_sig_set_window[] = {
    0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0xc5, 0xf8, 0x10, 0x06, 0x48, 0x89, 0xfb};
/* FUN_001929c0 entry, the site, the code after it, and the code the stub skips to */
static const unsigned char iss_temporal_sig_aa_entry[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
    0x48, 0x81, 0xec, 0x18, 0x08, 0x00, 0x00};
static const unsigned char iss_temporal_sig_aa_site[ISS_TEMPORAL_AA_LENGTH] = {
    0x48, 0x8d, 0x05, 0x7d, 0x90, 0xd8, 0x00, 0x8a, 0x80, 0x05, 0x0f, 0xdf, 0x00};
static const unsigned char iss_temporal_sig_aa_next[] = {
    0x41, 0x0a, 0x86, 0x8d, 0xeb, 0x01, 0x00, 0x0f, 0x85, 0x99, 0x00, 0x00, 0x00};
static const unsigned char iss_temporal_sig_aa_skip[] = {0x48, 0x8d, 0x1d, 0xa2, 0x91, 0xbb, 0x07};
static const unsigned char iss_temporal_sig_base_window[16] = { /* (-1, -1, 1, 1) */
    0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f};

#define ISS_TEMPORAL_SIG(at, array) {(at), (uint32_t)sizeof(array), (array)}
static const IssTemporalSignature iss_temporal_signatures[] = {
    ISS_TEMPORAL_SIG(0x472810, iss_temporal_sig_jitter_entry),
    ISS_TEMPORAL_SIG(0x47293c, iss_temporal_sig_jitter_before),
    ISS_TEMPORAL_SIG(ISS_TEMPORAL_JITTER_SITE, iss_temporal_sig_jitter_site),
    ISS_TEMPORAL_SIG(ISS_TEMPORAL_JITTER_FALL, iss_temporal_sig_jitter_fall),
    ISS_TEMPORAL_SIG(ISS_TEMPORAL_JITTER_TARGET, iss_temporal_sig_jitter_target),
    ISS_TEMPORAL_SIG(ISS_TEMPORAL_SET_WINDOW, iss_temporal_sig_set_window),
    ISS_TEMPORAL_SIG(0x1929c0, iss_temporal_sig_aa_entry),
    ISS_TEMPORAL_SIG(ISS_TEMPORAL_AA_SITE, iss_temporal_sig_aa_site),
    ISS_TEMPORAL_SIG(ISS_TEMPORAL_AA_NEXT, iss_temporal_sig_aa_next),
    ISS_TEMPORAL_SIG(ISS_TEMPORAL_AA_SKIP, iss_temporal_sig_aa_skip),
    ISS_TEMPORAL_SIG(ISS_TEMPORAL_BASE_WINDOW, iss_temporal_sig_base_window),
};
#undef ISS_TEMPORAL_SIG

/* 1 when the image has the bytes of this eboot at every place the hooks read or replace. */
static inline int iss_temporal_signature_match(const unsigned char *image, uint64_t size) {
    for (unsigned i = 0; i < sizeof(iss_temporal_signatures) / sizeof(iss_temporal_signatures[0]); ++i) {
        const IssTemporalSignature *s = &iss_temporal_signatures[i];
        if (size < (uint64_t)s->at + s->length || memcmp(image + s->at, s->bytes, s->length)) return 0;
    }
    return size >= (uint64_t)ISS_TEMPORAL_CAMERA_POINTER + 8 && size >= (uint64_t)ISS_TEMPORAL_FRAME_OBJECT + ISS_TEMPORAL_NO_AA_FLAG + 1;
}

/* ------------------------------------------------------------------------------ the shared state */
typedef struct IssTemporalSlot {
    volatile uint32_t lock; /* odd while the slot is being written */
    IssTemporalFrame frame;
} IssTemporalSlot;

typedef struct IssTemporalState {
    volatile uint8_t active;    /* installed and the renderer ready: the stubs read this byte */
    volatile uint8_t ready;
    uint8_t installed;
    uint8_t spare;
    volatile uint32_t phases;   /* Halton phases; 0 = 8 */
    uint32_t phase;
    volatile uint32_t seq;      /* last frame opened */
    uint32_t last_width, last_height;
    uint32_t was_active;
    unsigned char *image;       /* the guest image base */
    IssTemporalSlot ring[ISS_TEMPORAL_RING];
} IssTemporalState;

static IssTemporalState iss_temporal_state; /* zero: phases 0 means the default of 8 */

static inline void iss_temporal_update_active(void) {
    iss_temporal_state.active = (uint8_t)(iss_temporal_state.installed && iss_temporal_state.ready);
}

/* ------------------------------------------------------------------------------ small helpers */
static inline uint32_t iss_temporal_read32(const unsigned char *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Van der Corput / Halton: radical inverse of `index` in `base`. */
static inline float iss_temporal_halton(uint32_t index, uint32_t base) {
    float fraction = 1.0f, result = 0.0f;
    while (index > 0) {
        fraction /= (float)base;
        result += fraction * (float)(index % base);
        index /= base;
    }
    return result;
}

/* The T# (8 dwords) at `descriptor`, decoded. */
static inline void iss_temporal_decode_image(const unsigned char *descriptor, IssTemporalImage *out) {
    memset(out, 0, sizeof(*out));
    if (!descriptor) return;
    out->descriptor = (uint64_t)(uintptr_t)descriptor;
    memcpy(out->dwords, descriptor, sizeof(out->dwords));
    out->base = (((uint64_t)(out->dwords[1] & 0x3f) << 32) | out->dwords[0]) << 8;
    out->width = (out->dwords[2] & 0x3fff) + 1;
    out->height = ((out->dwords[2] >> 14) & 0x3fff) + 1;
    out->valid = out->base != 0 && out->width >= 16 && out->height >= 16 && out->width <= 16384 &&
                 out->height <= 16384;
}

static inline void iss_temporal_slot_begin(IssTemporalSlot *slot) {
    __atomic_fetch_add(&slot->lock, 1u, __ATOMIC_ACQ_REL);
}
static inline void iss_temporal_slot_end(IssTemporalSlot *slot) {
    __atomic_fetch_add(&slot->lock, 1u, __ATOMIC_RELEASE);
}

static inline IssTemporalSlot *iss_temporal_current_slot(void) {
    IssTemporalState *s = &iss_temporal_state;
    return s->seq ? &s->ring[s->seq % ISS_TEMPORAL_RING] : NULL;
}

/* ---------------------------------------------------------------------- host functions (guest ABI) */

/* The jitter hook: opens the frame and writes the window for FUN_00581a50 into window[4]. */
static ISS_TEMPORAL_ABI void iss_temporal_begin_frame(float *window) {
    IssTemporalState *s = &iss_temporal_state;
    const unsigned char *image = s->image;
    uint32_t width = iss_temporal_read32(image + ISS_TEMPORAL_RENDER_SIZE);
    uint32_t height = iss_temporal_read32(image + ISS_TEMPORAL_RENDER_SIZE + 4);
    const int no_aa = image[ISS_TEMPORAL_FRAME_OBJECT + ISS_TEMPORAL_NO_AA_FLAG] != 0;
    const int jitter = !no_aa && width && height;
    uint32_t phases = s->phases ? s->phases : 8;
    if (phases > ISS_TEMPORAL_MAX_PHASES) phases = ISS_TEMPORAL_MAX_PHASES;
    float jx = 0.0f, jy = 0.0f;
    uint32_t phase = 0;
    if (jitter) {
        phase = s->phase % phases;
        s->phase = (phase + 1) % phases;
        jx = iss_temporal_halton(phase + 1, 2) - 0.5f;
        jy = iss_temporal_halton(phase + 1, 3) - 0.5f;
    }
    const float a = jitter ? 2.0f * jx / (float)width : 0.0f, b = jitter ? 2.0f * jy / (float)height : 0.0f;
    window[0] = -1.0f + a;
    window[1] = -1.0f + b;
    window[2] = 1.0f + a;
    window[3] = 1.0f + b;

    const uint32_t seq = s->seq + 1;
    IssTemporalSlot *slot = &s->ring[seq % ISS_TEMPORAL_RING];
    iss_temporal_slot_begin(slot);
    IssTemporalFrame *f = &slot->frame;
    memset(f, 0, sizeof(*f));
    f->seq = seq;
    f->phase = phase;
    f->jitter[0] = jx;
    f->jitter[1] = jy;
    memcpy(f->window, window, sizeof(f->window));
    f->render_width = width;
    f->render_height = height;
    f->reset = !s->was_active || width != s->last_width || height != s->last_height;
    iss_temporal_slot_end(slot);
    s->was_active = 1;
    s->last_width = width;
    s->last_height = height;
    s->seq = seq;
}

/* The jitter hook, after the window was applied: the camera into the frame. */
static ISS_TEMPORAL_ABI void iss_temporal_end_frame(const unsigned char *camera) {
    IssTemporalSlot *slot = iss_temporal_current_slot();
    if (!slot || !camera) return;
    iss_temporal_slot_begin(slot);
    IssTemporalCamera *c = &slot->frame.camera;
    c->address = (uint64_t)(uintptr_t)camera;
    memcpy(c->proj, camera + 0x1c0, sizeof(c->proj));
    memcpy(c->m200, camera + 0x200, sizeof(c->m200));
    memcpy(c->m240, camera + 0x240, sizeof(c->m240));
    memcpy(c->vec420, camera + 0x420, sizeof(c->vec420));
    memcpy(c->m430, camera + 0x430, sizeof(c->m430));
    memcpy(c->m470, camera + 0x470, sizeof(c->m470));
    memcpy(c->raw580, camera + 0x580, sizeof(c->raw580));
    memcpy(&c->near_z, camera + 0x584, 4);
    memcpy(&c->far_z, camera + 0x588, 4);
    memcpy(&c->fov, camera + 0x590, 4);
    memcpy(&c->aspect, camera + 0x59c, 4);
    memcpy(c->window, camera + 0x5ac, sizeof(c->window));
    iss_temporal_slot_end(slot);
}

/* The marker (header + payload, see iss_temporal.h) for `frame`; out has ISS_TEMPORAL_MARKER_DWORDS dwords. */
static inline void iss_temporal_build_marker(const IssTemporalFrame *frame, uint32_t *out) {
    uint32_t *p = out + 1;
    out[0] = ISS_TEMPORAL_MARKER_HEADER;
    p[0] = ISS_TEMPORAL_MARKER_MAGIC | ISS_TEMPORAL_MARKER_KIND_RESOLVE;
    p[1] = ISS_TEMPORAL_MARKER_VERSION;
    p[2] = frame->seq;
    p[3] = (frame->reset ? ISS_TEMPORAL_FLAG_CAMERA_CUT : 0u) | ISS_TEMPORAL_FLAG_REVERSED_DEPTH;
    p[4] = frame->render_width;
    p[5] = frame->render_height;
    /* The window moves right by A: the content moves left, so x flips. y: the window's rows are the
     * game's clip y, which is up, and the image's y is down, so the content moves down: no flip. */
    const float jitter[2] = {-frame->jitter[0], frame->jitter[1]};
    memcpy(&p[6], jitter, sizeof(jitter));
    memcpy(&p[8], frame->camera.m470, sizeof(frame->camera.m470));
    memcpy(&p[24], &frame->camera.near_z, 4);
    memcpy(&p[25], &frame->camera.far_z, 4);
    memcpy(&p[26], &frame->camera.fov, 4);
    p[27] = (uint32_t)frame->aa_input.base;
    p[28] = (uint32_t)(frame->aa_input.base >> 32);
    p[29] = 0;
    p[30] = 0;
    p[31] = (uint32_t)frame->aa_output.base;
    p[32] = (uint32_t)(frame->aa_output.base >> 32);
}

/* Appends the marker to the game's draw command buffer `context` (a DrawCommandBuffer; its writer
 * is at +8: end +0x10, cursor +0x18, grow function +0x20, grow argument +0x28, the layout every
 * wrapper in the eboot's 0x61e000 range uses). 1 when written. */
typedef char (ISS_TEMPORAL_ABI *IssTemporalGrow)(void *writer, uint32_t dwords, void *argument);
static inline int iss_temporal_write_marker(unsigned char *context, const IssTemporalFrame *frame) {
    unsigned char *writer = context + 8;
    unsigned char *end, *cursor;
    memcpy(&end, writer + 8, sizeof(end));
    memcpy(&cursor, writer + 0x10, sizeof(cursor));
    if (!end || !cursor) return 0;
    if ((uint64_t)((end - cursor) >> 2) < ISS_TEMPORAL_MARKER_DWORDS) {
        IssTemporalGrow grow;
        void *argument;
        memcpy(&grow, writer + 0x18, sizeof(grow));
        memcpy(&argument, writer + 0x20, sizeof(argument));
        if (!grow || !grow(writer, ISS_TEMPORAL_MARKER_DWORDS, argument)) return 0;
        memcpy(&end, writer + 8, sizeof(end));
        memcpy(&cursor, writer + 0x10, sizeof(cursor));
        if ((uint64_t)((end - cursor) >> 2) < ISS_TEMPORAL_MARKER_DWORDS) return 0;
    }
    uint32_t packet[ISS_TEMPORAL_MARKER_DWORDS];
    iss_temporal_build_marker(frame, packet);
    memcpy(cursor, packet, sizeof(packet));
    cursor += sizeof(packet);
    memcpy(writer + 0x10, &cursor, sizeof(cursor));
    return 1;
}

/* The AA hook: the game's AA would run on `input` and write `output` (objects whose T# is at +0xb4). */
static ISS_TEMPORAL_ABI int iss_temporal_aa_replaced(unsigned char *context, const unsigned char *input,
                                                     const unsigned char *output) {
    IssTemporalSlot *slot = iss_temporal_current_slot();
    if (!slot || !context || !input || !output) return 0;
    IssTemporalFrame frame = slot->frame; /* only this thread writes the slot between the two hooks */
    iss_temporal_decode_image(input + ISS_TEMPORAL_AA_IMAGE_OFFSET, &frame.aa_input);
    iss_temporal_decode_image(output + ISS_TEMPORAL_AA_IMAGE_OFFSET, &frame.aa_output);
    if (!frame.aa_input.valid || !frame.aa_output.valid) return 0; /* not the objects we think: the game's AA runs */
    frame.command_context = (uint64_t)(uintptr_t)context;
    frame.aa_replaced = 1;
    if (!iss_temporal_write_marker(context, &frame)) return 0;
    iss_temporal_slot_begin(slot);
    slot->frame = frame;
    iss_temporal_slot_end(slot);
    return 1;
}

/* A consistent copy of frame `seq`; 0 when it is gone or not opened yet. */
static inline int iss_temporal_copy_frame(uint32_t seq, IssTemporalFrame *out) {
    if (!seq || seq > iss_temporal_state.seq) return 0;
    const IssTemporalSlot *slot = &iss_temporal_state.ring[seq % ISS_TEMPORAL_RING];
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t before = __atomic_load_n(&slot->lock, __ATOMIC_ACQUIRE);
        if (before & 1u) continue;
        memcpy(out, &slot->frame, sizeof(*out));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&slot->lock, __ATOMIC_ACQUIRE) == before) return out->seq == seq;
    }
    return 0;
}

/* ---------------------------------------------------------------------------- the stub assembler */
typedef struct IssTemporalTargets {
    uintptr_t active;          /* &iss_temporal_state.active */
    uintptr_t begin_frame, end_frame, aa_replaced; /* the host functions */
    uintptr_t camera_pointer;  /* image + ISS_TEMPORAL_CAMERA_POINTER */
    uintptr_t set_window;      /* image + ISS_TEMPORAL_SET_WINDOW */
    uintptr_t aa_mode;         /* image + ISS_TEMPORAL_AA_MODE */
    uintptr_t frame_object;    /* image + ISS_TEMPORAL_FRAME_OBJECT */
    uintptr_t jitter_fall, jitter_target; /* image + ISS_TEMPORAL_JITTER_FALL / _TARGET */
    uintptr_t aa_next, aa_skip;           /* image + ISS_TEMPORAL_AA_NEXT / _SKIP */
} IssTemporalTargets;

typedef struct IssTemporalAsm {
    unsigned char *code;
    unsigned size;
} IssTemporalAsm;

static inline void iss_asm_bytes(IssTemporalAsm *a, const unsigned char *bytes, unsigned length) {
    memcpy(a->code + a->size, bytes, length);
    a->size += length;
}
#define ISS_ASM(a, ...) do { static const unsigned char b_[] = {__VA_ARGS__}; iss_asm_bytes((a), b_, sizeof(b_)); } while (0)
static inline void iss_asm_u64(IssTemporalAsm *a, uint64_t value) {
    memcpy(a->code + a->size, &value, 8);
    a->size += 8;
}
/* jmp *0(%rip) ; .quad target */
static inline void iss_asm_jump(IssTemporalAsm *a, uintptr_t target) {
    ISS_ASM(a, 0xff, 0x25, 0, 0, 0, 0);
    iss_asm_u64(a, target);
}
/* movabs $value,%rax */
static inline void iss_asm_rax(IssTemporalAsm *a, uintptr_t value) {
    ISS_ASM(a, 0x48, 0xb8);
    iss_asm_u64(a, value);
}
/* A short conditional jump whose displacement is filled by iss_asm_here. */
static inline unsigned iss_asm_branch(IssTemporalAsm *a, unsigned char opcode) {
    unsigned char b[2] = {opcode, 0};
    iss_asm_bytes(a, b, 2);
    return a->size - 1;
}
static inline void iss_asm_here(IssTemporalAsm *a, unsigned branch) {
    a->code[branch] = (unsigned char)(a->size - (branch + 1));
}

enum { ISS_TEMPORAL_STUB_MAX = 192 };

/* The JITTER stub (see the top of the file). On entry rcx = &AA mode, rdi = camera, as at the site. */
static inline unsigned iss_temporal_build_jitter_stub(unsigned char *out, const IssTemporalTargets *t) {
    IssTemporalAsm a = {out, 0};
    ISS_ASM(&a, 0x49, 0xbb);                      /* movabs $active,%r11 */
    iss_asm_u64(&a, t->active);
    ISS_ASM(&a, 0x41, 0x80, 0x3b, 0x00);          /* cmpb $0,(%r11) */
    const unsigned to_on = iss_asm_branch(&a, 0x75); /* jne on */
    ISS_ASM(&a, 0x80, 0x39, 0x00);                /* cmpb $0,(%rcx)        the game's pair */
    const unsigned to_target = iss_asm_branch(&a, 0x74); /* je target */
    iss_asm_jump(&a, t->jitter_fall);
    iss_asm_here(&a, to_target);                  /* target: */
    const unsigned target = a.size;
    iss_asm_jump(&a, t->jitter_target);
    iss_asm_here(&a, to_on);                      /* on: */
    ISS_ASM(&a, 0x48, 0x8d, 0x7d, 0xc0);          /* lea -0x40(%rbp),%rdi   the game's window slot */
    iss_asm_rax(&a, t->begin_frame);
    ISS_ASM(&a, 0xff, 0xd0);                      /* call *%rax: begin_frame(window) */
    iss_asm_rax(&a, t->camera_pointer);
    ISS_ASM(&a, 0x48, 0x8b, 0x38);                /* mov (%rax),%rdi        camera */
    ISS_ASM(&a, 0x48, 0x8d, 0x75, 0xc0);          /* lea -0x40(%rbp),%rsi   window */
    iss_asm_rax(&a, t->set_window);
    ISS_ASM(&a, 0xff, 0xd0);                      /* call *%rax: FUN_00581a50(camera, window) */
    iss_asm_rax(&a, t->camera_pointer);
    ISS_ASM(&a, 0x48, 0x8b, 0x38);                /* mov (%rax),%rdi */
    iss_asm_rax(&a, t->end_frame);
    ISS_ASM(&a, 0xff, 0xd0);                      /* call *%rax: end_frame(camera) */
    iss_asm_rax(&a, t->aa_mode);
    ISS_ASM(&a, 0xc6, 0x00, 0x00);                /* movb $0,(%rax)         no T2x for the rest of the frame */
    const unsigned char back[2] = {0xeb, (unsigned char)(target - (a.size + 2))}; /* jmp target (backwards) */
    iss_asm_bytes(&a, back, 2);
    return a.size;
}

/* The AA stub. On entry r13 = the draw command buffer, r12 = the AA's input object, r15 = its output
 * object, r14 = the post object, as at the site. */
static inline unsigned iss_temporal_build_aa_stub(unsigned char *out, const IssTemporalTargets *t) {
    IssTemporalAsm a = {out, 0};
    ISS_ASM(&a, 0x49, 0xbb);                      /* movabs $active,%r11 */
    iss_asm_u64(&a, t->active);
    ISS_ASM(&a, 0x41, 0x80, 0x3b, 0x00);          /* cmpb $0,(%r11) */
    const unsigned to_orig_1 = iss_asm_branch(&a, 0x74); /* je orig */
    iss_asm_rax(&a, t->frame_object);
    ISS_ASM(&a, 0x80, 0xb8, 0x05, 0x0f, 0xdf, 0x00, 0x00); /* cmpb $0,0xdf0f05(%rax): the game's own no-AA flag */
    const unsigned to_orig_2 = iss_asm_branch(&a, 0x75);   /* jne orig */
    ISS_ASM(&a, 0x41, 0x80, 0xbe, 0x8d, 0xeb, 0x01, 0x00, 0x00); /* cmpb $0,0x1eb8d(%r14): its second one */
    const unsigned to_orig_3 = iss_asm_branch(&a, 0x75);   /* jne orig */
    ISS_ASM(&a, 0x4c, 0x89, 0xef);                /* mov %r13,%rdi */
    ISS_ASM(&a, 0x4c, 0x89, 0xe6);                /* mov %r12,%rsi */
    ISS_ASM(&a, 0x4c, 0x89, 0xfa);                /* mov %r15,%rdx */
    iss_asm_rax(&a, t->aa_replaced);
    ISS_ASM(&a, 0xff, 0xd0);                      /* call *%rax: aa_replaced(context, input, output) */
    ISS_ASM(&a, 0x84, 0xc0);                      /* test %al,%al */
    const unsigned to_orig_4 = iss_asm_branch(&a, 0x74); /* je orig: it could not take over */
    iss_asm_jump(&a, t->aa_skip);
    iss_asm_here(&a, to_orig_1);
    iss_asm_here(&a, to_orig_2);
    iss_asm_here(&a, to_orig_3);
    iss_asm_here(&a, to_orig_4);                  /* orig: the two instructions the jump replaced */
    iss_asm_rax(&a, t->frame_object);
    ISS_ASM(&a, 0x8a, 0x80, 0x05, 0x0f, 0xdf, 0x00); /* mov 0xdf0f05(%rax),%al */
    iss_asm_jump(&a, t->aa_next);
    return a.size;
}

/* jmp rel32 at `site`, nops after it up to `length`; 0 (nothing written) when out of range. */
static inline int iss_temporal_jump_in_range(const unsigned char *site, const unsigned char *stub) {
    const int64_t delta = (int64_t)(intptr_t)stub - (int64_t)(intptr_t)(site + 5);
    return delta >= INT32_MIN && delta <= INT32_MAX;
}
static inline void iss_temporal_write_jump(unsigned char *site, unsigned length, const unsigned char *stub) {
    const int32_t relative = (int32_t)((int64_t)(intptr_t)stub - (int64_t)(intptr_t)(site + 5));
    site[0] = 0xe9;
    memcpy(site + 1, &relative, 4);
    memset(site + 5, 0x90, length - 5);
}

/* Builds both stubs into `stubs` (2 * ISS_TEMPORAL_STUB_MAX bytes, written before they are made
 * executable) and writes the two jumps into `image`. 0, with the image untouched, when the code
 * is not this eboot's or a stub is out of reach of a rel32 jump. */
static inline int iss_temporal_install_hooks(unsigned char *image, uint64_t size, unsigned char *stubs) {
    if (!iss_temporal_signature_match(image, size)) return 0;
    unsigned char *jitter_stub = stubs, *aa_stub = stubs + ISS_TEMPORAL_STUB_MAX;
    if (!iss_temporal_jump_in_range(image + ISS_TEMPORAL_JITTER_SITE, jitter_stub) ||
        !iss_temporal_jump_in_range(image + ISS_TEMPORAL_AA_SITE, aa_stub)) return 0;
    IssTemporalTargets t;
    t.active = (uintptr_t)&iss_temporal_state.active;
    t.begin_frame = (uintptr_t)&iss_temporal_begin_frame;
    t.end_frame = (uintptr_t)&iss_temporal_end_frame;
    t.aa_replaced = (uintptr_t)&iss_temporal_aa_replaced;
    t.camera_pointer = (uintptr_t)(image + ISS_TEMPORAL_CAMERA_POINTER);
    t.set_window = (uintptr_t)(image + ISS_TEMPORAL_SET_WINDOW);
    t.aa_mode = (uintptr_t)(image + ISS_TEMPORAL_AA_MODE);
    t.frame_object = (uintptr_t)(image + ISS_TEMPORAL_FRAME_OBJECT);
    t.jitter_fall = (uintptr_t)(image + ISS_TEMPORAL_JITTER_FALL);
    t.jitter_target = (uintptr_t)(image + ISS_TEMPORAL_JITTER_TARGET);
    t.aa_next = (uintptr_t)(image + ISS_TEMPORAL_AA_NEXT);
    t.aa_skip = (uintptr_t)(image + ISS_TEMPORAL_AA_SKIP);
    if (iss_temporal_build_jitter_stub(jitter_stub, &t) > ISS_TEMPORAL_STUB_MAX ||
        iss_temporal_build_aa_stub(aa_stub, &t) > ISS_TEMPORAL_STUB_MAX) return 0;
    iss_temporal_state.image = image;
    iss_temporal_write_jump(image + ISS_TEMPORAL_JITTER_SITE, ISS_TEMPORAL_JITTER_LENGTH, jitter_stub);
    iss_temporal_write_jump(image + ISS_TEMPORAL_AA_SITE, ISS_TEMPORAL_AA_LENGTH, aa_stub);
    iss_temporal_state.installed = 1;
    iss_temporal_update_active();
    return 1;
}

#endif
