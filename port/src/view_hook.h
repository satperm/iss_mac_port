/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ISS (CUSA00223 01.07 eboot): draw distance, LOD and clip planes.
 * Offsets are eboot virtual addresses (= image offsets). Found with Ghidra (PORT_ISS.md, 16/10):
 *   - camera LOD factor (camera+0x5a8) = FOV term * region LOD_ADJUST * kViewLodTable[level].lod;
 *     every LOD band distance (kViewBands) is multiplied by it. `level` (0..20) is the game's
 *     dynamic quality step, raised when the frame's draw lists get too big.
 *   - region settings (script NEAR_CLIP / LOD_ADJUST / ...) blend into a block whose LOD_ADJUST is
 *     read by the two kViewRegionSites (vmulss 0x9c(%rax)); FADE_DISTANCE (region+0x2f0) is the
 *     distance over which a region's settings fade in (kViewFadeSites, vdivss 0x2f0(%rbx)).
 *   - new cameras copy the near (10) and far (1e7) clip planes from two globals.
 * Data writes need no hook; the two code sites jump to stubs that read the menu's floats. */
#ifndef TLG_VIEW_HOOK_H
#define TLG_VIEW_HOOK_H
#include <stdint.h>
#include <string.h>

#define TLG_VIEW_LOD_TABLE 0xda08c0u /* 21 rows of 0x14 bytes: float lod, int shadow cascades
                                        delta, float max distance, float, int */
#define TLG_VIEW_LOD_ROWS 21u
#define TLG_VIEW_LOD_STRIDE 0x14u
#define TLG_VIEW_BANDS 0xdfaa34u      /* 6 floats: LOD band distances (a 7th, FLT_MAX, follows) */
#define TLG_VIEW_BAND_COUNT 6u
#define TLG_VIEW_NEAR_CLIP 0xdf9e64u  /* float 10.0: new cameras' near plane, the regions' default */
#define TLG_VIEW_FAR_CLIP 0xdf9e68u   /* float 1e7: new cameras' far plane */
#define TLG_VIEW_SITE_LENGTH 8u

static const float tlg_view_lod_original[TLG_VIEW_LOD_ROWS] = {
    1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.5f, 0.4f, 0.4f, 0.3f, 0.2f,
    0.2f, 0.2f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
static const float tlg_view_band_original[TLG_VIEW_BAND_COUNT] = {400.0f, 750.0f, 1500.0f,
                                                                   3500.0f, 6000.0f, 15000.0f};
#define TLG_VIEW_NEAR_ORIGINAL 10.0f
#define TLG_VIEW_FAR_ORIGINAL 1.0e7f

/* vmulss 0x9c(%rax),%xmm0,%xmm0, then movslq 0x1dca8(%rcx),%rax: rax is dead after the multiply. */
static const uint32_t tlg_view_region_sites[2] = {0x581088u, 0x5811ffu};
static const unsigned char tlg_view_region_code[15] = {0xc5, 0xfa, 0x59, 0x80, 0x9c, 0, 0, 0,
                                                       0x48, 0x63, 0x81, 0xa8, 0xdc, 0x01, 0x00};
/* vdivss 0x2f0(%rbx),%xmm0,%xmm0 (the region's FADE_DISTANCE) */
static const uint32_t tlg_view_fade_sites[2] = {0x1bae75u, 0x1bb1e9u};
static const unsigned char tlg_view_fade_code[8] = {0xc5, 0xfa, 0x5e, 0x83, 0xf0, 0x02, 0, 0};

static inline int tlg_view_float_is(const unsigned char *image, uint32_t at, float value) {
    float v;
    memcpy(&v, image + at, 4);
    return v == value;
}

/* The code and the data the hooks rely on, all unchanged (this eboot build). */
static inline int tlg_view_signature_match(const unsigned char *image, uint64_t size) {
    if (size < TLG_VIEW_BANDS + 4 * TLG_VIEW_BAND_COUNT) return 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (memcmp(image + tlg_view_region_sites[i], tlg_view_region_code, sizeof(tlg_view_region_code)) ||
            memcmp(image + tlg_view_fade_sites[i], tlg_view_fade_code, sizeof(tlg_view_fade_code))) return 0;
    }
    for (unsigned r = 0; r < TLG_VIEW_LOD_ROWS; ++r)
        if (!tlg_view_float_is(image, TLG_VIEW_LOD_TABLE + r * TLG_VIEW_LOD_STRIDE, tlg_view_lod_original[r])) return 0;
    for (unsigned b = 0; b < TLG_VIEW_BAND_COUNT; ++b)
        if (!tlg_view_float_is(image, TLG_VIEW_BANDS + 4 * b, tlg_view_band_original[b])) return 0;
    return tlg_view_float_is(image, TLG_VIEW_NEAR_CLIP, TLG_VIEW_NEAR_ORIGINAL) &&
           tlg_view_float_is(image, TLG_VIEW_FAR_CLIP, TLG_VIEW_FAR_ORIGINAL);
}

static inline unsigned tlg_view_jump_back(unsigned char *out, uintptr_t resume) {
    const unsigned char jmp[6] = {0xff, 0x25, 0, 0, 0, 0}; /* jmp *0(%rip) */
    memcpy(out, jmp, 6);
    memcpy(out + 6, &resume, 8);
    return 14;
}

/* LOD_ADJUST weight: xmm0 *= 1 + (adjust - 1) * weight; params = {weight, 1.0f}. ymm1 is saved
 * below the red zone, rax is free (the next instruction loads it). */
static inline unsigned tlg_view_build_region_stub(unsigned char *out, uintptr_t params, uintptr_t resume) {
    static const unsigned char head[] = {
        0x48, 0x8d, 0xa4, 0x24, 0x60, 0xff, 0xff, 0xff, /* lea -0xa0(%rsp),%rsp */
        0xc5, 0xfc, 0x11, 0x0c, 0x24,                   /* vmovups %ymm1,(%rsp) */
        0xc5, 0xfa, 0x10, 0x88, 0x9c, 0, 0, 0,          /* vmovss 0x9c(%rax),%xmm1 */
        0x48, 0xb8};                                    /* movabs $params,%rax */
    static const unsigned char tail[] = {
        0xc5, 0xf2, 0x5c, 0x48, 0x04,                   /* vsubss 0x4(%rax),%xmm1,%xmm1 */
        0xc5, 0xf2, 0x59, 0x08,                         /* vmulss (%rax),%xmm1,%xmm1 */
        0xc5, 0xf2, 0x58, 0x48, 0x04,                   /* vaddss 0x4(%rax),%xmm1,%xmm1 */
        0xc5, 0xfa, 0x59, 0xc1,                         /* vmulss %xmm1,%xmm0,%xmm0 */
        0xc5, 0xfc, 0x10, 0x0c, 0x24,                   /* vmovups (%rsp),%ymm1 */
        0x48, 0x8d, 0xa4, 0x24, 0xa0, 0, 0, 0};         /* lea 0xa0(%rsp),%rsp */
    unsigned n = 0;
    memcpy(out + n, head, sizeof(head)); n += sizeof(head);
    memcpy(out + n, &params, 8); n += 8;
    memcpy(out + n, tail, sizeof(tail)); n += sizeof(tail);
    return n + tlg_view_jump_back(out + n, resume);
}

/* FADE_DISTANCE scale: the game's division, then xmm0 *= *inverse_scale (rax saved). */
static inline unsigned tlg_view_build_fade_stub(unsigned char *out, uintptr_t inverse_scale, uintptr_t resume) {
    static const unsigned char head[] = {
        0xc5, 0xfa, 0x5e, 0x83, 0xf0, 0x02, 0, 0,       /* vdivss 0x2f0(%rbx),%xmm0,%xmm0 */
        0x48, 0x8d, 0x64, 0x24, 0x80,                   /* lea -0x80(%rsp),%rsp */
        0x50,                                           /* push %rax */
        0x48, 0xb8};                                    /* movabs $inverse_scale,%rax */
    static const unsigned char tail[] = {
        0xc5, 0xfa, 0x59, 0x00,                         /* vmulss (%rax),%xmm0,%xmm0 */
        0x58,                                           /* pop %rax */
        0x48, 0x8d, 0xa4, 0x24, 0x80, 0, 0, 0};         /* lea 0x80(%rsp),%rsp */
    unsigned n = 0;
    memcpy(out + n, head, sizeof(head)); n += sizeof(head);
    memcpy(out + n, &inverse_scale, 8); n += 8;
    memcpy(out + n, tail, sizeof(tail)); n += sizeof(tail);
    return n + tlg_view_jump_back(out + n, resume);
}
#endif
