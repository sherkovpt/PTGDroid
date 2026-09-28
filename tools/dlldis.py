"""Disassemble any E32 image with import-stub names: dlldis.py <image> <addr>[:count] ..."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM  # noqa: E402
import e32  # noqa: E402
from resolve_imports import load_defs, resolve  # noqa: E402

img = e32.load(sys.argv[1])
base = img.hdr["code_base"]
rows = resolve(img, load_defs())
iat0 = base + img.hdr["text_size"]
words = struct.unpack("<%dI" % (len(img.code) // 4), img.code)
stubs = {}
for i in range(len(words) - 3):
    if words[i:i + 3] == (0xE59FC004, 0xE59CC000, 0xE12FFF1C):
        r = rows[(words[i + 3] - iat0) // 4]
        stubs[base + 4 * i] = f"<{r['lib']}:{r['name'] or r['ordinal']}>"
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
for arg in sys.argv[2:]:
    a, _, n = arg.partition(":")
    a = int(a, 16)
    n = int(n or 24)
    for ins in md.disasm(img.code[a - base:a - base + 4 * n], a):
        extra = ""
        if ins.mnemonic.startswith("b") and ins.op_str.startswith("#"):
            extra = stubs.get(int(ins.op_str[1:], 16), "")
        if ins.mnemonic.startswith("ldr") and "[pc" in ins.op_str and "#" in ins.op_str:
            lit = struct.unpack_from("<I", img.code, ins.address + 8 + int(ins.op_str.split("#")[1].rstrip("]"), 16) - base)[0]
            extra = f"=0x{lit:08x} {stubs.get(lit, '')}"
        print(f"{ins.address:08x}  {ins.mnemonic:8} {ins.op_str:30} {extra}")
    print("--")
