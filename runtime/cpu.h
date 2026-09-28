#pragma once
#include <stdint.h>
#include <string.h>

#ifdef _MSC_VER
#include <intrin.h>
#define FORCEINLINE __forceinline
#define NORETURN __declspec(noreturn)
#else
#define FORCEINLINE inline __attribute__((always_inline))
#define NORETURN __attribute__((noreturn))
#endif

typedef struct CPU {
    uint32_t r[16];
    uint8_t n, z, c, v;
} CPU;

typedef void (*GuestFn)(CPU *c);

/* Guest memory is a flat 4 GiB reservation; guest address a lives at MEM + a. */
extern uint8_t *MEM;

#define R(i) (c->r[i])

static FORCEINLINE uint32_t ror32(uint32_t v, uint32_t s) {
    s &= 31;
    return s ? (v >> s) | (v << (32 - s)) : v;
}

/* ARMv4 LDR of a misaligned address rotates the aligned word. */
static FORCEINLINE uint32_t rd32(uint32_t a) {
    uint32_t v;
    memcpy(&v, MEM + (a & ~3u), 4);
    return (a & 3) ? ror32(v, 8 * (a & 3)) : v;
}
static FORCEINLINE uint32_t rd16(uint32_t a) { uint16_t v; memcpy(&v, MEM + (a & ~1u), 2); return v; }
static FORCEINLINE uint32_t rd8(uint32_t a) { return MEM[a]; }
static FORCEINLINE void wr32(uint32_t a, uint32_t v) { memcpy(MEM + (a & ~3u), &v, 4); }
static FORCEINLINE void wr16(uint32_t a, uint32_t v) { uint16_t h = (uint16_t)v; memcpy(MEM + (a & ~1u), &h, 2); }
static FORCEINLINE void wr8(uint32_t a, uint32_t v) { MEM[a] = (uint8_t)v; }

#define NZ(x) do { uint32_t _t = (x); c->n = _t >> 31; c->z = _t == 0; } while (0)

#define C_EQ (c->z)
#define C_NE (!c->z)
#define C_CS (c->c)
#define C_CC (!c->c)
#define C_MI (c->n)
#define C_PL (!c->n)
#define C_VS (c->v)
#define C_VC (!c->v)
#define C_HI (c->c && !c->z)
#define C_LS (!c->c || c->z)
#define C_GE (c->n == c->v)
#define C_LT (c->n != c->v)
#define C_GT (!c->z && c->n == c->v)
#define C_LE (c->z || c->n != c->v)

/* Register-specified shifts (amount from low byte of Rs), with and without carry-out. */
static FORCEINLINE uint32_t lsl_r(uint32_t v, uint32_t s) { s &= 0xFF; return s >= 32 ? 0 : v << s; }
static FORCEINLINE uint32_t lsr_r(uint32_t v, uint32_t s) { s &= 0xFF; return s >= 32 ? 0 : v >> s; }
static FORCEINLINE uint32_t asr_r(uint32_t v, uint32_t s) { s &= 0xFF; return s >= 32 ? (uint32_t)((int32_t)v >> 31) : (uint32_t)((int32_t)v >> s); }
static FORCEINLINE uint32_t ror_r(uint32_t v, uint32_t s) { return ror32(v, s & 0xFF); }

static FORCEINLINE uint32_t lsl_rc(uint32_t v, uint32_t s, uint8_t *cf) {
    s &= 0xFF;
    if (s == 0) return v;
    if (s < 32) { *cf = (v >> (32 - s)) & 1; return v << s; }
    *cf = s == 32 ? (v & 1) : 0;
    return 0;
}
static FORCEINLINE uint32_t lsr_rc(uint32_t v, uint32_t s, uint8_t *cf) {
    s &= 0xFF;
    if (s == 0) return v;
    if (s < 32) { *cf = (v >> (s - 1)) & 1; return v >> s; }
    *cf = s == 32 ? v >> 31 : 0;
    return 0;
}
static FORCEINLINE uint32_t asr_rc(uint32_t v, uint32_t s, uint8_t *cf) {
    s &= 0xFF;
    if (s == 0) return v;
    if (s < 32) { *cf = ((int32_t)v >> (s - 1)) & 1; return (uint32_t)((int32_t)v >> s); }
    *cf = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static FORCEINLINE uint32_t ror_rc(uint32_t v, uint32_t s, uint8_t *cf) {
    s &= 0xFF;
    if (s == 0) return v;
    s &= 31;
    if (s == 0) { *cf = v >> 31; return v; }
    *cf = (v >> (s - 1)) & 1;
    return ror32(v, s);
}

static FORCEINLINE uint32_t add_flags(CPU *c, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t r64 = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)r64;
    c->c = (uint8_t)(r64 >> 32);
    c->v = (uint8_t)((~(a ^ b) & (a ^ r)) >> 31);
    NZ(r);
    return r;
}
/* a - b - !cin, ARM carry = NOT borrow */
static FORCEINLINE uint32_t sub_flags(CPU *c, uint32_t a, uint32_t b, uint32_t cin) {
    return add_flags(c, a, ~b, cin);
}

/* Provided by the runtime. */
void dispatch(CPU *c, uint32_t addr);        /* call guest code at addr (function pointer / vtable) */
void hle_call(CPU *c, uint32_t import_index); /* call a reimplemented Symbian import */
NORETURN void guest_fault(CPU *c, uint32_t addr, const char *what);
uint32_t hle_swi(CPU *c, uint32_t num);
