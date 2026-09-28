"""Convert 24-bit BMP dumps to PNG: bmp2png.py file.bmp... (writes file.png next to each)"""
import struct, sys, zlib
for src in sys.argv[1:]:
    d = open(src, 'rb').read()
    w, h = struct.unpack_from('<ii', d, 18)
    off = struct.unpack_from('<I', d, 10)[0]
    flip, h = h > 0, abs(h)
    row = (w * 3 + 3) & ~3
    rows = []
    for y in range(h):
        sy = h - 1 - y if flip else y
        r = d[off + sy * row: off + sy * row + w * 3]
        rows.append(b'\0' + b''.join(bytes((r[i + 2], r[i + 1], r[i])) for i in range(0, w * 3, 3)))
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(src[:-4] + '.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                                        + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))
