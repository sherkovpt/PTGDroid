"""Static recompiler: EKA1 ARMv4T (ARM mode) E32 image -> C source.

Every guest function becomes `void f_XXXXXXXX(CPU *c)`. Intra-function branches are gotos,
BL is a direct C call, returns are `return`, indirect calls go through dispatch().
Guest code is loaded at its link base, so PC-relative literals are folded to constants.
"""
import json
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import e32  # noqa: E402
from analyze import Analysis  # noqa: E402
from resolve_imports import load_defs, resolve  # noqa: E402

CONDS = ["C_EQ", "C_NE", "C_CS", "C_CC", "C_MI", "C_PL", "C_VS", "C_VC",
         "C_HI", "C_LS", "C_GE", "C_LT", "C_GT", "C_LE", None, "NV"]
AL = 0xE
MOV_LR_PC = 0x01A0E00F  # mov lr, pc (cond masked off)
NORETURN_PREFIXES = ("Leave__4User", "LeaveNoMemory__4User", "Panic__4User", "Invariant__4User",
                     "Exit__4User", "LeaveIfError__4Useri" + "_never")
TRAP_NAME = "Trap__5TTrapRi"
STUB = (0xE59FC004, 0xE59CC000, 0xE12FFF1C)  # ldr ip,[pc,#4]; ldr ip,[ip]; bx ip; .word iat
FUNCS_PER_FILE = 48


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Unsupported(Exception):
    pass


class Recompiler:
    def __init__(self, path, base=None, module="app", import_base=0):
        self.a = Analysis(path, base)
        self.module = module
        self.import_base = import_base
        self.base = self.a.base
        self.text_size = self.a.text_size
        self.imports = resolve(self.a.img, load_defs())
        iat0 = self.base + self.text_size
        self.stubs = {}
        w = self.a.words
        for i in range(0, self.text_size // 4 - 3):
            if w[i] == STUB[0] and w[i + 1] == STUB[1] and w[i + 2] == STUB[2]:
                self.stubs[i * 4] = (w[i + 3] - iat0) // 4
        arm, thumb, _ = self.a.discover()
        self.entries = set(arm) | set(self.stubs)
        self.thumb = set(thumb)
        self.noreturn_stubs = set()
        weak = self.entries - self.a.strong_entries - set(self.stubs)
        rejected = {e for e in weak if not self.plausible(e)}
        self.entries -= rejected
        self.rejected = rejected
        self.noreturn_stubs = {off for off, idx in self.stubs.items()
                               if (self.imports[idx]["name"] or "").startswith(NORETURN_PREFIXES)}
        self.trap_stubs = {off for off, idx in self.stubs.items() if self.imports[idx]["name"] == TRAP_NAME}
        self.stats = Counter()

    # ---------- helpers ----------
    def W(self, off):
        return self.a.words[off // 4]

    def valid(self, off):
        return 0 <= off < self.text_size and off % 4 == 0 and off not in self.a.reloc_sites

    def addr(self, off):
        return self.base + off

    def prev_is_mov_lr_pc(self, off):
        return off >= 4 and (self.W(off - 4) & 0x0FFFFFFF) == MOV_LR_PC

    def prev_pops_into(self, off, reg):
        """GCC interworking epilogue: `ldmfd sp!, {..., rX}` then `bx rX` (same condition)."""
        if off < 4:
            return False
        p, w = self.W(off - 4), self.W(off)
        return ((p & 0x0FFF0000) == 0x08BD0000 and (p >> reg) & 1
                and (p >> 28) in (w >> 28, AL))

    def find_cmp_bound(self, off, reg):
        """Jump tables are guarded by `cmp reg, #N`; returns entry count N+1."""
        for back in range(1, 6):
            p = off - 4 * back
            if p < 0:
                break
            w = self.W(p)
            if (w & 0x0FF0F000) == 0x03500000 and ((w >> 16) & 0xF) == reg:
                rot = ((w >> 8) & 0xF) * 2
                return e32_ror(w & 0xFF, rot) + 1
        return None

    # ---------- classification ----------
    def classify(self, off):
        """Returns (kind, info). Kinds drive both traversal and emission."""
        w = self.W(off)
        cond = w >> 28
        if cond == 0xF:
            return "undef", None
        if (w & 0x0FFFFFF0) == 0x012FFF10:
            rm = w & 0xF
            if rm == 14 or self.prev_pops_into(off, rm):
                return "ret", None
            if self.prev_is_mov_lr_pc(off):
                return "icall", None
            return "ijump", None
        if (w & 0x0E000000) == 0x0A000000:
            t = off + 8 + sx(w & 0xFFFFFF, 24) * 4
            if w & 0x01000000:
                return "bl", t
            return "b", t
        if (w & 0x0FFFFFF0) == 0x008FF100:  # add pc, pc, rm, lsl #2
            n = self.find_cmp_bound(off, w & 0xF)
            if n is None:
                raise Unsupported("jump table without bound")
            return "jt_add", [off + 8 + 4 * i for i in range(n)]
        if (w & 0x0FFFFFF0) == 0x079FF100:  # ldr pc, [pc, rm, lsl #2]
            n = self.find_cmp_bound(off, w & 0xF)
            if n is None:
                raise Unsupported("jump table without bound")
            return "jt_ldr", [self.W(off + 8 + 4 * i) - self.base for i in range(n)]
        writes_pc = False
        if (w & 0x0C000000) == 0x00000000 and not self.is_misc(w):
            op = (w >> 21) & 0xF
            if op not in (8, 9, 10, 11) and ((w >> 12) & 0xF) == 15:
                if (w & 0x0FFFFFFF) in (0x01A0F00E, 0x01B0F00E):
                    return "ret", None
                writes_pc = True
        elif (w & 0x0C000000) == 0x04000000 and (w & 0x00100000) and ((w >> 12) & 0xF) == 15:
            # ldr pc, [sp], #4  == pop {pc}
            if (w & 0x0FFFFFFF) == 0x049DF004:
                return "ret", None
            writes_pc = True
        elif (w & 0x0E000000) == 0x08000000 and (w & 0x00100000) and (w & 0x8000):
            rn = (w >> 16) & 0xF
            if rn == 13 and (w & 0x00200000) and not self.prev_is_mov_lr_pc(off):
                return "ret_ldm", None
            writes_pc = True
        if writes_pc:
            return ("icall", None) if self.prev_is_mov_lr_pc(off) else ("ijump", None)
        return "plain", None

    @staticmethod
    def is_misc(w):
        return ((w & 0x0E000090) == 0x00000090 and (w & 0x60) != 0) or (w & 0x0FC000F0) == 0x00000090 \
            or (w & 0x0F8000F0) == 0x00800090 or (w & 0x0FB00FF0) == 0x01000090 \
            or (w & 0x0FBF0FFF) == 0x010F0000 or (w & 0x0DB0F000) == 0x0120F000

    def plausible(self, entry):
        """Pointer-derived entries may be read-only data; real compiled code never contains
        coprocessor/undefined encodings or branches leaving .text."""
        try:
            insns, _ = self.traverse(entry)
        except Unsupported:
            return False
        if entry not in insns:
            return False
        for off in insns:
            w = self.W(off)
            try:
                kind, info = self.classify(off)
            except Unsupported:
                return False
            top = (w >> 25) & 7
            if kind == "undef" or top == 6 or (top == 7 and (w & 0x0F000000) != 0x0F000000):
                return False
            if top == 7:  # SWI never appears in application code
                return False
            if top == 3 and (w & 0x10):
                return False
            if kind in ("b", "bl") and not (0 <= info < self.text_size):
                return False
        return True

    # ---------- traversal ----------
    def traverse(self, entry):
        insns, targets, work = set(), set(), [entry]
        while work:
            off = work.pop()
            while True:
                if off in insns or not self.valid(off):
                    break
                insns.add(off)
                kind, info = self.classify(off)
                cond = self.W(off) >> 28
                falls = cond != AL
                if kind == "b":
                    if info in self.entries and info != entry:
                        pass  # tail call
                    else:
                        targets.add(info)
                        work.append(info)
                elif kind == "bl":
                    falls = not (cond == AL and info in self.noreturn_stubs)
                elif kind in ("jt_add", "jt_ldr"):
                    for t in info:
                        targets.add(t)
                        work.append(t)
                elif kind in ("ret", "ret_ldm", "ijump", "undef"):
                    pass
                else:
                    falls = True
                if not falls:
                    break
                off += 4
        return sorted(insns), targets

    # ---------- emission ----------
    def reg(self, r, off, pc_delta=8):
        return f"0x{self.addr(off) + pc_delta:08X}u" if r == 15 else f"R({r})"

    def shift_imm(self, w, off, want_carry):
        """Operand2 register form shifted by immediate. Returns (expr, carry_stmt)."""
        rm, typ, amt = w & 0xF, (w >> 5) & 3, (w >> 7) & 0x1F
        v = self.reg(rm, off)
        if typ == 0:
            if amt == 0:
                return v, None
            return f"({v} << {amt})", f"sc = ({v} >> {32 - amt}) & 1;"
        if typ == 1:
            if amt == 0:
                return "0u", f"sc = {v} >> 31;"
            return f"({v} >> {amt})", f"sc = ({v} >> {amt - 1}) & 1;"
        if typ == 2:
            a = 31 if amt == 0 else amt
            carry = f"sc = {v} >> 31;" if amt == 0 else f"sc = ({v} >> {amt - 1}) & 1;"
            return f"(uint32_t)((int32_t){v} >> {a})", carry
        if amt == 0:
            return f"(((uint32_t)c->c << 31) | ({v} >> 1))", f"sc = {v} & 1;"
        return f"ror32({v}, {amt})", f"sc = ({v} >> {amt - 1}) & 1;"

    def operand2(self, w, off, want_carry):
        if w & 0x02000000:
            rot = ((w >> 8) & 0xF) * 2
            imm = e32_ror(w & 0xFF, rot)
            carry = f"sc = {imm >> 31};" if rot else None
            return f"0x{imm:X}u", carry
        if w & 0x10:
            rm, typ, rs = w & 0xF, (w >> 5) & 3, (w >> 8) & 0xF
            v, s = self.reg(rm, off, 12), self.reg(rs, off, 12)
            fn = ["lsl", "lsr", "asr", "ror"][typ]
            if want_carry:
                return f"{fn}_rc({v}, {s}, &sc)", None
            return f"{fn}_r({v}, {s})", None
        return self.shift_imm(w, off, want_carry)

    def emit_dp(self, w, off, pc_value_sink=None):
        op, s = (w >> 21) & 0xF, (w >> 20) & 1
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        logical = op in (0, 1, 8, 9, 12, 13, 14, 15)
        want_carry = bool(s and logical)
        b, carry = self.operand2(w, off, want_carry)
        a = self.reg(rn, off, 12 if (not (w & 0x02000000) and (w & 0x10)) else 8)
        pre = []
        if want_carry:
            pre.append("uint8_t sc = c->c;")
            if carry:
                pre.append(carry)
        pre.append(f"uint32_t op2 = {b};")
        ops = {
            0: "a & op2", 1: "a ^ op2", 12: "a | op2", 14: "a & ~op2", 13: "op2", 15: "~op2",
            8: "a & op2", 9: "a ^ op2",
        }
        if logical:
            res = ops[op]
            body = [f"uint32_t res = {res};"]
            if s:
                body += ["NZ(res);", "c->c = sc;"]
        else:
            if s or op in (10, 11):
                expr = {
                    4: "add_flags(c, a, op2, 0)", 5: "add_flags(c, a, op2, c->c)",
                    2: "sub_flags(c, a, op2, 1)", 6: "sub_flags(c, a, op2, c->c)",
                    3: "sub_flags(c, op2, a, 1)", 7: "sub_flags(c, op2, a, c->c)",
                    10: "sub_flags(c, a, op2, 1)", 11: "add_flags(c, a, op2, 0)",
                }[op]
            else:
                expr = {
                    4: "a + op2", 5: "a + op2 + c->c", 2: "a - op2", 6: "a - op2 - !c->c",
                    3: "op2 - a", 7: "op2 - a - !c->c",
                }[op]
            body = [f"uint32_t res = {expr};"]
        needs_a = op not in (13, 15)
        head = [f"uint32_t a = {a};"] if needs_a else []
        if op in (8, 9, 10, 11):
            tail = ["(void)res;"]
        elif rd == 15:
            tail = [pc_value_sink("res")]
        else:
            tail = [f"R({rd}) = res;"]
        return "{ " + " ".join(head + pre + body + tail) + " }"

    def mem_offset(self, w, off):
        if not (w & 0x02000000):
            return f"0x{w & 0xFFF:X}u"
        expr, _ = self.shift_imm(w, off, False)
        return expr

    def emit_ldst(self, w, off, pc_value_sink=None):
        p, u, bb, wb, ld = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        o = self.mem_offset(w, off)
        sign = "+" if u else "-"
        # PC-relative literal load with immediate offset: fold the constant.
        if rn == 15 and not (w & 0x02000000) and ld and rd != 15:
            ea = self.addr(off) + 8 + ((w & 0xFFF) if u else -(w & 0xFFF))
            eo = ea - self.base
            if 0 <= eo < len(self.a.img.code) and not bb and eo % 4 == 0:
                return f"R({rd}) = 0x{self.W(eo):08X}u;"
        base = self.reg(rn, off)
        parts = [f"uint32_t base = {base};", f"uint32_t ea = base {sign} {o};"]
        addr = "ea" if p else "base"
        writeback = (not p) or wb
        if ld:
            parts.append(f"uint32_t v = {'rd8' if bb else 'rd32'}({addr});")
            if writeback and rn != 15:
                parts.append(f"R({rn}) = ea;")
            parts.append(pc_value_sink("v") if rd == 15 else f"R({rd}) = v;")
        else:
            val = self.reg(rd, off, 12)
            parts.append(f"{'wr8' if bb else 'wr32'}({addr}, {val});")
            if writeback and rn != 15:
                parts.append(f"R({rn}) = ea;")
        return "{ " + " ".join(parts) + " }"

    def emit_half(self, w, off):
        p, u, imm, wb, ld = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn, rd, sh = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 5) & 3
        o = f"0x{((w >> 4) & 0xF0) | (w & 0xF):X}u" if imm else self.reg(w & 0xF, off)
        sign = "+" if u else "-"
        parts = [f"uint32_t base = {self.reg(rn, off)};", f"uint32_t ea = base {sign} {o};"]
        addr = "ea" if p else "base"
        writeback = (not p) or wb
        if rd == 15:
            raise Unsupported("halfword pc")
        if ld:
            load = {1: f"rd16({addr})", 2: f"(uint32_t)(int32_t)(int8_t)rd8({addr})",
                    3: f"(uint32_t)(int32_t)(int16_t)rd16({addr})"}[sh]
            parts.append(f"uint32_t v = {load};")
            if writeback:
                parts.append(f"R({rn}) = ea;")
            parts.append(f"R({rd}) = v;")
        else:
            if sh != 1:
                raise Unsupported("ldrd/strd")
            parts.append(f"wr16({addr}, {self.reg(rd, off, 12)});")
            if writeback:
                parts.append(f"R({rn}) = ea;")
        return "{ " + " ".join(parts) + " }"

    def emit_ldm(self, w, off, pc_value_sink=None):
        p, u, wb, ld = (w >> 24) & 1, (w >> 23) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn = (w >> 16) & 0xF
        regs = [i for i in range(16) if w & (1 << i)]
        n = len(regs)
        start = {(0, 1): "+ 0", (1, 1): "+ 4", (0, 0): f"- {4 * n - 4}", (1, 0): f"- {4 * n}"}[(p, u)]
        new = f"+ {4 * n}" if u else f"- {4 * n}"
        parts = [f"uint32_t base = R({rn});", f"uint32_t ea = base {start};"]
        if ld:
            for i, r in enumerate(regs):
                parts.append(f"uint32_t v{r} = rd32(ea + {4 * i});")
            if wb and rn not in regs:
                parts.append(f"R({rn}) = base {new};")
            for r in regs:
                parts.append(pc_value_sink(f"v{r}") if r == 15 else f"R({r}) = v{r};")
        else:
            for i, r in enumerate(regs):
                parts.append(f"wr32(ea + {4 * i}, {self.reg(r, off, 12) if r == 15 else f'R({r})'});")
            if wb:
                parts.append(f"R({rn}) = base {new};")
        return "{ " + " ".join(parts) + " }"

    def emit_mul(self, w):
        rd, rn, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
        acc = f" + R({rn})" if w & 0x00200000 else ""
        s = "NZ(res);" if w & 0x00100000 else ""
        return f"{{ uint32_t res = R({rm}) * R({rs}){acc}; R({rd}) = res; {s} }}"

    def emit_mull(self, w):
        hi, lo, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
        signed, acc, s = w & 0x00400000, w & 0x00200000, w & 0x00100000
        if signed:
            prod = f"(uint64_t)((int64_t)(int32_t)R({rm}) * (int64_t)(int32_t)R({rs}))"
        else:
            prod = f"((uint64_t)R({rm}) * (uint64_t)R({rs}))"
        a = f" + (((uint64_t)R({hi}) << 32) | R({lo}))" if acc else ""
        st = "c->n = (uint8_t)(res >> 63); c->z = res == 0;" if s else ""
        return f"{{ uint64_t res = {prod}{a}; R({lo}) = (uint32_t)res; R({hi}) = (uint32_t)(res >> 32); {st} }}"

    def emit_insn(self, off, entry, kind, info):
        w = self.W(off)
        a = self.addr(off)
        call_sink = lambda v: f"R(14) = 0x{a + 4:08X}u; dispatch(c, {v});"  # noqa: E731
        jump_sink = lambda v: f"dispatch(c, {v}); return;"  # noqa: E731
        ret_sink = lambda v: "return;"  # noqa: E731

        if kind == "undef":
            return f'guest_fault(c, 0x{a:08X}u, "undefined");'
        if kind == "ret":
            return "return;"
        if kind == "b":
            if info in self.entries and info != entry:
                self.stats["tailcall"] += 1
                return f"{self.call_target(info, a, tail=True)} return;"
            return self.goto(info)
        if kind == "bl":
            return self.call_target(info, a)
        if kind in ("jt_add", "jt_ldr"):
            rm = w & 0xF
            cases = " ".join(f"case {i}: {self.goto(t)}" for i, t in enumerate(info))
            return f'switch (R({rm})) {{ {cases} default: guest_fault(c, 0x{a:08X}u, "jump table"); }}'

        sink = {"icall": call_sink, "ijump": jump_sink, "ret_ldm": ret_sink, "plain": None}[kind]
        if kind == "icall":
            self.stats["icall"] += 1
        if (w & 0x0FFFFFF0) == 0x012FFF10:  # bx rm (non-lr)
            return sink(f"R({w & 0xF})")
        if (w & 0x0FC000F0) == 0x00000090:
            return self.emit_mul(w)
        if (w & 0x0F8000F0) == 0x00800090:
            return self.emit_mull(w)
        if (w & 0x0FB00FF0) == 0x01000090:
            rn, rd, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, w & 0xF
            if w & 0x00400000:
                return f"{{ uint32_t t = rd8(R({rn})); wr8(R({rn}), R({rm})); R({rd}) = t; }}"
            return f"{{ uint32_t t = rd32(R({rn})); wr32(R({rn}), R({rm})); R({rd}) = t; }}"
        if (w & 0x0E000090) == 0x00000090 and (w & 0x60):
            return self.emit_half(w, off)
        if (w & 0x0FBF0FFF) == 0x010F0000:
            rd = (w >> 12) & 0xF
            return (f"R({rd}) = ((uint32_t)c->n << 31) | ((uint32_t)c->z << 30) | "
                    f"((uint32_t)c->c << 29) | ((uint32_t)c->v << 28) | 0x10u;")
        if (w & 0x0DB0F000) == 0x0120F000:
            if not (w & 0x00080000):
                return "/* msr (control/status fields ignored) */"
            if w & 0x02000000:
                val = f"0x{e32_ror(w & 0xFF, ((w >> 8) & 0xF) * 2):X}u"
            else:
                val = f"R({w & 0xF})"
            return (f"{{ uint32_t v = {val}; c->n = v >> 31; c->z = (v >> 30) & 1; "
                    f"c->c = (v >> 29) & 1; c->v = (v >> 28) & 1; }}")
        top = (w >> 25) & 7
        if top in (0, 1):
            return self.emit_dp(w, off, sink)
        if top in (2, 3):
            if top == 3 and (w & 0x10):
                return f'guest_fault(c, 0x{a:08X}u, "undefined");'
            return self.emit_ldst(w, off, sink)
        if top == 4:
            return self.emit_ldm(w, off, sink)
        if top == 7 and (w & 0x0F000000) == 0x0F000000:
            return f"hle_swi(c, 0x{w & 0xFFFFFF:X}u);"
        self.stats["coproc"] += 1
        return f'guest_fault(c, 0x{a:08X}u, "coprocessor");'

    def goto(self, t):
        if t in self._body:
            return f"goto L_{self.addr(t):08X};"
        self.stats["branch into data"] += 1
        return f'guest_fault(c, 0x{self.addr(t) & 0xFFFFFFFF:08X}u, "branch into data");'

    def call_target(self, t, a, tail=False):
        lr = "" if tail else f"R(14) = 0x{a + 4:08X}u; "
        if t in self.trap_stubs and not tail:
            self.stats["trap"] += 1
            return (f"{lr}{{ jmp_buf *jb = trap_enter(c); if (setjmp(*jb) == 0) "
                    f"{{ hle_call(c, {self.import_base + self.stubs[t]}); }} else {{ trap_resume(c); }} }}")
        if t in self.stubs:
            return f"{lr}hle_call(c, {self.import_base + self.stubs[t]});"
        if t in self.thumb:
            return f"{lr}dispatch(c, 0x{self.addr(t) | 1:08X}u);"
        if t not in self.entries:
            self.stats["call to non-function"] += 1
            return f'guest_fault(c, 0x{self.addr(t) & 0xFFFFFFFF:08X}u, "call to non-function");'
        return f"{lr}f_{self.addr(t):08X}(c);"

    def gen_function(self, entry):
        name = f"f_{self.addr(entry):08X}"
        if entry in self.stubs:
            return f"void {name}(CPU *c) {{ hle_call(c, {self.stubs[entry]}); }}\n"
        insns, targets = self.traverse(entry)
        # r15 is otherwise unused (PC reads are constants): record the function for crash reports.
        out = [f"void {name}(CPU *c) {{", f"  R(15) = 0x{self.addr(entry):08X}u;"]
        body = set(insns)
        self._body = body
        if insns and insns[0] != entry and entry in body:
            # Code reached from the entry can lie at lower addresses (e.g. this-adjusting thunks).
            targets.add(entry)
            out.append(f"  goto L_{self.addr(entry):08X};")
        for off in insns:
            w = self.W(off)
            a = self.addr(off)
            if off in targets:
                out.append(f"L_{a:08X}:;")
            try:
                kind, info = self.classify(off)
                code = self.emit_insn(off, entry, kind, info)
            except Unsupported as ex:
                self.stats[f"unsupported: {ex}"] += 1
                code = f'guest_fault(c, 0x{a:08X}u, "unsupported: {ex}");'
                kind = "undef"
            cond = w >> 28
            if cond != AL and kind != "undef":
                code = f"if ({CONDS[cond]}) {{ {code} }}"
            out.append(f"  /* {a:08X} */ {code}")
            ends = cond == AL and (kind in ("b", "ret", "ret_ldm", "ijump", "undef", "jt_add", "jt_ldr")
                                   or (kind == "bl" and info in self.noreturn_stubs))
            if not ends and off + 4 not in body:
                out.append(f'  guest_fault(c, 0x{a + 4:08X}u, "fell off function");')
        if entry not in body:
            out.append(f'  guest_fault(c, 0x{self.addr(entry):08X}u, "entry is data");')
        out.append("}\n")
        self.stats["insns"] += len(insns)
        return "\n".join(out)

    def write(self, outdir):
        outdir = Path(outdir)
        outdir.mkdir(parents=True, exist_ok=True)
        entries = sorted(self.entries)
        names = [f"f_{self.addr(e):08X}" for e in entries]
        (outdir / "funcs.h").write_text(
            '#pragma once\n#include "cpu.h"\n#include <setjmp.h>\n'
            "jmp_buf *trap_enter(CPU *c);\nvoid trap_resume(CPU *c);\n"
            + "".join(f"void {n}(CPU *c);\n" for n in names))
        files = []
        for i in range(0, len(entries), FUNCS_PER_FILE):
            fn = outdir / f"recomp_{self.module}_{i // FUNCS_PER_FILE:03d}.c"
            src = ['#include "funcs.h"\n#ifdef _MSC_VER\n#pragma warning(disable: 4102 4146 4702)\n#endif\n']
            for e in entries[i:i + FUNCS_PER_FILE]:
                src.append(self.gen_function(e))
            fn.write_text("\n".join(src))
            files.append(fn.name)
        tbl = ['#include "funcs.h"', "typedef struct { uint32_t addr; GuestFn fn; } DispatchEntry;",
               f"const unsigned DISPATCH_{self.module}_COUNT = {len(entries)};",
               f"const DispatchEntry DISPATCH_{self.module}[] = {{"]
        tbl += [f"  {{0x{self.addr(e):08X}u, {n}}}," for e, n in zip(entries, names)]
        tbl.append("};")
        (outdir / "dispatch_table.c").write_text("\n".join(tbl) + "\n")
        imp = ['#include <stdint.h>',
               "typedef struct { const char *lib; uint32_t ordinal; const char *name; } ImportInfo;",
               f"const unsigned IMPORTS_{self.module}_COUNT = {len(self.imports)};",
               f"const ImportInfo IMPORTS_{self.module}[] = {{"]
        for r in self.imports:
            nm = json.dumps(r["name"]) if r["name"] else "0"
            imp.append(f'  {{"{r["lib"]}", {r["ordinal"]}, {nm}}},')
        imp.append("};")
        (outdir / "imports_table.c").write_text("\n".join(imp) + "\n")
        return files


def e32_ror(v, r):
    r &= 31
    return ((v >> r) | (v << (32 - r))) & 0xFFFFFFFF if r else v


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("outdir")
    ap.add_argument("--base", type=lambda v: int(v, 0))
    ap.add_argument("--module", default="app")
    ap.add_argument("--import-base", type=int, default=0)
    args = ap.parse_args()
    rc = Recompiler(args.image, args.base, args.module, args.import_base)
    files = rc.write(args.outdir)
    print(f"functions: {len(rc.entries)} (stubs {len(rc.stubs)}, trap stubs {len(rc.trap_stubs)}, "
          f"noreturn stubs {len(rc.noreturn_stubs)})  files: {len(files)}")
    for k, v in rc.stats.most_common():
        print(f"  {k}: {v}")


if __name__ == "__main__":
    main()
