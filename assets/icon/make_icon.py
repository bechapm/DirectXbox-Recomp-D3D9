"""Draw DirectXboxRecomp's icon and render it at every size the platforms use.

    python3 assets/icon/make_icon.py

Writes xboxrecomp.svg (the source of truth), png/<n>.png and xboxrecomp.ico
beside this script, and src/config/recomp_icon.h: the 64-pixel image as RGBA
bytes, which the SDL windows (launcher and game) set as their icon. Windows
takes the .ico through cmake/title_icon.cmake. Needs rsvg-convert (librsvg).

The mark is our own: a black X, bevelled and extruded toward the lower right,
on a green tile. It is deliberately not Microsoft's logo or the prototype
console's shape (no jewel, no slab body); see README.md, "Trademarks".
"""

import math
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
HEADER = HERE.parent.parent / "src" / "config" / "recomp_icon.h"
HEADER_SIZE = 64
SIZES = [16, 24, 32, 48, 64, 128, 256, 512]
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]

# The X's outline, clockwise in a 256-unit box: arms cut square at the ends.
X = [(40, 40), (92, 40), (128, 91), (164, 40), (216, 40), (157, 128),
     (216, 216), (164, 216), (128, 165), (92, 216), (40, 216), (99, 128)]
DEPTH = (9.0, 11.0)       # the extrusion, toward the lower right
BEVEL = 7.0               # how far the top face is inset from the outline
LIGHT = (-0.6, -0.8)      # where the light comes from: the upper left


def inset(poly, d):
    """The polygon with every edge moved inward by d (clockwise winding)."""
    n = len(poly)
    lines = []
    for i in range(n):
        (x0, y0), (x1, y1) = poly[i], poly[(i + 1) % n]
        ex, ey = x1 - x0, y1 - y0
        length = math.hypot(ex, ey)
        nx, ny = -ey / length, ex / length       # inward for clockwise
        lines.append(((x0 + nx * d, y0 + ny * d), (ex, ey)))
    out = []
    for i in range(n):
        (p, r), (q, s) = lines[i - 1], lines[i]
        cross = r[0] * s[1] - r[1] * s[0]
        t = ((q[0] - p[0]) * s[1] - (q[1] - p[1]) * s[0]) / cross
        out.append((p[0] + t * r[0], p[1] + t * r[1]))
    return out


def shade(edge_dir, lit, dark):
    """A colour between dark and lit by how much an edge faces the light."""
    ex, ey = edge_dir
    length = math.hypot(ex, ey)
    nx, ny = ey / length, -ex / length           # outward for clockwise
    k = max(0.0, min(1.0, 0.5 + 0.5 * (nx * LIGHT[0] + ny * LIGHT[1])))
    return "#%02x%02x%02x" % tuple(round(d + (l - d) * k) for l, d in zip(lit, dark))


def pts(poly, dx=0.0, dy=0.0):
    return " ".join("%.2f,%.2f" % (x + dx, y + dy) for x, y in poly)


def svg():
    n = len(X)
    sides = []
    # The extrusion: one quad per edge that faces away from the viewer's
    # side of the depth, drawn farthest first.
    for i in range(n):
        a, b = X[i], X[(i + 1) % n]
        ex, ey = b[0] - a[0], b[1] - a[1]
        if ey * DEPTH[0] - ex * DEPTH[1] < 0:    # faces up-left: hidden
            continue
        quad = [a, b, (b[0] + DEPTH[0], b[1] + DEPTH[1]), (a[0] + DEPTH[0], a[1] + DEPTH[1])]
        key = (a[0] + b[0]) * DEPTH[0] + (a[1] + b[1]) * DEPTH[1]
        sides.append((key, quad, shade((ex, ey), (0x2a, 0x30, 0x26), (0x02, 0x03, 0x02))))
    sides.sort()
    top = inset(X, BEVEL)
    bevels = []
    for i in range(n):
        j = (i + 1) % n
        quad = [X[i], X[j], top[j], top[i]]
        ex, ey = X[j][0] - X[i][0], X[j][1] - X[i][1]
        bevels.append((quad, shade((ex, ey), (0x5c, 0x66, 0x55), (0x05, 0x06, 0x05))))

    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">',
           "  <!-- DirectXboxRecomp's own mark, drawn by make_icon.py: edit that, not this. -->",
           "  <defs>",
           '    <linearGradient id="tile" x1="0" y1="0" x2="0" y2="1">'
           '<stop offset="0" stop-color="#86cc1e"/><stop offset="1" stop-color="#2c740b"/></linearGradient>',
           '    <radialGradient id="glow" cx="0.3" cy="0.2" r="0.8">'
           '<stop offset="0" stop-color="#e2ff8c" stop-opacity="0.5"/>'
           '<stop offset="1" stop-color="#e2ff8c" stop-opacity="0"/></radialGradient>',
           '    <linearGradient id="top" x1="0" y1="0" x2="1" y2="1">'
           '<stop offset="0" stop-color="#2b3128"/><stop offset="0.5" stop-color="#121510"/>'
           '<stop offset="1" stop-color="#070806"/></linearGradient>',
           '    <filter id="blur" x="-20%" y="-20%" width="140%" height="140%">'
           '<feGaussianBlur stdDeviation="6"/></filter>',
           "  </defs>",
           '  <rect x="8" y="8" width="240" height="240" rx="52" fill="url(#tile)"/>',
           '  <rect x="8" y="8" width="240" height="240" rx="52" fill="url(#glow)"/>',
           '  <rect x="9" y="9" width="238" height="238" rx="51" fill="none" '
           'stroke="#efffc4" stroke-opacity="0.4" stroke-width="2"/>',
           '  <polygon points="%s" fill="#0a2a02" opacity="0.5" filter="url(#blur)"/>'
           % pts(X, DEPTH[0] + 8, DEPTH[1] + 10)]
    for _, quad, colour in sides:
        out.append('  <polygon points="%s" fill="%s" stroke="%s" stroke-width="0.6" '
                   'stroke-linejoin="round"/>' % (pts(quad), colour, colour))
    for quad, colour in bevels:
        out.append('  <polygon points="%s" fill="%s" stroke="%s" stroke-width="0.6" '
                   'stroke-linejoin="round"/>' % (pts(quad), colour, colour))
    out.append('  <polygon points="%s" fill="url(#top)"/>' % pts(top))
    out.append("</svg>")
    return "\n".join(out) + "\n"


def ico(pngs):
    """A Windows .ico holding the PNGs as they are (Vista and later)."""
    head = struct.pack("<HHH", 0, 1, len(pngs))
    entries, data = b"", b""
    offset = 6 + 16 * len(pngs)
    for size, blob in pngs:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset)
        data += blob
        offset += len(blob)
    return head + entries + data


def png_rgba(path):
    """An 8-bit RGBA PNG, as rsvg-convert writes it, decoded to raw bytes."""
    data = path.read_bytes()
    pos, idat = 8, b""
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, colour = struct.unpack(">IIBB", body[:10])
            if (depth, colour) != (8, 6):
                raise SystemExit(f"make_icon: {path} is not 8-bit RGBA")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw, stride, out, prev, o = zlib.decompress(idat), w * 4, bytearray(), bytearray(w * 4), 0
    for _ in range(h):
        kind, line = raw[o], bytearray(raw[o + 1:o + 1 + stride])
        o += 1 + stride
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b, c = prev[x], prev[x - 4] if x >= 4 else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return w, h, bytes(out)


def header(w, h, rgba):
    rows = [", ".join("0x%02x" % v for v in rgba[i:i + 16]) for i in range(0, len(rgba), 16)]
    return ("/* DirectXboxRecomp's icon, %dx%d RGBA, for SDL_SetWindowIcon.\n"
            " * Written by assets/icon/make_icon.py: edit that, not this. */\n"
            "#ifndef RECOMP_ICON_H\n#define RECOMP_ICON_H\n\n"
            "#define RECOMP_ICON_SIZE %d\n\n"
            "static const unsigned char recomp_icon_rgba[%d] = {\n    %s\n};\n\n"
            "#endif\n" % (w, h, w, len(rgba), ",\n    ".join(rows)))


def main():
    src = HERE / "xboxrecomp.svg"
    src.write_text(svg())
    if not shutil.which("rsvg-convert"):
        raise SystemExit("make_icon: rsvg-convert is needed to render the PNGs")
    (HERE / "png").mkdir(exist_ok=True)
    for n in SIZES:
        subprocess.run(["rsvg-convert", "-w", str(n), "-h", str(n), str(src),
                        "-o", str(HERE / "png" / f"{n}.png")], check=True)
    (HERE / "xboxrecomp.ico").write_bytes(
        ico([(n, (HERE / "png" / f"{n}.png").read_bytes()) for n in ICO_SIZES]))
    HEADER.write_text(header(*png_rgba(HERE / "png" / f"{HEADER_SIZE}.png")))
    print(f"make_icon: {src.name}, png/ ({', '.join(map(str, SIZES))}), xboxrecomp.ico, "
          f"{HEADER.relative_to(HERE.parent.parent)}")


if __name__ == "__main__":
    main()
