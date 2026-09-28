/* ARMv4T (ARM state) interpreter. Runs guest code that has no recompiled translation: the whole
 * game in the generic build, or anything the recompiler missed in a recompiled build.
 * Semantics match the recompiler (tools/difftest.py validates both against Unicorn). */
#include "hle.h"

#define RET_SENTINEL 0xFFFFFFF0u

GuestFn hostfn_lookup(uint32_t addr);

/* runtime.c */
int stub_import_index(uint32_t addr);   /* global import index of an import stub, or -1 */
int is_trap_import(uint32_t idx);
GuestFn dispatch_lookup(uint32_t addr);
int guest_code_addr(uint32_t addr);

static FORCEINLINE int cond_ok(const CPU *c, uint32_t cond) {
    switch (cond) {
    case 0x0: return c->z;
    case 0x1: return !c->z;
    case 0x2: return c->c;
    case 0x3: return !c->c;
    case 0x4: return c->n;
    case 0x5: return !c->n;
    case 0x6: return c->v;
    case 0x7: return !c->v;
    case 0x8: return c->c && !c->z;
    case 0x9: return !c->c || c->z;
    case 0xA: return c->n == c->v;
    case 0xB: return c->n != c->v;
    case 0xC: return !c->z && c->n == c->v;
    case 0xD: return c->z || c->n != c->v;
    case 0xE: return 1;
    default: return 0;
    }
}

/* Register read as an operand: PC reads as instruction address + 8 (+12 with register shifts). */
static FORCEINLINE uint32_t reg(const CPU *c, uint32_t r, uint32_t pc, uint32_t delta) {
    return r == 15 ? pc + delta : c->r[r];
}

/* Operand 2 of a data-processing instruction; *cf receives the shifter carry-out. */
static FORCEINLINE uint32_t operand2(const CPU *c, uint32_t w, uint32_t pc, uint8_t *cf) {
    *cf = c->c;
    if (w & 0x02000000) {
        uint32_t rot = ((w >> 8) & 0xF) * 2, imm = ror32(w & 0xFF, rot);
        if (rot) *cf = imm >> 31;
        return imm;
    }
    uint32_t typ = (w >> 5) & 3;
    if (w & 0x10) {
        uint32_t v = reg(c, w & 0xF, pc, 12), s = reg(c, (w >> 8) & 0xF, pc, 12) & 0xFF;
        switch (typ) {
        case 0: return lsl_rc(v, s, cf);
        case 1: return lsr_rc(v, s, cf);
        case 2: return asr_rc(v, s, cf);
        default: return ror_rc(v, s, cf);
        }
    }
    uint32_t v = reg(c, w & 0xF, pc, 8), amt = (w >> 7) & 0x1F;
    switch (typ) {
    case 0:
        if (!amt) return v;
        *cf = (v >> (32 - amt)) & 1;
        return v << amt;
    case 1:
        if (!amt) { *cf = v >> 31; return 0; }
        *cf = (v >> (amt - 1)) & 1;
        return v >> amt;
    case 2:
        if (!amt) { *cf = v >> 31; return (uint32_t)((int32_t)v >> 31); }
        *cf = (v >> (amt - 1)) & 1;
        return (uint32_t)((int32_t)v >> amt);
    default:
        if (!amt) { uint32_t r = ((uint32_t)c->c << 31) | (v >> 1); *cf = v & 1; return r; }
        *cf = (v >> (amt - 1)) & 1;
        return ror32(v, amt);
    }
}

/* Offset of LDR/STR with a register operand (shift by immediate, no carry). */
static FORCEINLINE uint32_t ldst_offset(const CPU *c, uint32_t w, uint32_t pc) {
    if (!(w & 0x02000000)) return w & 0xFFF;
    uint8_t cf;
    return operand2(c, w & ~0x10u & ~0x02000000u, pc, &cf);
}

static NORETURN void undefined(CPU *c, uint32_t pc) { guest_fault(c, pc, "interp: undefined instruction"); }

/* Calls host code (import, host function, recompiled function) as if by BL and resumes at LR. */
static void call_host(CPU *c, GuestFn fn, int import_idx) {
    uint32_t ret = c->r[14];
    if (import_idx >= 0) hle_call(c, (uint32_t)import_idx);
    else fn(c);
    c->r[15] = ret;
}

enum { STOP = 0, RUN = 1, TRAP = 2 };

/* After a control transfer to `target`. Returns RUN to keep interpreting at c->r[15], STOP when
 * the outermost function returned, or TRAP (with *trap_idx) for a call to TTrap::Trap, which the
 * loop handles itself: its setjmp must live in the frame that stays active while the trap runs. */
static int enter(CPU *c, uint32_t target, int *trap_idx, uint32_t stop) {
    for (;;) {
        c->r[15] = target;
        if (target == stop) return STOP;
        int idx = stub_import_index(target);
        GuestFn fn = NULL;
        if (idx >= 0) {
            if (is_trap_import((uint32_t)idx)) { *trap_idx = idx; return TRAP; }
            call_host(c, NULL, idx);
        } else if ((fn = dispatch_lookup(target)) || (fn = hostfn_lookup(target))) {
            call_host(c, fn, -1);
        } else {
            if (target & 1) guest_fault(c, target, "interp: Thumb code is not supported");
            return RUN;
        }
        target = c->r[15]; /* the call returned to LR: continue there */
        if (target == stop) return STOP;
        return RUN;
    }
}

/* Transfer control; handles TTrap::Trap with setjmp in this (long-lived) frame. */
#define GOTO(target_expr)                                                            \
    do {                                                                                 \
        int trap_idx_ = -1;                                                              \
        int r_ = enter(c, (target_expr), &trap_idx_, stop);                                    \
        if (r_ == STOP) return;                                                          \
        if (r_ == TRAP) {                                                                \
            c->r[15] = c->r[14];                                                         \
            jmp_buf *jb_ = trap_enter(c);                                                \
            if (setjmp(*jb_) == 0) {                                                     \
                hle_call(c, (uint32_t)trap_idx_);                                        \
            } else {                                                                     \
                /* sp, r4-r11 and lr as at the Trap call; Trap now returns 1 */          \
                trap_resume(c);                                                          \
                c->r[15] = c->r[14];                                                     \
            }                                                                            \
        }                                                                                \
    } while (0)

static void interp_loop(CPU *c, uint32_t stop) {
    for (;;) {
        uint32_t pc = c->r[15];
        uint32_t w = rd32(pc);
        c->r[15] = pc + 4;
        if (!cond_ok(c, w >> 28)) {
            if ((w >> 28) == 0xF) undefined(c, pc);
            continue;
        }
        uint32_t op = (w >> 25) & 7;

        if (op <= 1) {
            /* ---- data processing and miscellaneous ---- */
            if ((w & 0x0FFFFFF0) == 0x012FFF10) { /* BX */
                GOTO(c->r[w & 0xF]);
                continue;
            }
            if (!(w & 0x02000000) && (w & 0x90) == 0x90) {
                if ((w & 0x0FC000F0) == 0x00000090) { /* MUL/MLA */
                    uint32_t rd = (w >> 16) & 0xF, rn = (w >> 12) & 0xF, rs = (w >> 8) & 0xF, rm = w & 0xF;
                    uint32_t res = c->r[rm] * c->r[rs] + ((w & 0x00200000) ? c->r[rn] : 0);
                    c->r[rd] = res;
                    if (w & 0x00100000) NZ(res);
                    continue;
                }
                if ((w & 0x0F8000F0) == 0x00800090) { /* UMULL/UMLAL/SMULL/SMLAL */
                    uint32_t hi = (w >> 16) & 0xF, lo = (w >> 12) & 0xF, rs = (w >> 8) & 0xF, rm = w & 0xF;
                    uint64_t res = (w & 0x00400000)
                        ? (uint64_t)((int64_t)(int32_t)c->r[rm] * (int64_t)(int32_t)c->r[rs])
                        : (uint64_t)c->r[rm] * c->r[rs];
                    if (w & 0x00200000) res += ((uint64_t)c->r[hi] << 32) | c->r[lo];
                    c->r[lo] = (uint32_t)res;
                    c->r[hi] = (uint32_t)(res >> 32);
                    if (w & 0x00100000) { c->n = (uint8_t)(res >> 63); c->z = res == 0; }
                    continue;
                }
                if ((w & 0x0FB00FF0) == 0x01000090) { /* SWP/SWPB */
                    uint32_t rn = (w >> 16) & 0xF, rd = (w >> 12) & 0xF, rm = w & 0xF, a = c->r[rn];
                    if (w & 0x00400000) { uint32_t t = rd8(a); wr8(a, c->r[rm]); c->r[rd] = t; }
                    else { uint32_t t = rd32(a); wr32(a, c->r[rm]); c->r[rd] = t; }
                    continue;
                }
                if (w & 0x60) { /* LDRH/STRH/LDRSB/LDRSH */
                    uint32_t p = (w >> 24) & 1, u = (w >> 23) & 1, wb = (w >> 21) & 1, ld = (w >> 20) & 1;
                    uint32_t rn = (w >> 16) & 0xF, rd = (w >> 12) & 0xF, sh = (w >> 5) & 3;
                    uint32_t off = (w & 0x00400000) ? (((w >> 4) & 0xF0) | (w & 0xF)) : c->r[w & 0xF];
                    uint32_t base = reg(c, rn, pc, 8), ea = u ? base + off : base - off, a = p ? ea : base;
                    if (ld) {
                        uint32_t v = sh == 1 ? rd16(a) : sh == 2 ? (uint32_t)(int32_t)(int8_t)rd8(a)
                                                               : (uint32_t)(int32_t)(int16_t)rd16(a);
                        if (!p || wb) c->r[rn] = ea;
                        c->r[rd] = v;
                    } else {
                        wr16(a, reg(c, rd, pc, 12));
                        if (!p || wb) c->r[rn] = ea;
                    }
                    continue;
                }
                undefined(c, pc);
            }
            if ((w & 0x0FBF0FFF) == 0x010F0000) { /* MRS */
                c->r[(w >> 12) & 0xF] = ((uint32_t)c->n << 31) | ((uint32_t)c->z << 30) |
                                        ((uint32_t)c->c << 29) | ((uint32_t)c->v << 28) | 0x10u;
                continue;
            }
            if ((w & 0x0DB0F000) == 0x0120F000) { /* MSR (flags only) */
                if (w & 0x00080000) {
                    uint32_t v = (w & 0x02000000) ? ror32(w & 0xFF, ((w >> 8) & 0xF) * 2) : c->r[w & 0xF];
                    c->n = v >> 31; c->z = (v >> 30) & 1; c->c = (v >> 29) & 1; c->v = (v >> 28) & 1;
                }
                continue;
            }
            uint32_t opc = (w >> 21) & 0xF, s = (w >> 20) & 1, rn = (w >> 16) & 0xF, rd = (w >> 12) & 0xF;
            uint8_t sc;
            uint32_t b = operand2(c, w, pc, &sc);
            uint32_t a = reg(c, rn, pc, (!(w & 0x02000000) && (w & 0x10)) ? 12 : 8), res;
            int logical = 0, write = 1;
            switch (opc) {
            case 0x0: res = a & b; logical = 1; break;
            case 0x1: res = a ^ b; logical = 1; break;
            case 0x2: res = s ? sub_flags(c, a, b, 1) : a - b; break;
            case 0x3: res = s ? sub_flags(c, b, a, 1) : b - a; break;
            case 0x4: res = s ? add_flags(c, a, b, 0) : a + b; break;
            case 0x5: res = s ? add_flags(c, a, b, c->c) : a + b + c->c; break;
            case 0x6: res = s ? sub_flags(c, a, b, c->c) : a - b - !c->c; break;
            case 0x7: res = s ? sub_flags(c, b, a, c->c) : b - a - !c->c; break;
            case 0x8: res = a & b; logical = 1; write = 0; break;
            case 0x9: res = a ^ b; logical = 1; write = 0; break;
            case 0xA: res = sub_flags(c, a, b, 1); write = 0; break;
            case 0xB: res = add_flags(c, a, b, 0); write = 0; break;
            case 0xC: res = a | b; logical = 1; break;
            case 0xD: res = b; logical = 1; break;
            case 0xE: res = a & ~b; logical = 1; break;
            default: res = ~b; logical = 1; break;
            }
            if (logical && s) { NZ(res); c->c = sc; }
            if (!write) continue;
            if (rd == 15) {
                GOTO(res);
                continue;
            }
            c->r[rd] = res;
            continue;
        }

        if (op <= 3) {
            /* ---- LDR/STR/LDRB/STRB ---- */
            if (op == 3 && (w & 0x10)) undefined(c, pc);
            uint32_t p = (w >> 24) & 1, u = (w >> 23) & 1, bb = (w >> 22) & 1, wb = (w >> 21) & 1, ld = (w >> 20) & 1;
            uint32_t rn = (w >> 16) & 0xF, rd = (w >> 12) & 0xF;
            uint32_t off = ldst_offset(c, w, pc);
            uint32_t base = reg(c, rn, pc, 8), ea = u ? base + off : base - off, a = p ? ea : base;
            if (ld) {
                uint32_t v = bb ? rd8(a) : rd32(a);
                if ((!p || wb) && rn != 15) c->r[rn] = ea;
                if (rd == 15) {
                    GOTO(v);
                    continue;
                }
                c->r[rd] = v;
            } else {
                uint32_t v = reg(c, rd, pc, 12);
                if (bb) wr8(a, v); else wr32(a, v);
                if ((!p || wb) && rn != 15) c->r[rn] = ea;
            }
            continue;
        }

        if (op == 4) {
            /* ---- LDM/STM ---- */
            uint32_t p = (w >> 24) & 1, u = (w >> 23) & 1, wb = (w >> 21) & 1, ld = (w >> 20) & 1;
            uint32_t rn = (w >> 16) & 0xF, list = w & 0xFFFF, n = 0;
            for (uint32_t i = 0; i < 16; i++) n += (list >> i) & 1;
            uint32_t base = c->r[rn], start;
            if (u) start = p ? base + 4 : base;
            else start = p ? base - 4 * n : base - 4 * n + 4;
            uint32_t nb = u ? base + 4 * n : base - 4 * n;
            if (ld) {
                uint32_t vals[16], a = start & ~3u;
                for (uint32_t i = 0; i < 16; i++) if (list & (1u << i)) { vals[i] = rd32(a); a += 4; }
                if (wb && !(list & (1u << rn))) c->r[rn] = nb;
                for (uint32_t i = 0; i < 15; i++) if (list & (1u << i)) c->r[i] = vals[i];
                if (list & 0x8000) GOTO(vals[15]);
            } else {
                uint32_t a = start & ~3u;
                for (uint32_t i = 0; i < 16; i++)
                    if (list & (1u << i)) { wr32(a, i == 15 ? pc + 12 : c->r[i]); a += 4; }
                if (wb) c->r[rn] = nb;
            }
            continue;
        }

        if (op == 5) {
            /* ---- B/BL ---- */
            int32_t imm = (int32_t)(w << 8) >> 6;
            uint32_t target = pc + 8 + (uint32_t)imm;
            if (w & 0x01000000) c->r[14] = pc + 4;
            GOTO(target);
            continue;
        }

        if (op == 7 && (w & 0x0F000000) == 0x0F000000) {
            hle_swi(c, w & 0xFFFFFF);
            continue;
        }
        guest_fault(c, pc, "interp: coprocessor instruction");
    }
}

/* Runs guest code at addr until it returns (called like any GuestFn by dispatch). */
/* Runs guest code at addr until it returns (called like any GuestFn by dispatch).
 * Returning is detected by reaching the caller's return address; when that address is itself
 * guest code (possibly a stale LR from host code) a sentinel is used instead, so a recursive call
 * through the same call site cannot be mistaken for the return. */
void interp_call(CPU *c, uint32_t addr) {
    uint32_t entry_lr = c->r[14];
    uint32_t stop = (entry_lr == 0 || guest_code_addr(entry_lr)) ? RET_SENTINEL : entry_lr;
    c->r[14] = stop;
    c->r[15] = addr;
    /* The first "instruction" is a jump to addr (it may itself be an import stub or host code). */
    int trap_idx = -1;
    int r = enter(c, addr, &trap_idx, stop);
    if (r == TRAP) guest_fault(c, addr, "interp: TTrap::Trap called directly as a function");
    if (r == RUN) interp_loop(c, stop);
    if (c->r[14] == RET_SENTINEL) c->r[14] = entry_lr;
}
