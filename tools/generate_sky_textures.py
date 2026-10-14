#!/usr/bin/env python3
"""Generates chapter 4's sky boxes: sky_clearday.png (Theresa's "perfectly
blue sky", Diego's beach) and sky_overcast.png (Diego's hill).

Usage: python3 tools/generate_sky_textures.py bin/assets/textures

Same layout as the other sky_*.png: a 1024x512 equirectangular gradient,
row 0 the zenith, row 256 the horizon, the lower half the ground beyond
the set. No dependencies -- the PNG is written by hand.
"""
import os
import struct
import sys
import zlib

W, H = 1024, 512


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def ramp(stops, t):
    """Colour at t in [0, 1] along (position, colour) stops."""
    for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
        if t <= p1:
            return lerp(c0, c1, (t - p0) / (p1 - p0) if p1 > p0 else 0)
    return stops[-1][1]


SKIES = {
    # Deep blue overhead paling to a hazy white-blue at the horizon.
    "sky_clearday": [(0.0, (38, 96, 196)), (0.35, (70, 140, 222)),
                     (0.49, (178, 212, 240)), (0.5, (196, 222, 242)),
                     (0.52, (120, 140, 120)), (1.0, (60, 70, 55))],
    # Flat low grey, barely lighter at the horizon -- nothing to look at.
    "sky_overcast": [(0.0, (118, 122, 128)), (0.4, (148, 152, 157)),
                     (0.5, (172, 175, 178)), (0.52, (110, 112, 112)),
                     (1.0, (70, 72, 72))],
}


def write_png(path, rows):
    raw = b"".join(b"\x00" + bytes(row) for row in rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        fh.write(chunk(b"IEND", b""))


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, stops in SKIES.items():
        rows = []
        for y in range(H):
            colour = ramp(stops, y / (H - 1))
            rows.append(colour * W)
        path = os.path.join(out, name + ".png")
        write_png(path, rows)
        print(path)


main()
