/* inFAMOUS Second Son (CUSA00223 01.07) performance patches, from axylpup/shadPS4
 * (src/common/memory_patcher.cpp, GPL-2.0-or-later; notes there: study-performance-mode-2026-10-20,
 * one-frame-late-particles-2026-10-12, study-fight-dips-2026-10-15). Offsets are eboot virtual
 * addresses (image = eboot vaddr 0). Every site is compared with the expected bytes first; one
 * mismatch refuses that patch (all or nothing).
 *
 * TLG_PERF_MODE=low|office: the game's own lower quality, through its 21-row quality ladder at
 *   0xda08c0 (20 bytes a row: LOD scale for its 7 distance bands, sun-shadow cascade offset, ...),
 *   which it climbs only when a frame overflows its GPU command memory. The preset is written into
 *   row 0 so the game's own path runs it. low: LOD 0.7, one sun-shadow cascade less, particles at
 *   half resolution, 2 high-res texture blocks; office: LOD 0.5, particles at quarter resolution,
 *   no high-res texture blocks, half the pedestrians.
 * TLG_PARTICLE_LATE_READ=1: the particle control blocks (survivor and light counts, CPU collision)
 *   are read at the start of the next frame instead of right after kicking this frame's particle
 *   chain, so the game's main thread no longer waits for that GPU work mid-frame (particles one
 *   frame late).
 * TLG_PARTICLE_LIGHT_ONE_SUBMIT=1 (with the late read): the per-light ring submit (25-74 a fight
 *   frame) becomes a NOP; the copies go out with the frame's first particle submit. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t offset; const unsigned char *bytes; uint32_t length; } IssPerfBytes;

static int iss_perf_matches(const unsigned char *image, uint64_t size, const IssPerfBytes *b, unsigned n,
                            const char *what) {
    for (unsigned i = 0; i < n; ++i) {
        if (b[i].offset + (uint64_t)b[i].length > size || memcmp(image + b[i].offset, b[i].bytes, b[i].length)) {
            printf("Performance patches: %s refused, eboot+0x%x differs (CUSA00223 01.07 only)\n", what, b[i].offset);
            return 0;
        }
    }
    return 1;
}

static void iss_perf_write(unsigned char *image, const IssPerfBytes *b, unsigned n) {
    for (unsigned i = 0; i < n; ++i) memcpy(image + b[i].offset, b[i].bytes, b[i].length);
}

static int iss_perf_mode(unsigned char *image, uint64_t size) {
    const char *env = getenv("TLG_PERF_MODE");
    const int low = env && (!strcmp(env, "low") || !strcmp(env, "1"));
    const int office = env && (!strcmp(env, "office") || !strcmp(env, "2"));
    if (!low && !office) return 0;
    static const unsigned char row0[20] = {0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24,
                                           0x74, 0x49, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const unsigned char je[2] = {0x74, 0x0f}, nops[2] = {0x90, 0x90}, zero4[4] = {0}, four[1] = {4},
                               one_f[4] = {0x00, 0x00, 0x80, 0x3f}, half_f[4] = {0x00, 0x00, 0x00, 0x3f};
    unsigned char row[20], level[4] = {(unsigned char)(low ? 1 : 2), 0, 0, 0}, slots[1] = {(unsigned char)(low ? 2 : 0)};
    memcpy(row, row0, sizeof row);
    const float lod = low ? 0.7f : 0.5f;
    const int32_t cascades = -1;
    memcpy(row, &lod, 4);
    memcpy(row + 4, &cascades, 4);
    const IssPerfBytes original[] = {
        {0xda08c0, row0, 20}, {0x3ed6b0, je, 2}, {0x3ed6b8, zero4, 4}, {0xfe9a2, four, 1}, {0xfe9b2, four, 1},
        {0x40ca01, one_f, 4}, {0x40c83f, one_f, 4},
    };
    const IssPerfBytes patched[] = {
        {0xda08c0, row, 20},      /* quality ladder row 0 */
        {0x3ed6b0, nops, 2},      /* particle level: always set */
        {0x3ed6b8, level, 4},     /* particle level (resolution 1.0 / 0.5 / 0.25) */
        {0xfe9a2, slots, 1},      /* high-res texture blocks (mov) */
        {0xfe9b2, slots, 1},      /* high-res texture blocks (cmp) */
        {0x40ca01, half_f, 4},    /* pedestrian ratio 0 (office) */
        {0x40c83f, half_f, 4},    /* pedestrian ratio 1 (office) */
    };
    const unsigned n = office ? 7 : 5;
    if (!iss_perf_matches(image, size, original, n, "TLG_PERF_MODE")) return 0;
    iss_perf_write(image, patched, n);
    printf("Performance patches: TLG_PERF_MODE=%s: LOD scale %.1f, one sun-shadow cascade less, particles at %s "
           "resolution, %u high-res texture blocks%s\n",
           low ? "low" : "office", lod, low ? "half" : "quarter", (unsigned)slots[0], office ? ", half the pedestrians" : "");
    return 1;
}

static int iss_particle_late_read(unsigned char *image, uint64_t size) {
    const char *env = getenv("TLG_PARTICLE_LATE_READ");
    if (!env || env[0] != '1') return 0;
    /* FUN_8003ee160 (no references): 43 code bytes + 5 pad; the main loop around 0x29cc84 and
     * 0x29cd14; the kept end-of-frame call at 0x29cd53. */
    static const unsigned char cave_old[48] = {
        0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8b, 0x5f, 0x58, 0xeb, 0x13, 0x0f, 0x1f, 0x40, 0x00,
        0x48, 0x89, 0xdf, 0xe8, 0x58, 0x5d, 0x00, 0x00, 0x48, 0x8b, 0x9b, 0xb0, 0x00, 0x00, 0x00, 0x48,
        0x85, 0xdb, 0x75, 0xec, 0x48, 0x83, 0xc4, 0x08, 0x5b, 0x5d, 0xc3, 0x90, 0x90, 0x90, 0x90, 0x90};
    static const unsigned char loop_old[31] = {
        0xe8, 0x47, 0xec, 0x17, 0x00, 0x48, 0x8d, 0x05, 0x40, 0xf0, 0xc7, 0x00, 0x48, 0x8d, 0x3d, 0x79,
        0x1e, 0xaa, 0x07, 0x48, 0x8b, 0xb0, 0xa0, 0xdc, 0x01, 0x00, 0xe8, 0x2d, 0x1b, 0x15, 0x00};
    static const unsigned char read_old[15] = {
        0x48, 0x8d, 0x1d, 0xf5, 0x1d, 0xaa, 0x07, 0x48, 0x89, 0xdf, 0xe8, 0xcd, 0x11, 0x15, 0x00};
    static const unsigned char join_old[8] = {0x48, 0x89, 0xdf, 0xe8, 0x15, 0x17, 0x15, 0x00};
    static const unsigned char cave_new[45] = {
        0x48, 0x8d, 0x1d, 0xa9, 0x09, 0x95, 0x07, 0x48, 0x89, 0xdf, 0xe8, 0x81, 0xfd, 0xff, 0xff,
        0x48, 0x89, 0xdf, 0xe8, 0xf9, 0x02, 0x00, 0x00, 0x48, 0x8d, 0x05, 0x52, 0xdb, 0xb2, 0x00,
        0x48, 0x89, 0xdf, 0x48, 0x8b, 0xb0, 0xa0, 0xdc, 0x01, 0x00, 0xe9, 0x11, 0xeb, 0xea, 0xff};
    static const unsigned char jump_new[21] = {0xe9, 0xd2, 0x14, 0x15, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                                              0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    static const unsigned char nop5[5] = {0x0f, 0x1f, 0x44, 0x00, 0x00};
    const IssPerfBytes original[] = {
        {0x3ee160, cave_old, 48}, {0x29cc84, loop_old, 31}, {0x29cd14, read_old, 15}, {0x29cd53, join_old, 8}};
    const IssPerfBytes patched[] = {{0x3ee160, cave_new, 45}, {0x29cc89, jump_new, 21}, {0x29cd1e, nop5, 5}};
    if (!iss_perf_matches(image, size, original, 4, "TLG_PARTICLE_LATE_READ")) return 0;
    iss_perf_write(image, patched, 3);
    puts("Performance patches: TLG_PARTICLE_LATE_READ: particle control blocks read one frame later "
         "(no mid-frame wait for the particle GPU work)");
    return 1;
}

static void iss_particle_light_one_submit(unsigned char *image, uint64_t size, int late_read) {
    const char *env = getenv("TLG_PARTICLE_LIGHT_ONE_SUBMIT");
    if (!env || env[0] != '1') return;
    if (!late_read) {
        puts("Performance patches: TLG_PARTICLE_LIGHT_ONE_SUBMIT refused: needs TLG_PARTICLE_LATE_READ applied");
        return;
    }
    static const unsigned char call_old[5] = {0xe8, 0xa5, 0x51, 0xd7, 0xff}; /* call 0x1691e0 */
    static const unsigned char nop5[5] = {0x0f, 0x1f, 0x44, 0x00, 0x00};
    const IssPerfBytes original[] = {{0x3f4036, call_old, 5}};
    const IssPerfBytes patched[] = {{0x3f4036, nop5, 5}};
    if (!iss_perf_matches(image, size, original, 1, "TLG_PARTICLE_LIGHT_ONE_SUBMIT")) return;
    iss_perf_write(image, patched, 1);
    puts("Performance patches: TLG_PARTICLE_LIGHT_ONE_SUBMIT: one ring submit for the particle lights");
}

static void iss_perf_patches(unsigned char *image, uint64_t size) {
    iss_perf_mode(image, size);
    const int late = iss_particle_late_read(image, size);
    iss_particle_light_one_submit(image, size, late);
}
