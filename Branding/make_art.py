"""Renders the icon and banner for plasma.modeling.

No rasteriser or Pillow on this machine, so PNG bytes are written directly - zlib-compressed
scanlines with a filter byte each. Drawn at 3x and box-downsampled for antialiasing.

Motif: an isometric blockout - a wall being carved by a dashed subtractive cutter, on the teal
accent the editor uses. Matches the plan document's viewport mock.
"""

import math
import os
import struct
import zlib

SS = 3  # supersample factor

# ---- palette ----------------------------------------------------------------
# blockout orange (PrototypeGrey) + editor teal accent, on a dark indigo ground
TOP = (0xE8, 0xB1, 0x84)
FRONT = (0xD6, 0x8D, 0x5F)
SIDE = (0xB0, 0x6F, 0x47)
EDGE = (0x6E, 0x43, 0x28)
TEAL = (0x14, 0xE0, 0xBD)
GRID = (0xFF, 0xFF, 0xFF)
BG_TOP = (0x12, 0x18, 0x2E)
BG_BOT = (0x07, 0x0A, 0x18)


def write_png(path, w, h, rgba):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def mix(c0, c1, t):
    t = 0.0 if t < 0 else (1.0 if t > 1 else t)
    return (c0[0] + (c1[0] - c0[0]) * t, c0[1] + (c1[1] - c0[1]) * t, c0[2] + (c1[2] - c0[2]) * t)


def over(dst, src, a):
    return (dst[0] + (src[0] - dst[0]) * a,
            dst[1] + (src[1] - dst[1]) * a,
            dst[2] + (src[2] - dst[2]) * a)


def point_in_poly(px, py, poly):
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > py) != (yj > py) and px < (xj - xi) * (py - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def seg_dist(px, py, x0, y0, x1, y1):
    dx, dy = x1 - x0, y1 - y0
    if dx == 0 and dy == 0:
        return math.hypot(px - x0, py - y0)
    t = ((px - x0) * dx + (py - y0) * dy) / (dx * dx + dy * dy)
    t = 0.0 if t < 0 else (1.0 if t > 1 else t)
    return math.hypot(px - (x0 + t * dx), py - (y0 + t * dy))


def render(w, h, cx, cy, scale):
    """Isometric wall + dashed cutter, centered on (cx, cy)."""
    W, H = w * SS, h * SS
    cx, cy, scale = cx * SS, cy * SS, scale * SS

    def iso(x, y, z):
        return (cx + (x - y) * 0.86 * scale, cy + (x + y) * 0.5 * scale - z * 0.95 * scale)

    # wall: 6 long, 1 deep, 3 high, centered-ish
    x0, x1, y0, y1, z0, z1 = -3.0, 3.0, -0.5, 0.5, 0.0, 3.0
    front = [iso(x0, y0, z0), iso(x1, y0, z0), iso(x1, y0, z1), iso(x0, y0, z1)]
    top = [iso(x0, y0, z1), iso(x1, y0, z1), iso(x1, y1, z1), iso(x0, y1, z1)]
    side = [iso(x1, y0, z0), iso(x1, y1, z0), iso(x1, y1, z1), iso(x1, y0, z1)]

    # doorway hole in the front face
    d0, d1, dz = -0.9, 0.9, 2.1
    hole = [iso(d0, y0, z0), iso(d1, y0, z0), iso(d1, y0, dz), iso(d0, y0, dz)]

    # dashed cutter box around the doorway
    c0x, c1x, c0y, c1y, cz = d0, d1, -1.1, 1.1, 2.1
    cutter_pts = []
    for (ax, ay, az), (bx, by, bz) in [
        ((c0x, c0y, 0), (c1x, c0y, 0)), ((c1x, c0y, 0), (c1x, c1y, 0)),
        ((c1x, c1y, 0), (c0x, c1y, 0)), ((c0x, c1y, 0), (c0x, c0y, 0)),
        ((c0x, c0y, cz), (c1x, c0y, cz)), ((c1x, c0y, cz), (c1x, c1y, cz)),
        ((c1x, c1y, cz), (c0x, c1y, cz)), ((c0x, c1y, cz), (c0x, c0y, cz)),
        ((c0x, c0y, 0), (c0x, c0y, cz)), ((c1x, c0y, 0), (c1x, c0y, cz)),
        ((c1x, c1y, 0), (c1x, c1y, cz)), ((c0x, c1y, 0), (c0x, c1y, cz)),
    ]:
        cutter_pts.append((iso(ax, ay, az), iso(bx, by, bz)))

    grid_lines = []
    for gi in range(-4, 6):
        grid_lines.append((iso(gi, -4, 0), iso(gi, 5, 0)))
        grid_lines.append((iso(-4, gi, 0), iso(6, gi, 0)))

    stride = W * 4
    buf = bytearray(W * H * 4)

    lw = max(1.4, 0.028 * scale)  # edge line width

    for py in range(H):
        t = py / (H - 1)
        bg = mix(BG_TOP, BG_BOT, t)
        for px in range(W):
            c = bg

            # ground grid (subtle)
            gd = min(seg_dist(px, py, a[0], a[1], b[0], b[1]) for a, b in grid_lines)
            if gd < lw:
                c = over(c, GRID, 0.06 * (1.0 - gd / lw))

            in_hole = point_in_poly(px, py, hole)

            if point_in_poly(px, py, top):
                c = TOP
            elif point_in_poly(px, py, side):
                c = SIDE
            elif point_in_poly(px, py, front) and not in_hole:
                c = FRONT
            elif in_hole:
                c = mix(bg, (0x0C, 0x10, 0x20), 0.5)

            # face edges
            for poly in (front, top, side, hole):
                n = len(poly)
                for i in range(n):
                    a, b = poly[i], poly[(i + 1) % n]
                    d = seg_dist(px, py, a[0], a[1], b[0], b[1])
                    if d < lw:
                        c = over(c, EDGE, 0.85 * (1.0 - d / lw))

            # dashed teal cutter, drawn on top
            for a, b in cutter_pts:
                d = seg_dist(px, py, a[0], a[1], b[0], b[1])
                if d < lw * 1.15:
                    seg_len = math.hypot(b[0] - a[0], b[1] - a[1])
                    if seg_len > 0:
                        tt = ((px - a[0]) * (b[0] - a[0]) + (py - a[1]) * (b[1] - a[1])) / (seg_len * seg_len)
                        tt = 0.0 if tt < 0 else (1.0 if tt > 1 else tt)
                        dash = (tt * seg_len / (0.16 * scale)) % 2.0
                        if dash < 1.2:
                            c = over(c, TEAL, 0.95 * (1.0 - d / (lw * 1.15)))

            o = py * stride + px * 4
            buf[o] = int(c[0])
            buf[o + 1] = int(c[1])
            buf[o + 2] = int(c[2])
            buf[o + 3] = 255

    # box downsample
    out = bytearray(w * h * 4)
    for py in range(h):
        for px in range(w):
            r = g = b = 0
            for sy in range(SS):
                for sx in range(SS):
                    o = ((py * SS + sy) * W + (px * SS + sx)) * 4
                    r += buf[o]
                    g += buf[o + 1]
                    b += buf[o + 2]
            n = SS * SS
            o = (py * w + px) * 4
            out[o] = r // n
            out[o + 1] = g // n
            out[o + 2] = b // n
            out[o + 3] = 255
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))

    icon = render(512, 512, 256, 300, 44)
    write_png(os.path.join(here, "ModelingIcon.png"), 512, 512, icon)
    print("ModelingIcon.png")

    banner = render(1100, 512, 550, 330, 52)
    write_png(os.path.join(here, "ModelingBanner.png"), 1100, 512, banner)
    print("ModelingBanner.png")


if __name__ == "__main__":
    main()
