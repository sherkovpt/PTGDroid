"""Differential test: run guest functions in Unicorn and write expected results for ptg_difftest.

State is generated from a seed with the same xorshift32 generator as runtime/main_difftest.c,
so only seeds and the observed effects (registers, flags, changed bytes) are exchanged.
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_PROT_ALL, UC_PROT_READ, UC_PROT_EXEC, \
    UC_PROT_WRITE, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_MEM_FETCH_UNMAPPED, UC_HOOK_MEM_READ, \
    UC_HOOK_MEM_WRITE
from unicorn.arm_const import *

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "recomp"))
from armrecomp import Recompiler  # noqa: E402

CODE_BASE = 0x10000000
DATA_BASE, DATA_SIZE = 0x20000000, 0x2000
STACK_BASE, STACK_SIZE = 0x30000000, 0x1000
SENTINEL = 0x0FFFF000
MAX_INSNS = 200000
REGS = [UC_ARM_REG_R0 + i for i in range(13)] + [UC_ARM_REG_SP, UC_ARM_REG_LR]


class Gen:
    def __init__(self, seed):
        self.s = seed or 1

    def next(self):
        s = self.s
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self.s = s
        return s

    def val(self):
        sel = self.next() & 3
        if sel == 0:
            return DATA_BASE + ((self.next() % DATA_SIZE) & ~3)
        if sel == 1:
            return self.next() & 0xFF
        if sel == 2:
            return self.next()
        return 0


PAGE = 0x1000
MAX_DEMAND_PAGES = 256


def demand_page(seed, page):
    """Content of an on-demand page; must match demand_fill() in runtime/main_difftest.c."""
    g = Gen((seed ^ page ^ 0x5BD1E995) & 0xFFFFFFFF)
    return struct.pack("<%dI" % (PAGE // 4), *[g.val() for _ in range(PAGE // 4)])


def demand_allowed(page):
    return page >= PAGE and not (CODE_BASE <= page < CODE_BASE + 0x200000) and page != SENTINEL


def fake_import_result(seed, idx, n):
    """Imports are stubbed in both runners: return a deterministic value, touch nothing else."""
    return Gen((seed ^ (idx << 16) ^ (n * 0x9E3779B1)) & 0xFFFFFFFF).val()


def initial_state(seed):
    g = Gen(seed)
    regs = [g.val() for _ in range(13)] + [STACK_BASE + STACK_SIZE - 512, SENTINEL]
    flags = g.next() & 0xF0000000
    data = struct.pack("<%dI" % (DATA_SIZE // 4), *[g.val() for _ in range(DATA_SIZE // 4)])
    stack = struct.pack("<%dI" % (STACK_SIZE // 4), *[g.val() for _ in range(STACK_SIZE // 4)])
    return regs, flags, data, stack


class Runner:
    def __init__(self, rc):
        self.rc = rc
        code = rc.a.img.code
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        size = (len(code) + 0xFFF) & ~0xFFF
        uc.mem_map(CODE_BASE, size, UC_PROT_READ | UC_PROT_EXEC)
        uc.mem_write(CODE_BASE, code)
        uc.mem_map(DATA_BASE, DATA_SIZE, UC_PROT_READ | UC_PROT_WRITE)
        uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_READ | UC_PROT_WRITE)
        uc.mem_map(SENTINEL, 0x1000, UC_PROT_ALL)
        stubs = sorted(rc.stubs)
        self.hit_import = False
        self.seed = 0
        self.import_calls = 0

        def on_stub(uc_, addr, size_, _):
            off = addr - CODE_BASE
            if off not in rc.stubs:
                return
            if off in rc.noreturn_stubs:
                self.hit_import = True
                uc_.emu_stop()
                return
            uc_.reg_write(UC_ARM_REG_R0, fake_import_result(self.seed, rc.stubs[off], self.import_calls))
            self.import_calls += 1
            uc_.reg_write(UC_ARM_REG_PC, uc_.reg_read(UC_ARM_REG_LR))
        uc.hook_add(UC_HOOK_CODE, on_stub, None, CODE_BASE + stubs[0], CODE_BASE + stubs[-1] + 16)
        self.pages = {}

        def on_unmapped(uc_, access, addr, size_, value, _):
            if access == UC_MEM_FETCH_UNMAPPED or len(self.pages) >= MAX_DEMAND_PAGES:
                return False
            page = addr & ~(PAGE - 1)
            if not demand_allowed(page) or page in self.pages:
                return False
            content = demand_page(self.seed, page)
            uc_.mem_map(page, PAGE, UC_PROT_READ | UC_PROT_WRITE)
            uc_.mem_write(page, content)
            self.pages[page] = content
            return True
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

        # QEMU implements ARMv6+ unaligned accesses; the ARM920T rotates LDR and aligns STR.
        # Unicorn can't be the reference there, so such cases are discarded.
        def on_access(uc_, access, addr, size_, value, _):
            if size_ > 1 and addr & (size_ - 1):
                self.unaligned = True
                uc_.emu_stop()
        uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, on_access)
        self.unaligned = False

    def run(self, func, seed):
        uc = self.uc
        regs, flags, data, stack = initial_state(seed)
        uc.mem_write(DATA_BASE, data)
        uc.mem_write(STACK_BASE, stack)
        for r, v in zip(REGS, regs):
            uc.reg_write(r, v)
        uc.reg_write(UC_ARM_REG_CPSR, flags | 0x10)
        self.hit_import = False
        self.unaligned = False
        self.seed = seed
        self.import_calls = 0
        for page in self.pages:
            uc.mem_unmap(page, PAGE)
        self.pages = {}
        try:
            uc.emu_start(CODE_BASE + func, SENTINEL, count=MAX_INSNS)
        except UcError:
            return None
        if self.hit_import or self.unaligned or uc.reg_read(UC_ARM_REG_PC) != SENTINEL:
            return None
        out_regs = [uc.reg_read(r) for r in REGS]
        out_flags = uc.reg_read(UC_ARM_REG_CPSR) & 0xF0000000
        diffs = []
        for base, before in [(DATA_BASE, data), (STACK_BASE, stack)] + list(self.pages.items()):
            after = bytes(uc.mem_read(base, len(before)))
            if after != before:
                diffs += [(base + i, after[i]) for i in range(len(before)) if after[i] != before[i]]
        return out_regs, out_flags, diffs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("harness")
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--only", type=lambda s: int(s, 16), nargs="*")
    ap.add_argument("--cases", default="build/difftest_cases.bin")
    args = ap.parse_args()

    rc = Recompiler(args.image)
    runner = Runner(rc)
    funcs = [f for f in sorted(rc.entries) if f not in rc.stubs and f not in rc.thumb]
    if args.only:
        funcs = [a - CODE_BASE for a in args.only]
    cases, discarded = [], 0
    for i, f in enumerate(funcs):
        for s in range(args.seeds):
            seed = 0x9E3779B9 ^ (f * 2654435761 + s * 40503) & 0xFFFFFFFF
            res = runner.run(f, seed)
            if res is None:
                discarded += 1
                continue
            cases.append((f, seed, res))
        if i % 500 == 0:
            print(f"  unicorn {i}/{len(funcs)} functions, {len(cases)} cases", file=sys.stderr)
    print(f"cases: {len(cases)}  discarded: {discarded}  functions covered: {len({c[0] for c in cases})}"
          f" of {len(funcs)}", flush=True)

    buf = bytearray(struct.pack("<4sI", b"DTC1", len(cases)))
    for f, seed, (regs, flags, diffs) in cases:
        buf += struct.pack("<II15III", CODE_BASE + f, seed, *regs, flags, len(diffs))
        for a, v in diffs:
            buf += struct.pack("<II", a, v)
    Path(args.cases).write_bytes(buf)
    r = subprocess.run([str(Path(args.harness).resolve()), args.image, args.cases])
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
