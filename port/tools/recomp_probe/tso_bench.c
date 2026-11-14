// SPDX-License-Identifier: GPL-2.0-or-later
// Static recompilation risk probe: what x86 memory ordering (TSO) costs natively on Apple Silicon.
// Rosetta runs x86 code with the CPU's hardware TSO mode, which macOS does not give to ordinary
// processes; recompiled code would make every guest load acquire (LDAPR) and every store release
// (STLR) instead. Game-like kernels, built three ways:
//   clang -O2 -arch x86_64 tso_bench.c -o tso_x86        (under Rosetta: today's path)
//   clang -O2 -arch arm64  tso_bench.c -o tso_arm         (native, plain accesses: upper bound)
//   clang -O2 -arch arm64 -DTSO tso_bench.c -o tso_tso    (native, acquire loads / release stores)
// Native C from source is an upper bound for recompiled code (no lifted flags or spilled guest
// registers), so the real gap would be smaller than tso_arm vs tso_x86.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef TSO
#define LD(p) ({ __typeof__(*(p)) ld_v_; __atomic_load((p), &ld_v_, __ATOMIC_ACQUIRE); ld_v_; })
#define ST(p, v) do { __typeof__(*(p)) st_v_ = (v); __atomic_store((p), &st_v_, __ATOMIC_RELEASE); } while (0)
#else
#define LD(p) (*(p))
#define ST(p, v) (*(p) = (v))
#endif

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

typedef struct Node { struct Node *next; uint64_t value; uint64_t pad[2]; } Node;

static uint64_t chase(Node *nodes, size_t n, int rounds) {
    uint64_t sum = 0;
    for (int r = 0; r < rounds; ++r) {
        Node *p = &nodes[0];
        for (size_t i = 0; i < n; ++i) {
            uint64_t v = LD(&p->value);
            ST(&p->value, v * 3 + i);
            sum += v;
            p = LD(&p->next);
        }
    }
    return sum;
}

typedef struct Body { float pos[4], vel[4], acc[4]; uint32_t flags, pad[3]; } Body;

static float physics(Body *b, size_t n, int rounds) {
    float total = 0;
    for (int r = 0; r < rounds; ++r) {
        for (size_t i = 0; i < n; ++i) {
            if (LD(&b[i].flags) & 1) continue;
            for (int k = 0; k < 3; ++k) {
                float v = LD(&b[i].vel[k]) + LD(&b[i].acc[k]) * 0.016f;
                ST(&b[i].vel[k], v * 0.999f);
                float p = LD(&b[i].pos[k]) + v * 0.016f;
                if (p < -100.f || p > 100.f) { p = -p * 0.5f; ST(&b[i].flags, LD(&b[i].flags) ^ 2); }
                ST(&b[i].pos[k], p);
                total += p;
            }
        }
    }
    return total;
}

static uint64_t hashing(uint64_t *keys, uint32_t *vals, size_t cap, int ops) {
    uint64_t x = 88172645463325252ull, found = 0;
    for (int i = 0; i < ops; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint64_t key = (x % (cap / 2)) + 1;
        size_t h = (key * 0x9E3779B97F4A7C15ull) & (cap - 1);
        for (;;) {
            uint64_t k = LD(&keys[h]);
            if (k == key) { ST(&vals[h], LD(&vals[h]) + 1); ++found; break; }
            if (k == 0) { ST(&keys[h], key); ST(&vals[h], 1u); break; }
            h = (h + 1) & (cap - 1);
        }
    }
    return found;
}

static float matrices(float *m, size_t count, int rounds) {
    float acc = 0;
    for (int r = 0; r < rounds; ++r) {
        for (size_t i = 0; i + 2 < count; ++i) {
            float *a = &m[i * 16], *b = &m[(i + 1) * 16], *c = &m[(i + 2) * 16];
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col) {
                    float s = 0;
                    for (int k = 0; k < 4; ++k) s += LD(&a[row * 4 + k]) * LD(&b[k * 4 + col]);
                    ST(&c[row * 4 + col], s * 0.5f + 0.25f);
                    acc += s;
                }
        }
    }
    return acc;
}

int main(void) {
    const size_t n = 1 << 20;
    Node *nodes = calloc(n, sizeof(Node));
    size_t *perm = malloc(n * sizeof(size_t));
    for (size_t i = 0; i < n; ++i) perm[i] = i;
    uint64_t s = 12345;
    for (size_t i = n - 1; i > 0; --i) { s = s * 6364136223846793005ull + 1; size_t j = (s >> 33) % (i + 1); size_t t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
    for (size_t i = 0; i < n; ++i) { nodes[perm[i]].next = &nodes[perm[(i + 1) % n]]; nodes[i].value = i; }
    const size_t nb = 1 << 16;
    Body *bodies = calloc(nb, sizeof(Body));
    for (size_t i = 0; i < nb; ++i) for (int k = 0; k < 3; ++k) { bodies[i].pos[k] = (float)(i % 97); bodies[i].vel[k] = 1.f; bodies[i].acc[k] = -0.5f * (float)((i % 3) - 1); }
    const size_t cap = 1 << 20;
    uint64_t *keys = calloc(cap, 8); uint32_t *vals = calloc(cap, 4);
    const size_t nm = 4096;
    float *mats = malloc(nm * 16 * sizeof(float));
    for (size_t i = 0; i < nm * 16; ++i) mats[i] = (float)(i % 7) * 0.1f;
    double t0, t[4]; volatile double sink = 0;
    t0 = now(); sink += (double)chase(nodes, n / 16, 128); t[0] = now() - t0;
    t0 = now(); sink += physics(bodies, nb, 2000); t[1] = now() - t0;
    t0 = now(); sink += (double)hashing(keys, vals, cap, 80000000); t[2] = now() - t0;
    t0 = now(); sink += matrices(mats, nm, 4000); t[3] = now() - t0;
    printf("chase %.3f s  physics %.3f s  hash %.3f s  matrix %.3f s  total %.3f s  (%g)\n",
           t[0], t[1], t[2], t[3], t[0] + t[1] + t[2] + t[3], (double)sink);
    return 0;
}
