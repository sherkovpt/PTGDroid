"""Function discovery for an EKA1 ARM image: finds function entries and code/data layout."""
import json
import struct
import sys
from pathlib import Path

import e32

RELOC_TEXT, RELOC_DATA, RELOC_INFERRED = 1, 2, 3


class Analysis:
    def __init__(self, path, base=None):
        img = e32.load(path)
        if base is not None and base != img.hdr["code_base"]:
            img = e32.rebase(img, base)
        self.img = img
        h = img.hdr
        self.base = h["code_base"]
        self.text_size = h["text_size"]
        self.code = img.code
        self.words = struct.unpack_from("<%dI" % (len(img.code) // 4), img.code)
        self.reloc_sites = {off for off, _ in img.code_relocs}
        self.iat = {}
        for b in img.imports:
            for o, off in zip(b.ordinals, b.iat_offsets):
                self.iat[off] = (b.dll, o)

    def in_text(self, addr):
        return self.base <= addr < self.base + self.text_size

    def word(self, off):
        return self.words[off // 4]

    def discover(self):
        arm, thumb = set(), set()
        pointers = {}
        # Every relocated word is a pointer; ones pointing into .text are code pointers
        # (vtables, callbacks) or literal-pool references to read-only data.
        for off in self.reloc_sites:
            v = self.word(off)
            if self.in_text(v & ~1):
                pointers[off] = v
        # BL targets: only scan words that are not relocated data.
        bl_targets = set()
        for i in range(self.text_size // 4):
            off = i * 4
            if off in self.reloc_sites:
                continue
            w = self.words[i]
            if (w & 0x0F000000) == 0x0B000000 and (w >> 28) != 0xF:
                imm = w & 0xFFFFFF
                if imm & 0x800000:
                    imm -= 1 << 24
                t = off + 8 + imm * 4
                if 0 <= t < self.text_size:
                    bl_targets.add(t)
        arm |= bl_targets
        for off, v in pointers.items():
            t = (v & ~1) - self.base
            if v & 1:
                thumb.add(t)
            elif self.looks_like_code(t):
                arm.add(t)
        self.strong_entries = set(bl_targets)
        self.strong_entries.add(self.img.hdr["entry_point"])
        for e in self.img.exports:
            self.strong_entries.add(e - self.base if e >= self.base else e)
        arm |= self.strong_entries
        return sorted(arm), sorted(thumb), pointers

    def looks_like_code(self, off):
        """Heuristic: a pointer target is code if it starts with a plausible ARM prologue/instruction."""
        if off % 4 or off in self.reloc_sites:
            return False
        w = self.word(off)
        cond = w >> 28
        return cond == 0xE


def main():
    a = Analysis(sys.argv[1])
    arm, thumb, pointers = a.discover()
    code_ptrs = sum(1 for v in pointers.values() if a.looks_like_code((v & ~1) - a.base))
    print(f"relocated words pointing into .text: {len(pointers)} (code-looking: {code_ptrs})")
    print(f"ARM function entries: {len(arm)}  Thumb entries: {len(thumb)}")
    if len(sys.argv) > 2:
        Path(sys.argv[2]).write_text(json.dumps({"arm": arm, "thumb": thumb}))


if __name__ == "__main__":
    main()
