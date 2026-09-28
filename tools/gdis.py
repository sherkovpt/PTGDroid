"""Disassemble guest code with import stub names: dis.py <addr_hex> [count]"""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "recomp"))
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
from armrecomp import Recompiler

import os
# Your own copy of the game: set PTG_APP, or put the memory-card files in <repo>/game/.
IMG = os.environ.get("PTG_APP", str(Path(__file__).resolve().parents[1] / "game/system/apps/6r72/6r72.app"))
rc = Recompiler(IMG)
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
base = rc.base

def name(t):
    off = t - base
    if off in rc.stubs:
        im = rc.imports[rc.stubs[off]]
        return f"<{im['lib']}:{im['name'] or im['ordinal']}>"
    return ""

def main(argv):
  for a in argv:
      start, _, n = a.partition(":")
      start = int(start, 16); n = int(n or 24)
      o = start - base
      for i in md.disasm(rc.a.img.code[o:o + 4 * n], start):
          extra = ""
          if i.mnemonic.startswith("b") and i.op_str.startswith("#"):
              extra = name(int(i.op_str[1:], 16))
          if i.mnemonic.startswith("ldr") and "[pc" in i.op_str:
              import struct
              off = int(i.op_str.split("#")[1].rstrip("]"), 16) if "#" in i.op_str else 0
              lit = struct.unpack_from("<I", rc.a.img.code, i.address + 8 + off - base)[0]
              extra = f"=0x{lit:08x} {name(lit)}"
          print(f"{i.address:08x}  {i.mnemonic:8} {i.op_str:30} {extra}")
      print("--")

if __name__ == '__main__':
    main(sys.argv[1:])
