"""Reader for Pathway to Glory data.pak: [u32 data_start][u32 count] then count x 17-byte entries."""
import struct
import sys
import zlib
from collections import Counter

ENTRY = struct.Struct("<IIIBI")  # name_hash, stored_size, real_size, compressed, offset


def entries(d):
    data_start, count = struct.unpack_from("<II", d, 0)
    assert 8 + count * ENTRY.size == data_start
    for i in range(count):
        yield ENTRY.unpack_from(d, 8 + i * ENTRY.size)


def read(d, e):
    h, stored, real, comp, off = e
    blob = d[off:off + stored]
    if comp:
        blob = zlib.decompress(blob)
    assert len(blob) == real
    return blob


def magic(b):
    head = b[:4]
    if all(32 <= c < 127 for c in head):
        return head.decode()
    return head.hex()


if __name__ == "__main__":
    d = open(sys.argv[1], "rb").read()
    es = list(entries(d))
    kinds = Counter()
    total = 0
    for e in es:
        b = read(d, e)
        total += len(b)
        kinds[magic(b)] += 1
    print(f"{len(es)} entries, {total / 1e6:.1f} MB uncompressed, {sum(e[3] for e in es)} compressed")
    for k, n in kinds.most_common(30):
        print(f"{n:6}  {k!r}")
