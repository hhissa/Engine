#!/usr/bin/env python3
"""Generates sculpt mode's shape icons into bin/assets/ui/shapes/.

shape_<id>.png is brush shape <id> (SculptBrushShape's value), ray-marched
with the same distance functions as assets/shaders/Builtin.BrushShapes.inc.
glsl and the same proportions controller_mode.cpp's sculpt_params() gives
it, lit from the top left, in white on transparent. Pure Python (slow-ish:
about a minute); writes PNGs with zlib directly.
"""
import math
import os
import struct
import zlib

SIZE = 96
HALF_VIEW = 1.45  # half the icon's width, in shape units at the shape
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "..", "..", "bin", "assets", "ui", "shapes")


def length(*v):
    return math.sqrt(sum(c * c for c in v))


def clamp(x, lo, hi):
    return lo if x < lo else hi if x > hi else x


def sphere(p, r):
    return length(*p) - r


def box(p, b):
    q = [abs(p[i]) - b[i] for i in range(3)]
    return length(*[max(c, 0.0) for c in q]) + min(max(q), 0.0)


def round_box(p, b, r):
    return box(p, [c - r for c in b]) - r


def box_frame(p, b, e):
    pp = [abs(p[i]) - b[i] for i in range(3)]
    q = [abs(pp[i] + e) - e for i in range(3)]

    def part(a, b_, c):
        return length(max(a, 0), max(b_, 0), max(c, 0)) + min(max(a, b_, c), 0.0)

    return min(part(pp[0], q[1], q[2]), part(q[0], pp[1], q[2]),
               part(q[0], q[1], pp[2]))


def torus(p, R, r):
    return length(length(p[0], p[2]) - R, p[1]) - r


def capped_cylinder(p, r, h):
    dx = abs(length(p[0], p[2])) - r
    dy = abs(p[1]) - h
    return min(max(dx, dy), 0.0) + length(max(dx, 0.0), max(dy, 0.0))


def capped_cone(p, h, r1, r2):
    qx, qy = length(p[0], p[2]), p[1]
    k1 = (r2, h)
    k2 = (r2 - r1, 2.0 * h)
    ca = (qx - min(qx, r1 if qy < 0.0 else r2), abs(qy) - h)
    t = clamp(((k1[0] - qx) * k2[0] + (k1[1] - qy) * k2[1]) /
              (k2[0] ** 2 + k2[1] ** 2), 0.0, 1.0)
    cb = (qx - k1[0] + k2[0] * t, qy - k1[1] + k2[1] * t)
    s = -1.0 if (cb[0] < 0.0 and ca[1] < 0.0) else 1.0
    return s * math.sqrt(min(ca[0] ** 2 + ca[1] ** 2, cb[0] ** 2 + cb[1] ** 2))


def octahedron(p, s):
    q = [abs(c) for c in p]
    m = q[0] + q[1] + q[2] - s
    if 3 * q[0] < m:
        r = q
    elif 3 * q[1] < m:
        r = [q[1], q[2], q[0]]
    elif 3 * q[2] < m:
        r = [q[2], q[0], q[1]]
    else:
        return m * 0.57735027
    k = clamp(0.5 * (r[2] - r[1] + s), 0.0, s)
    return length(r[0], r[1] - s + k, r[2] - k)


def pyramid(p, h, base):
    b2 = base * base
    m2 = h * h + b2
    px, py, pz = abs(p[0]), p[1], abs(p[2])
    if pz > px:
        px, pz = pz, px
    px -= base
    pz -= base
    q = (pz, h * py - base * px, h * px + base * py)
    s = max(-q[0], 0.0)
    t = clamp((q[1] - base * pz) / (m2 + b2), 0.0, 1.0)
    a = m2 * (q[0] + s) ** 2 + q[1] ** 2
    bb = m2 * (q[0] + base * t) ** 2 + (q[1] - m2 * t) ** 2
    d2 = 0.0 if min(q[1], -q[0] * m2 - q[1] * base) > 0.0 else min(a, bb)
    sign = 1.0 if max(q[2], -py) >= 0.0 else -1.0
    return math.sqrt((d2 + q[2] ** 2) / m2) * sign


def hex_prism(p, r, h):
    kx, ky, kz = -0.8660254, 0.5, 0.57735
    qx, qy, qz = abs(p[0]), abs(p[1]), abs(p[2])
    d = 2.0 * min(kx * qx + ky * qz, 0.0)
    ax, az = qx - d * kx, qz - d * ky
    cx = clamp(ax, -kz * r, kz * r)
    dx = length(ax - cx, az - r) * (1.0 if az - r >= 0 else -1.0)
    dy = qy - h
    return min(max(dx, dy), 0.0) + length(max(dx, 0.0), max(dy, 0.0))


def round_cone(p, r1, r2, hh):
    y = p[1] + hh
    height = 2.0 * hh
    b = (r1 - r2) / height
    a = math.sqrt(max(1.0 - b * b, 0.0))
    qx, qy = length(p[0], p[2]), y
    k = -b * qx + a * qy
    if k < 0.0:
        return length(qx, qy) - r1
    if k > a * height:
        return length(qx, qy - height) - r2
    return qx * a + qy * b - r1


def capsule(p, r, h):
    y = p[1] - clamp(p[1], -h, h)
    return length(p[0], y, p[2]) - r


def link(p, le, r1, r2):
    qy = max(abs(p[1]) - le, 0.0)
    return length(length(p[0], qy) - r1, p[2]) - r2


def ellipsoid(p, r):
    k0 = length(*[p[i] / r[i] for i in range(3)])
    k1 = length(*[p[i] / (r[i] * r[i]) for i in range(3)])
    return k0 * (k0 - 1.0) / k1 if k1 > 0 else -min(r)


# id: (name, sdf) -- proportions match sculpt_params() at size 1. Plane (2)
# has no icon: sculpt mode doesn't offer it.
SHAPES = {
    0: ("sphere", lambda p: sphere(p, 1.0)),
    1: ("box", lambda p: box(p, (0.8, 0.8, 0.8))),
    3: ("torus", lambda p: torus(p, 0.75, 0.3)),
    4: ("cylinder", lambda p: capped_cylinder(p, 0.7, 0.9)),
    5: ("cone", lambda p: capped_cone(p, 0.9, 0.85, 0.2)),
    6: ("roundbox", lambda p: round_box(p, (0.8, 0.8, 0.8), 0.25)),
    7: ("frame", lambda p: box_frame(p, (0.8, 0.8, 0.8), 0.12)),
    8: ("octahedron", lambda p: octahedron(p, 1.2)),
    9: ("pyramid", lambda p: pyramid((p[0], p[1] + 0.6, p[2]), 1.3, 0.85)),
    10: ("hexprism", lambda p: hex_prism(p, 0.8, 0.75)),
    11: ("roundcone", lambda p: round_cone(p, 0.75, 0.3, 0.75)),
    12: ("capsule", lambda p: capsule(p, 0.55, 0.65)),
    13: ("link", lambda p: link(p, 0.45, 0.5, 0.18)),
    14: ("ellipsoid", lambda p: ellipsoid(p, (1.15, 0.7, 0.8))),
}


def normalize(v):
    n = length(*v)
    return [c / n for c in v]


def rot_y(p, a):
    c, s = math.cos(a), math.sin(a)
    return (c * p[0] + s * p[2], p[1], -s * p[0] + c * p[2])


def rot_x(p, a):
    c, s = math.cos(a), math.sin(a)
    return (p[0], c * p[1] - s * p[2], s * p[1] + c * p[2])


def render(sdf):
    # Camera on +Z looking at the origin, the shape turned to show three
    # faces. y is up in the icon.
    def scene(p):
        return sdf(rot_y(rot_x(p, 0.45), 0.6))

    eye = (0.0, 0.0, 6.0)
    light = normalize((-0.5, 0.8, 0.6))
    pixels = bytearray()
    pixel_world = 2.0 * HALF_VIEW / SIZE  # world size of a pixel at the origin
    for j in range(SIZE):
        pixels.append(0)  # PNG filter byte
        for i in range(SIZE):
            u = (i + 0.5) / SIZE * 2.0 - 1.0
            v = 1.0 - (j + 0.5) / SIZE * 2.0
            d = normalize((u * HALF_VIEW / 6.0, v * HALF_VIEW / 6.0, -1.0))
            t, closest, hit = 3.0, 1e9, False
            for _ in range(200):
                p = (eye[0] + d[0] * t, eye[1] + d[1] * t, eye[2] + d[2] * t)
                dist = scene(p)
                closest = min(closest, dist)
                if dist < 0.002:
                    hit = True
                    break
                t += dist * 0.6  # some SDFs (pyramid) are not exact
                if t > 9.0:
                    break
            if hit:
                e = 0.002
                n = normalize([scene((p[0] + (e if k == 0 else 0), p[1] + (e if k == 1 else 0),
                                      p[2] + (e if k == 2 else 0))) -
                               scene((p[0] - (e if k == 0 else 0), p[1] - (e if k == 1 else 0),
                                      p[2] - (e if k == 2 else 0)))
                               for k in range(3)])
                diffuse = max(sum(n[k] * light[k] for k in range(3)), 0.0)
                shade = 0.45 + 0.55 * diffuse
                grey = int(255 * clamp(shade, 0.0, 1.0))
                pixels += bytes((grey, grey, grey, 255))
            else:
                # Anti-aliased silhouette: partial cover within a pixel.
                cover = clamp(1.0 - closest / pixel_world, 0.0, 1.0)
                pixels += bytes((150, 150, 150, int(255 * cover * 0.9)))
    return bytes(pixels)


def write_png(path, raw):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    os.makedirs(OUT, exist_ok=True)
    for shape_id, (name, sdf) in SHAPES.items():
        write_png(os.path.join(OUT, f"shape_{shape_id}.png"), render(sdf))
        print(f"shape_{shape_id}.png ({name})")


if __name__ == "__main__":
    main()
