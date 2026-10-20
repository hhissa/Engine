#!/usr/bin/env python3
"""Generates style mode's fleck style icons into bin/assets/ui/flecks/.

style_<i>.png is fleck style i drawn in white -- the same shapes as
fleck_mask() in assets/shaders/Builtin.ChunkPointSplat.comp.glsl, so keep
the two in step. Pure Python: writes PNGs with zlib directly.
"""
import math
import os
import struct
import zlib

STYLES = ["Square", "Circle", "Triangle", "Hexagon", "Dots", "Scribble",
          "Veins", "Splat", "Straight", "Heavy", "Hatching", "Cubist",
          "Impressionist", "Expressive", "Baroque", "Streaky"]
SIZE = 128
SAMPLES = 3
SCALE = 0.82  # the shape's [-1,1] square, as a fraction of the icon


def fract(x):
    return x - math.floor(x)


def seg_dist(px, py, ax, ay, bx, by):
    pax, pay, bax, bay = px - ax, py - ay, bx - ax, by - ay
    h = max(0.0, min(1.0, (pax * bax + pay * bay) / (bax * bax + bay * bay)))
    return math.hypot(pax - bax * h, pay - bay * h)


def mask(style, x, y):
    r = math.hypot(x, y)
    if style == 0:
        return max(abs(x), abs(y)) <= 0.8
    if style == 1:
        return r <= 1.0
    if style == 2:
        return y >= -0.48 and abs(x) <= (0.95 - y) * 0.5734
    if style == 3:
        ax, ay = abs(x), abs(y)
        return max(ax * 0.866 + ay * 0.5, ay) <= 0.85
    if style == 4:
        dots = [(0, 0, 0.3), (0.6, 0.55, 0.27), (-0.6, 0.5, 0.27),
                (0.55, -0.6, 0.27), (-0.55, -0.6, 0.27)]
        return any(math.hypot(x - cx, y - cy) <= cr for cx, cy, cr in dots)
    if style == 5:
        return abs(x) <= 0.95 and abs(y - 0.4 * math.sin(x * 7.0)) <= 0.2
    if style == 6:
        return (seg_dist(x, y, 0, -0.95, 0, 0.95) <= 0.1 or
                seg_dist(x, y, 0, 0.1, 0.7, 0.7) <= 0.08 or
                seg_dist(x, y, 0, -0.3, -0.65, 0.25) <= 0.08)
    if style == 7:
        a = math.atan2(y, x)
        return r <= 0.66 + 0.18 * math.sin(5 * a + 0.7) + 0.12 * math.sin(9 * a + 2.1)
    if style == 8:
        return abs(y) <= 0.22 and abs(x) <= 0.95
    if style == 9:
        return seg_dist(x, y, -0.55, 0, 0.55, 0) <= 0.45
    if style == 10:
        return max(abs(x), abs(y)) <= 0.9 and fract((y + 1.0) * 2.0) < 0.35
    if style == 11:
        v = [(-0.9, -0.3), (0.2, -0.95), (0.85, 0.4), (-0.3, 0.85)]
        for i in range(4):
            (ax, ay), (bx, by) = v[i], v[(i + 1) % 4]
            if (bx - ax) * (y - ay) - (by - ay) * (x - ax) < 0:
                return False
        return True
    if style == 12:
        return x * x / 0.9025 + y * y / 0.2025 <= 1.0
    if style == 13:
        return abs(x) <= 0.95 and abs(y - (0.6 * x * x - 0.3)) <= 0.3 * (1 - abs(x)) + 0.03
    if style == 14:
        a = math.atan2(y, x) + math.pi
        for k in range(2):
            target = 0.2 + (a + 2 * math.pi * k) / (2 * math.pi) * 0.38
            if target < 0.97 and abs(r - target) < 0.1:
                return True
        return False
    if style == 15:
        return abs(x) <= 0.98 and (
            (abs(y - 0.1) <= 0.13 and fract(x * 2.5 + 0.3) < 0.72) or
            (abs(y + 0.45) <= 0.08 and fract(x * 1.7) < 0.6))
    return r <= 1.0


def write_png(path, rows):
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for a in row:
            raw += bytes((255, 255, 255, int(a * 255 + 0.5)))
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    out = os.path.join(root, "bin", "assets", "ui", "flecks")
    os.makedirs(out, exist_ok=True)
    half = SIZE / 2.0
    for i in range(len(STYLES)):
        rows = []
        for py in range(SIZE):
            row = []
            for px in range(SIZE):
                hits = 0
                for sy in range(SAMPLES):
                    for sx in range(SAMPLES):
                        x = (px + (sx + 0.5) / SAMPLES - half) / (half * SCALE)
                        y = (half - (py + (sy + 0.5) / SAMPLES)) / (half * SCALE)
                        hits += mask(i, x, y)
                row.append(hits / (SAMPLES * SAMPLES))
            rows.append(row)
        write_png(os.path.join(out, f"style_{i}.png"), rows)
    print("Fleck style icons written to", out)


if __name__ == "__main__":
    main()
