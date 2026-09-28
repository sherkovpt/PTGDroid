/* Guest heap: one allocator for all RHeaps. Block = [u32 size][u32 magic] + payload.
 * Free lists live on the host so stray guest writes can't corrupt allocator state. */
#include "hle.h"
#include <stdlib.h>

#define HDR 8u
#define MAGIC_USED 0xA110CA7Eu
#define MAGIC_FREE 0xF4EEB10Cu
#define SMALL_MAX 4096u
#define NCLASS (SMALL_MAX / 8 + 1)
#define COMMIT_STEP 0x1000000u

typedef struct { uint32_t *v; uint32_t n, cap; } Vec;

static Vec small[NCLASS];
static Vec large; /* free large blocks (payload addresses) */
static uint32_t bump = HEAP_BASE, committed = HEAP_BASE, used_bytes;

static void vpush(Vec *v, uint32_t x) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 64; v->v = realloc(v->v, v->cap * 4); }
    v->v[v->n++] = x;
}

void heap_init(void) {}

static uint32_t round8(uint32_t n) { return n ? (n + 7) & ~7u : 8; }

static uint32_t carve(uint32_t size) {
    uint32_t need = HDR + size;
    if (bump + need > HEAP_LIMIT) return 0;
    while (bump + need > committed) {
        mem_commit(committed, COMMIT_STEP);
        committed += COMMIT_STEP;
    }
    uint32_t p = bump + HDR;
    bump += need;
    wr32(p - 8, size);
    return p;
}

uint32_t galloc(uint32_t n) {
    if (n > 0x10000000u) return 0;
    uint32_t size = round8(n), p = 0;
    if (size <= SMALL_MAX) {
        Vec *v = &small[size / 8];
        if (v->n) p = v->v[--v->n];
    } else {
        uint32_t best = UINT32_MAX, bi = 0;
        for (uint32_t i = 0; i < large.n; i++) {
            uint32_t s = rd32(large.v[i] - 8);
            if (s >= size && s < best) { best = s; bi = i; if (s == size) break; }
        }
        if (best != UINT32_MAX) {
            p = large.v[bi];
            large.v[bi] = large.v[--large.n];
            if (best - size >= SMALL_MAX + HDR) { /* split the tail off */
                uint32_t rest = p + size + HDR;
                wr32(rest - 8, best - size - HDR);
                wr32(rest - 4, MAGIC_FREE);
                vpush(&large, rest);
                wr32(p - 8, size);
            }
        }
    }
    if (!p) p = carve(size);
    if (!p) return 0;
    wr32(p - 4, MAGIC_USED);
    used_bytes += rd32(p - 8);
    return p;
}

uint32_t gallocz(uint32_t n) {
    uint32_t p = galloc(n);
    if (p) memset(MEM + p, 0, rd32(p - 8));
    return p;
}

void gfree(uint32_t p) {
    if (!p) return;
    if (rd32(p - 4) != MAGIC_USED) {
        LOG("heap: free of invalid pointer %08x (magic %08x)", p, rd32(p - 4));
        return;
    }
    uint32_t size = rd32(p - 8);
    wr32(p - 4, MAGIC_FREE);
    used_bytes -= size;
    if (size <= SMALL_MAX) vpush(&small[size / 8], p);
    else vpush(&large, p);
}

uint32_t galloc_len(uint32_t p) { return p ? rd32(p - 8) : 0; }

uint32_t grealloc(uint32_t p, uint32_t n) {
    if (!p) return galloc(n);
    uint32_t old = rd32(p - 8);
    if (round8(n) <= old) return p;
    uint32_t q = galloc(n);
    if (!q) return 0;
    memcpy(MEM + q, MEM + p, old);
    gfree(p);
    return q;
}

uint32_t heap_used(void) { return used_bytes; }
