#!/usr/bin/env python3
"""Generates assets/textures/china_blue.png -- a blue-and-white china plate
pattern for the blades cutaway's broken plate (see generate_cutaways.py).

Usage: python3 tools/generate_china_texture.py bin/assets/textures

Radially symmetric about the texture's centre, so a plate maps it on with
uv_scale = its diameter and uv_offset putting its centre on (0.5, 0.5).
No dependencies -- the PNG is written by hand.
"""
import math
import os
import struct
import sys
import zlib

SIZE = 512
WHITE = (247, 247, 252)
COBALT = (32, 52, 138)


def blue_amount(u, v):
    """0 (white) .. 1 (cobalt) at texture point (u, v) in [0, 1)."""
    x, y = u - 0.5, v - 0.5
    r = math.hypot(x, y) * 2  # 0 at the centre, 1 at the plate's edge
    t = math.atan2(y, x)
    if r > 0.985:
        return 0.0
    # Rim: a solid band, then a ring of scallops inside it.
    if r > 0.9:
        return 1.0
    if r > 0.82:
        scallop = 0.86 + 0.035 * math.cos(16 * t)
        return 1.0 if r > scallop else 0.0
    if 0.79 < r < 0.805:
        return 1.0
    # Well: leafy scrolls between two thin rings.
    if 0.46 < r < 0.79:
        s = math.sin(10 * t + 14 * r) * math.cos(5 * t - 9 * r)
        leaf = math.sin(20 * t) * math.sin((r - 0.46) / 0.33 * math.pi)
        return 1.0 if s > 0.55 or leaf > 0.85 else 0.0
    if 0.43 < r < 0.445:
        return 1.0
    # Centre: an eight-petalled rosette around a dot.
    if r < 0.06:
        return 1.0
    petal = 0.36 * abs(math.cos(4 * t))
    if r < petal and r > petal - 0.05:
        return 1.0
    if r < petal * 0.55:
        return 0.6
    return 0.0


def main():
    out = sys.argv[1]
    rows = []
    for j in range(SIZE):
        row = bytearray([0])  # filter: none
        for i in range(SIZE):
            # 2x2 supersampled, so the pattern's edges aren't jagged.
            b = sum(blue_amount((i + dx) / SIZE, (j + dy) / SIZE)
                    for dx in (0.25, 0.75) for dy in (0.25, 0.75)) / 4
            row += bytes(round(w + (c - w) * b) for w, c in zip(WHITE, COBALT))
        rows.append(bytes(row))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) +
           chunk(b"IEND", b""))
    path = os.path.join(out, "china_blue.png")
    with open(path, "wb") as fh:
        fh.write(png)
    print(path)


main()
