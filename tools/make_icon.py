"""Generates the PTGDroid launcher icon (original design): adaptive vector icon + legacy PNGs.
Olive background, khaki badge, dark star and two gold sergeant chevrons."""
import math, struct, zlib
from pathlib import Path

RES = Path(__file__).resolve().parent.parent / "android/app/src/main/res"
OLIVE, KHAKI, DARK, GOLD = (0x3E, 0x4A, 0x1E), (0xD2, 0xBC, 0x86), (0x2B, 0x33, 0x14), (0xE8, 0xB8, 0x3A)

# Geometry in the 108x108 adaptive-icon space (safe zone = central 66x66).
CX, CY, BADGE_R = 54.0, 52.0, 27.0
def star(cx, cy, R, r):
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        rad = R if i % 2 == 0 else r
        pts.append((cx + rad * math.cos(a), cy + rad * math.sin(a)))
    return pts
STAR = star(CX, CY - 8, 13.0, 5.4)
def chevron(y, w=15.0, h=5.0, t=3.2):
    return [(CX - w, y), (CX, y + h), (CX + w, y), (CX + w, y + t), (CX, y + h + t), (CX - w, y + t)]
CHEVRONS = [chevron(59.5), chevron(66.0)]

def path(pts):
    return "M" + " L".join(f"{x:.2f},{y:.2f}" for x, y in pts) + " Z"

def write_vector():
    (RES / "drawable").mkdir(parents=True, exist_ok=True)
    (RES / "mipmap-anydpi-v26").mkdir(parents=True, exist_ok=True)
    hexc = lambda c: "#FF%02X%02X%02X" % c
    fg = f'''<?xml version="1.0" encoding="utf-8"?>
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="108dp" android:height="108dp" android:viewportWidth="108" android:viewportHeight="108">
    <path android:fillColor="{hexc(GOLD)}" android:pathData="M{CX},{CY} m-{BADGE_R + 2},0 a{BADGE_R + 2},{BADGE_R + 2} 0 1,0 {2 * (BADGE_R + 2)},0 a{BADGE_R + 2},{BADGE_R + 2} 0 1,0 -{2 * (BADGE_R + 2)},0"/>
    <path android:fillColor="{hexc(KHAKI)}" android:pathData="M{CX},{CY} m-{BADGE_R},0 a{BADGE_R},{BADGE_R} 0 1,0 {2 * BADGE_R},0 a{BADGE_R},{BADGE_R} 0 1,0 -{2 * BADGE_R},0"/>
    <path android:fillColor="{hexc(DARK)}" android:pathData="{path(STAR)}"/>
    <path android:fillColor="{hexc(GOLD)}" android:pathData="{path(CHEVRONS[0])}"/>
    <path android:fillColor="{hexc(GOLD)}" android:pathData="{path(CHEVRONS[1])}"/>
</vector>
'''
    (RES / "drawable/ic_launcher_foreground.xml").write_text(fg)
    (RES / "drawable/ic_launcher_background.xml").write_text(f'''<?xml version="1.0" encoding="utf-8"?>
<shape xmlns:android="http://schemas.android.com/apk/res/android" android:shape="rectangle">
    <solid android:color="{hexc(OLIVE)}"/>
</shape>
''')
    (RES / "mipmap-anydpi-v26/ic_launcher.xml").write_text('''<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@drawable/ic_launcher_background"/>
    <foreground android:drawable="@drawable/ic_launcher_foreground"/>
</adaptive-icon>
''')

def inside_poly(x, y, pts):
    c = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]; xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            c = not c
        j = i
    return c

def color_at(x, y):
    """Colour of the legacy icon at (x, y) in 108-space; None = transparent (outside rounded square)."""
    # legacy icons: rounded square covering 8..100
    m, rad = 8.0, 18.0
    if x < m or x > 108 - m or y < m or y > 108 - m:
        return None
    for cx, cy in ((m + rad, m + rad), (108 - m - rad, m + rad), (m + rad, 108 - m - rad), (108 - m - rad, 108 - m - rad)):
        if (x < m + rad or x > 108 - m - rad) and (y < m + rad or y > 108 - m - rad):
            if (x - cx) ** 2 + (y - cy) ** 2 > rad * rad and abs(x - cx) <= rad and abs(y - cy) <= rad:
                return None
    d2 = (x - CX) ** 2 + (y - CY) ** 2
    col = OLIVE
    if d2 <= (BADGE_R + 2) ** 2: col = GOLD
    if d2 <= BADGE_R ** 2: col = KHAKI
    if inside_poly(x, y, STAR): col = DARK
    if any(inside_poly(x, y, ch) for ch in CHEVRONS): col = GOLD
    return col

def write_png(path, size, ss=4):
    rows = []
    for py in range(size):
        row = bytearray([0])
        for px in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    x = (px + (sx + 0.5) / ss) * 108 / size
                    y = (py + (sy + 0.5) / ss) * 108 / size
                    c = color_at(x, y)
                    if c:
                        acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]; acc[3] += 255
            n = ss * ss
            a = acc[3] // n
            row += bytes([acc[0] // max(1, acc[3] // 255), acc[1] // max(1, acc[3] // 255),
                          acc[2] // max(1, acc[3] // 255), a] if a else [0, 0, 0, 0])
        rows.append(bytes(row))
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)

if __name__ == "__main__":
    write_vector()
    for folder, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        write_png(RES / f"mipmap-{folder}/ic_launcher.png", size)
    write_png(Path(__file__).resolve().parent.parent / "build/icon_preview.png", 432)
    print("icon written")
