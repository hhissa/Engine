#!/usr/bin/env python3
"""Generates coat mode's shaded artwork into bin/assets/ui/coat/.

Pure Python (no imaging libraries): writes PNGs with zlib directly.

  finish_<i>_base.png  a lit sphere for finish preset i, in greyscale. Drawn
                       tinted by the chosen colour, so the preview shows the
                       finish in that colour.
  finish_<i>_spec.png  that finish's highlight (white with alpha), drawn on
                       top -- tinted by the colour too for metals, which
                       reflect in their own colour.
  swatch.png           a white disc, tinted to show a colour.
  hue.png              a hue wheel (the Hue tool's icon).

The finish list and its roughness/metallic/waxy values must match
kCoatFinishes in src/controller_mode.cpp (same order).
"""
import math
import os
import struct
import zlib

FINISHES = [  # name, roughness, metallic, waxy -- same as the Qt mode's presets
    ("Default", 0.6, 0.0, False),
    ("Plastic", 0.35, 0.0, False),
    ("Shiny", 0.08, 0.0, False),
    ("Rough", 0.9, 0.0, False),
    ("Metal", 0.35, 1.0, False),
    ("Wax", 0.6, 0.0, True),
    ("Shiny Metal", 0.08, 1.0, False),
    ("Shiny Wax", 0.08, 0.0, True),
    ("Rough Metal", 0.9, 1.0, False),
    ("Rough Wax", 0.9, 0.0, True),
    ("Clay", 0.78, 0.0, True),
]

SIZE = 128
RADIUS = 58.0
SAMPLES = 3  # per axis, for anti-aliased edges


def write_png(path, pixels):
    """pixels: rows of (r, g, b, a) floats in 0..1."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)
        for r, g, b, a in row:
            raw += bytes(int(max(0.0, min(1.0, c)) * 255 + 0.5) for c in (r, g, b, a))
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def normalize(v):
    l = math.sqrt(sum(c * c for c in v))
    return tuple(c / l for c in v)


LIGHT = normalize((-0.55, 0.6, 0.65))  # upper left, toward the viewer
VIEW = (0.0, 0.0, 1.0)
HALF = normalize(tuple(l + v for l, v in zip(LIGHT, VIEW)))


def lobe(n_dot_h, roughness):
    """A highlight lobe tuned to read at button size: sharpness capped so
    even a mirror finish shows a visible spot."""
    alpha = max(roughness * roughness, 0.002)
    shininess = min(max(2.0 / (alpha * alpha) - 2.0, 2.0), 300.0)
    return max(n_dot_h, 0.0) ** shininess


def shade(nx, ny, nz, roughness, metallic, waxy):
    """Returns (diffuse grey, highlight) at a sphere normal."""
    n_dot_l = max(nx * LIGHT[0] + ny * LIGHT[1] + nz * LIGHT[2], 0.0)
    n_dot_h = nx * HALF[0] + ny * HALF[1] + nz * HALF[2]
    fresnel = (1.0 - max(nz, 0.0)) ** 4
    if metallic > 0.5:
        # Metal: little diffuse; a strong reflection (tinted when drawn)
        # plus a soft sheen so rough metal isn't black.
        diffuse = 0.16 + 0.3 * n_dot_l
        spec = lobe(n_dot_h, roughness) * (1.0 - 0.45 * roughness)
        spec += 0.35 * (0.3 + 0.7 * n_dot_l) * (0.5 + 0.5 * fresnel)
    else:
        diffuse = 0.28 + 0.72 * n_dot_l
        peak = 0.9 * (1.0 - roughness) ** 1.5 + 0.04
        spec = lobe(n_dot_h, roughness) * peak
        spec += 0.18 * fresnel * (1.0 - roughness)
    if waxy:
        spec += lobe(n_dot_h, 0.45) * 0.22  # the soft second lobe
    return diffuse, min(spec, 1.0)


def sphere_layers(roughness, metallic, waxy):
    base, spec = [], []
    c = SIZE / 2.0
    for y in range(SIZE):
        base_row, spec_row = [], []
        for x in range(SIZE):
            cover = d_sum = s_sum = 0.0
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    px = (x + (sx + 0.5) / SAMPLES - c) / RADIUS
                    py = (c - (y + (sy + 0.5) / SAMPLES)) / RADIUS
                    r2 = px * px + py * py
                    if r2 > 1.0:
                        continue
                    d, s = shade(px, py, math.sqrt(1.0 - r2), roughness, metallic, waxy)
                    cover += 1.0
                    d_sum += d
                    s_sum += s
            total = SAMPLES * SAMPLES
            if cover == 0.0:
                base_row.append((0.0, 0.0, 0.0, 0.0))
                spec_row.append((0.0, 0.0, 0.0, 0.0))
                continue
            d = d_sum / cover
            base_row.append((d, d, d, cover / total))
            spec_row.append((1.0, 1.0, 1.0, (s_sum / cover) * cover / total))
        base.append(base_row)
        spec.append(spec_row)
    return base, spec


def disc(colour_at):
    rows = []
    c = SIZE / 2.0
    for y in range(SIZE):
        row = []
        for x in range(SIZE):
            cover = 0.0
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    px = x + (sx + 0.5) / SAMPLES - c
                    py = y + (sy + 0.5) / SAMPLES - c
                    if px * px + py * py <= RADIUS * RADIUS:
                        cover += 1.0
            r, g, b = colour_at(x - c + 0.5, y - c + 0.5)
            row.append((r, g, b, cover / (SAMPLES * SAMPLES)))
        rows.append(row)
    return rows


def hsv_to_rgb(h, s, v):
    i = int(h * 6.0) % 6
    f = h * 6.0 - int(h * 6.0)
    p, q, t = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
    return [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i]


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    out = os.path.join(root, "bin", "assets", "ui", "coat")
    os.makedirs(out, exist_ok=True)
    for i, (_, roughness, metallic, waxy) in enumerate(FINISHES):
        base, spec = sphere_layers(roughness, metallic, waxy)
        write_png(os.path.join(out, f"finish_{i}_base.png"), base)
        write_png(os.path.join(out, f"finish_{i}_spec.png"), spec)
    write_png(os.path.join(out, "swatch.png"), disc(lambda x, y: (1.0, 1.0, 1.0)))

    def wheel(x, y):
        r = math.hypot(x, y) / RADIUS
        h = (math.atan2(-y, x) / (2 * math.pi)) % 1.0
        return hsv_to_rgb(h, min(1.0, 0.25 + r), 1.0)
    write_png(os.path.join(out, "hue.png"), disc(wheel))
    print("Coat artwork written to", out)


if __name__ == "__main__":
    main()
