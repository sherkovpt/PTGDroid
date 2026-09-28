"""Map (dll, ordinal) imports of an E32 image to symbol names via EKA2L1's epoc6 export lists."""
import json
import re
import sys
from pathlib import Path

import e32

HERE = Path(__file__).resolve().parent
DEFS = [HERE.parent / "third_party/eka2l1/epoc6_n.def", HERE.parent / "third_party/eka2l1/epoc6.def"]
# Libraries shipped with the game itself: recompiled alongside it, so no symbol names needed.
BUNDLED = {"arenaframework", "gamecomms", "gameutils", "zlib"}


def load_defs():
    libs = {}
    for p in DEFS:
        cur = None
        seen = set()
        for line in open(p, encoding="latin1"):
            m = re.match(r"LIB\((\w+)\)", line)
            if m:
                cur = m.group(1).lower()
                if cur in libs and cur not in seen:
                    cur = None  # earlier (more specific) file wins
                else:
                    libs[cur] = []
                    seen.add(cur)
                continue
            m = re.match(r'EXPORT\("(.*)", (\d+)\)', line)
            if m and cur:
                libs[cur].append(m.group(1))
    return libs


def lib_key(dll):
    return re.sub(r"\[.*", "", dll).split(".")[0].lower()


def resolve(img, libs):
    out = []
    for b in img.imports:
        key = lib_key(b.dll)
        names = libs.get(key, [])
        for o, iat in zip(b.ordinals, b.iat_offsets):
            if key in BUNDLED:
                name, status = None, "bundled"
            elif o <= len(names):
                name, status = names[o - 1], "resolved"
            else:
                name, status = None, "unknown"
            out.append({"lib": key, "ordinal": o, "iat": iat, "name": name, "status": status})
    return out


if __name__ == "__main__":
    img = e32.load(sys.argv[1])
    rows = resolve(img, load_defs())
    if len(sys.argv) > 2:
        Path(sys.argv[2]).write_text(json.dumps(rows, indent=1))
    for status in ("resolved", "bundled", "unknown"):
        print(status, sum(r["status"] == status for r in rows))
    for r in rows:
        if r["status"] == "unknown":
            print(f"  unknown {r['lib']}@{r['ordinal']}")
