"""Dump guest words at an address, naming import stubs and guest functions: vtable.py <addr> [n]"""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdis import rc, name, base  # noqa

for a in sys.argv[1:]:
    start, _, n = a.partition(":")
    start = int(start, 16); n = int(n or 24)
    for i in range(n):
        off = start - base + 4 * i
        w = struct.unpack_from("<I", rc.a.img.code, off)[0]
        tag = name(w) or ("fn" if (w - base) in rc.entries else "")
        print(f"{start + 4*i:08x} [{i:2}] {w:08x} {tag}")
    print("--")
