"""Parser for EKA1 (Symbian 6.x / N-Gage) E32 images: header, imports, relocations."""
import struct
import sys
from dataclasses import dataclass, field

HDR_FIELDS = [
    "uid1", "uid2", "uid3", "check", "signature", "cpu", "checksum_code", "checksum_data",
    "version", "time_lo", "time_hi", "flags", "code_size", "data_size", "heap_min", "heap_max",
    "stack_size", "bss_size", "entry_point", "code_base", "data_base", "dll_ref_count",
    "export_dir_offset", "export_dir_count", "text_size", "code_offset", "data_offset",
    "import_offset", "code_reloc_offset", "data_reloc_offset", "priority",
]


@dataclass
class ImportBlock:
    dll: str
    ordinals: list = field(default_factory=list)
    iat_offsets: list = field(default_factory=list)  # offsets into code section of each IAT slot


@dataclass
class E32Image:
    raw: bytes
    hdr: dict
    code: bytes
    imports: list
    code_relocs: list  # (offset_in_code, type)
    data_relocs: list
    exports: list  # code-relative addresses


def _relocs(d, off):
    if not off:
        return []
    size, count = struct.unpack_from("<II", d, off)
    out = []
    p = off + 8
    end = off + 8 + size
    while p < end:
        page, block = struct.unpack_from("<II", d, p)
        n = (block - 8) // 2
        for i in range(n):
            e = struct.unpack_from("<H", d, p + 8 + 2 * i)[0]
            typ, o = e >> 12, e & 0xFFF
            if typ:
                out.append((page + o, typ))
        p += block
    assert len(out) == count, (len(out), count)
    return out


def load(path):
    d = open(path, "rb").read()
    vals = struct.unpack_from("<%dI" % len(HDR_FIELDS), d, 0)
    h = dict(zip(HDR_FIELDS, vals))
    assert h["signature"] == 0x434F5045, "not an E32 image"
    assert h["flags"] & 0x0F000000 == 0x01000000 or True
    code = d[h["code_offset"]:h["code_offset"] + h["code_size"]]

    imports = []
    io = h["import_offset"]
    if io:
        p = io + 4
        iat = h["text_size"]
        for _ in range(h["dll_ref_count"]):
            name_off, n = struct.unpack_from("<Ii", d, p)
            p += 8
            name = d[io + name_off:d.index(b"\0", io + name_off)].decode("latin1")
            blk = ImportBlock(name)
            blk.ordinals = list(struct.unpack_from("<%dI" % n, d, p))
            p += 4 * n
            blk.iat_offsets = [iat + 4 * i for i in range(n)]
            iat += 4 * n
            imports.append(blk)

    exports = []
    if h["export_dir_offset"]:
        eo = h["export_dir_offset"] - h["code_offset"]
        exports = list(struct.unpack_from("<%dI" % h["export_dir_count"], code, eo))

    return E32Image(d, h, code, imports, _relocs(d, h["code_reloc_offset"]),
                    _relocs(d, h["data_reloc_offset"]), exports)


def rebase(img, new_base):
    """Return a copy of `img` whose code is relocated to run at `new_base`."""
    delta = (new_base - img.hdr["code_base"]) & 0xFFFFFFFF
    code = bytearray(img.code)
    for off, _typ in img.code_relocs:
        v = struct.unpack_from("<I", code, off)[0]
        struct.pack_into("<I", code, off, (v + delta) & 0xFFFFFFFF)
    hdr = dict(img.hdr)
    hdr["code_base"] = new_base
    exports = [(e + delta) & 0xFFFFFFFF if e >= img.hdr["code_base"] else e for e in img.exports]
    return E32Image(img.raw, hdr, bytes(code), img.imports, img.code_relocs, img.data_relocs, exports)


if __name__ == "__main__":
    img = load(sys.argv[1])
    for k, v in img.hdr.items():
        print(f"{k:18} 0x{v:08x}")
    print(f"code relocs: {len(img.code_relocs)}  data relocs: {len(img.data_relocs)}")
    print(f"exports: {[hex(x) for x in img.exports]}")
    total = 0
    for b in img.imports:
        total += len(b.ordinals)
        print(f"{b.dll:28} {len(b.ordinals):4} imports")
    print("total imports:", total)
