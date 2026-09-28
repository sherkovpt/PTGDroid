"""Find GCC 2.95 (vtable-thunks) vtables in the image and infer virtual slot numbers of system
methods from the import stubs that appear in them.

Layout: vptr -> [0, 0, slot0, slot1, ...]; slots are relocated pointers to code.
Output: analysis/vtables.json {vtables: {addr: [entries]}, slots: {method_name: slot}}
"""
import json
import struct
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdis import rc, base  # noqa: E402

code = rc.a.img.code
text = rc.text_size
relocs = rc.a.reloc_sites


def word(off):
    return struct.unpack_from("<I", code, off)[0]


def slot_name(w):
    off = w - base
    if off in rc.stubs:
        return rc.imports[rc.stubs[off]]["name"] or f"{rc.imports[rc.stubs[off]]['lib']}@{rc.imports[rc.stubs[off]]['ordinal']}"
    if off in rc.entries:
        return f"fn_{w:08x}"
    return None


def is_code_ptr(off):
    return off in relocs and 0 <= word(off) - base < text and slot_name(word(off)) is not None


vtables = {}
off = 0
while off < text - 12:
    if word(off) == 0 and word(off + 4) == 0 and is_code_ptr(off + 8) and off not in relocs:
        slots = []
        p = off + 8
        while p < text and is_code_ptr(p):
            slots.append(slot_name(word(p)))
            p += 4
        vtables[base + off] = slots
        off = p
    else:
        off += 4

seen = defaultdict(set)
for vt, slots in vtables.items():
    for i, s in enumerate(slots):
        if s and not s.startswith("fn_") and s != "__pure_virtual":
            seen[s].add(i)
slots = {k: sorted(v) for k, v in seen.items()}
conflicts = {k: v for k, v in slots.items() if len(v) > 1}

out = Path(__file__).resolve().parent.parent / "analysis" / "vtables.json"
out.write_text(json.dumps({"vtables": {f"{k:08x}": v for k, v in vtables.items()},
                           "slots": {k: v[0] for k, v in slots.items()}}, indent=1))
print(f"vtables: {len(vtables)}  system virtual methods with known slot: {len(slots)}  conflicts: {len(conflicts)}")
for k, v in sorted(conflicts.items()):
    print("  conflict", k, v)
