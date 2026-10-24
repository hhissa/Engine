#!/usr/bin/env python3
"""Writes face.stamps: a realistic head-and-shoulders bust with blue eyes,
for controller mode (`modelling_software_v2 --controller scenes/face.stamps`).

Built in centimetres in "face space" (x = the face's right on screen,
y = up, z = toward the viewer, origin about the middle of the skull) from
ordinary adult proportions, then scaled to scene units and turned to face
the default camera. Each part is one object, so the colours stay separate:
skin, eye whites, irises, pupils, lips and brows. Colours ride in "#coat"
lines (see ControllerMode::load_stamps()).

Run: python3 make_face.py [OUT]   (default: face.stamps next to this file)
"""
import math
import os
import sys

SCALE = 0.12                 # scene units per cm: a ~30 cm bust is ~3.6 units
ORIGIN = (0.0, -0.2, 3.2)    # where the face-space origin goes in the scene

SPHERE, BOX, ROUND_BOX, ELLIPSOID, CAPSULE = 0, 1, 6, 14, 12
UNION, SUBTRACT, INTERSECT = 2, 3, 4  # SculptBrushOp::Object*

stamps = []  # (object, shape, centre, params, smoothness, op, rotation)


def world(p):
    """Face space (y up, z toward the viewer) -> scene (y down, camera
    looking along +z): a half turn about X, then scale and place."""
    x, y, z = p
    return (ORIGIN[0] + x * SCALE, ORIGIN[1] - y * SCALE, ORIGIN[2] - z * SCALE)


def quat_y_to(d):
    """Quaternion (x, y, z, w) turning a shape's local +Y onto scene
    direction d."""
    n = math.sqrt(sum(c * c for c in d))
    d = tuple(c / n for c in d)
    dot = d[1]
    if dot > 0.999999:
        return (0.0, 0.0, 0.0, 1.0)
    if dot < -0.999999:
        return (1.0, 0.0, 0.0, 0.0)
    axis = (d[2], 0.0, -d[0])  # (0,1,0) x d
    an = math.sqrt(axis[0] ** 2 + axis[2] ** 2)
    half = math.acos(dot) / 2
    s = math.sin(half) / an
    return (axis[0] * s, 0.0, axis[2] * s, math.cos(half))


# The face-to-scene half turn about X, as a quaternion; ellipsoids keep
# their axes under it (radii are unsigned), so they need no rotation.
IDENTITY = (0.0, 0.0, 0.0, 1.0)


def add(obj, shape, centre, params, k=0.0, op=UNION, rot=IDENTITY):
    params = tuple(params) + (0.0,) * (4 - len(params))
    stamps.append((obj, shape, world(centre), tuple(p * SCALE for p in params),
                   k * SCALE, op, rot))


def ellipsoid(obj, c, r, k=0.0, op=UNION):
    add(obj, ELLIPSOID, c, r, k, op)


def sphere(obj, c, r, k=0.0, op=UNION):
    add(obj, SPHERE, c, (r,), k, op)


def capsule(obj, a, b, r, k=0.0, op=UNION):
    """A rod from a to b (face space)."""
    mid = tuple((p + q) / 2 for p, q in zip(a, b))
    length = math.dist(a, b)
    wa, wb = world(a), world(b)
    rot = quat_y_to(tuple(q - p for p, q in zip(wa, wb)))
    add(obj, CAPSULE, mid, (r, length / 2), k, op, rot)


def mirror(fn):
    """Calls fn(side) for the face's right (+1) and left (-1)."""
    for side in (1, -1):
        fn(side)


SKIN, WHITES, IRIS, PUPIL, LIPS, BROWS, HAIR = 1, 2, 3, 4, 5, 6, 7

# --- Head (skin) -------------------------------------------------------------
# Proportions: eyes about halfway down the head; brow, nose base and chin
# splitting the face into rough thirds.
EYE_X, EYE_Y, EYE_Z, EYE_R = 3.1, 1.0, 6.3, 1.2

# Skull and face masses, blended generously.
ellipsoid(SKIN, (0, 2.6, -1.0), (7.3, 8.6, 9.0))            # cranium
ellipsoid(SKIN, (0, 5.4, 5.0), (5.5, 3.6, 3.5), k=2.0)      # forehead
ellipsoid(SKIN, (0, -2.2, 3.0), (5.9, 6.6, 5.6), k=2.5)     # face mass
ellipsoid(SKIN, (0, 2.7, 7.3), (5.2, 1.3, 1.7), k=1.8)      # brow ridge
mirror(lambda s: ellipsoid(SKIN, (s * 4.4, -0.9, 5.9), (2.2, 1.5, 2.1), k=2.0))  # cheekbones
mirror(lambda s: ellipsoid(SKIN, (s * 3.5, -3.4, 5.6), (2.3, 2.1, 2.0), k=2.2))  # cheeks
mirror(lambda s: ellipsoid(SKIN, (s * 5.4, 3.2, 3.2), (1.7, 2.4, 2.5), k=2.0))   # temples
ellipsoid(SKIN, (0, -5.8, 2.4), (4.8, 3.0, 5.0), k=2.2)     # jaw
mirror(lambda s: sphere(SKIN, (s * 4.6, -6.3, -0.5), 1.6, k=2.0))                # jaw angles
ellipsoid(SKIN, (0, -8.0, 5.8), (2.5, 1.8, 2.2), k=1.6)     # chin
ellipsoid(SKIN, (0, -4.4, 7.0), (2.9, 1.9, 1.5), k=1.2)     # muzzle (round of the mouth)

# Neck and shoulders, then a flat cut for the base of the bust.
capsule(SKIN, (0, -5.5, -1.6), (0, -16.0, -1.2), 4.8, k=2.5)
mirror(lambda s: capsule(SKIN, (s * 2.4, -8.2, 1.6), (s * 0.8, -14.5, 3.8), 1.0, k=1.5))  # neck muscles
ellipsoid(SKIN, (0, -18.5, -1.5), (14.0, 4.0, 7.0), k=3.5)  # shoulders
add(SKIN, BOX, (0, -0.5, 0), (15.0, 18.5, 12.0), op=INTERSECT)  # the base cut (y = -19)

# Eye sockets (just the hollow in front of each eye), then the lids over
# the eyeballs (spheres a little bigger than the eye, shifted up / down, so
# their edges cross the eye just above and just below the iris).
mirror(lambda s: ellipsoid(SKIN, (s * EYE_X, EYE_Y + 0.15, 8.1), (1.6, 1.1, 0.9), k=0.6, op=SUBTRACT))
mirror(lambda s: sphere(SKIN, (s * EYE_X, EYE_Y + 0.9, EYE_Z), 1.3, k=0.35))       # upper lids
mirror(lambda s: sphere(SKIN, (s * EYE_X, EYE_Y - 1.0, EYE_Z - 0.05), 1.26, k=0.3))   # lower lids

# Nose: bridge, tip, wings, then the nostrils carved underneath.
capsule(SKIN, (0, 1.7, 8.3), (0, -1.9, 10.2), 0.85, k=1.0)
sphere(SKIN, (0, -2.35, 10.1), 1.05, k=0.8)
mirror(lambda s: sphere(SKIN, (s * 1.25, -2.85, 8.9), 0.85, k=0.6))
mirror(lambda s: ellipsoid(SKIN, (s * 0.6, -3.35, 9.5), (0.28, 0.14, 0.36), k=0.15, op=SUBTRACT))

# Ears: a shell, its hollow, and the lobe.
mirror(lambda s: ellipsoid(SKIN, (s * 7.1, 0.8, -0.6), (1.1, 3.0, 1.9), k=0.8))
mirror(lambda s: ellipsoid(SKIN, (s * 8.15, 1.0, -0.3), (0.4, 1.8, 1.0), k=0.6, op=SUBTRACT))
mirror(lambda s: sphere(SKIN, (s * 7.3, -2.0, -0.3), 0.75, k=0.6))

# Philtrum ridges between nose and lip.
mirror(lambda s: capsule(SKIN, (s * 0.35, -3.6, 9.0), (s * 0.42, -4.3, 8.7), 0.2, k=0.3))

# --- Eyes --------------------------------------------------------------------
mirror(lambda s: sphere(WHITES, (s * EYE_X, EYE_Y, EYE_Z), EYE_R))
# Iris: a shallow disc lying on the front of the eyeball (front at the
# cornea), pupil a thinner one just in front of it.
mirror(lambda s: ellipsoid(IRIS, (s * EYE_X, EYE_Y, EYE_Z + EYE_R - 0.23), (0.62, 0.62, 0.26)))
mirror(lambda s: ellipsoid(PUPIL, (s * EYE_X, EYE_Y, EYE_Z + EYE_R + 0.01), (0.27, 0.27, 0.07)))

# --- Lips ----------------------------------------------------------------------
ellipsoid(LIPS, (0, -4.75, 8.15), (2.1, 0.55, 0.75))          # upper lip
mirror(lambda s: ellipsoid(LIPS, (s * 0.6, -4.5, 8.25), (0.75, 0.4, 0.62), k=0.25))  # its bow
ellipsoid(LIPS, (0, -5.6, 7.95), (1.85, 0.68, 0.82), k=0.15)  # lower lip

# --- Brows ---------------------------------------------------------------------
def brow(s):
    # A chain of rods along the arch (low at the nose, peaking two thirds
    # out), thick at the inner end and tapering outward, set mostly into
    # the skin so only a thin layer shows.
    points = ((1.2, 2.5, 8.75), (2.2, 2.85, 8.7), (3.2, 3.05, 8.4),
              (4.1, 3.0, 7.9), (4.8, 2.75, 7.3))
    radii = (0.32, 0.29, 0.25, 0.2)
    for (a, b, r) in zip(points, points[1:], radii):
        capsule(BROWS, (s * a[0], a[1], a[2]), (s * b[0], b[1], b[2]), r, k=0.25)


mirror(brow)

# --- Hair -----------------------------------------------------------------------
# A cap a little over the skull, cut off below a sloping plane: high at the
# forehead, over the ears at the sides, lower at the back. Drawn as loose
# flecks (see LOOSENESS) for a short-cropped texture.
ellipsoid(HAIR, (0, 2.6, -1.0), (7.55, 8.85, 9.25))          # a thin layer over the skull
ellipsoid(HAIR, (0, 5.4, 5.0), (5.75, 3.85, 3.75), k=2.0)     # and over the forehead
HAIRLINE = 3.6     # cut height at the middle of the head
HAIR_SLOPE = 0.42  # rise per cm toward the front
n = (0.0, 1.0, -HAIR_SLOPE)
nl = math.sqrt(sum(c * c for c in n))
n = tuple(c / nl for c in n)
H = 25.0
centre = tuple(c + n_c * H for c, n_c in zip((0.0, HAIRLINE, 0.0), n))
add(HAIR, BOX, centre, (H, H, H), op=INTERSECT, rot=quat_y_to((n[0], -n[1], -n[2])))

COATS = {
    # object: colour (linear), roughness, metallic, waxy
    SKIN: ((0.50, 0.32, 0.24), 0.7, 0.0, 0.0),
    WHITES: ((0.75, 0.73, 0.70), 0.12, 0.0, 0.0),
    IRIS: ((0.10, 0.32, 0.78), 0.15, 0.0, 0.0),
    PUPIL: ((0.02, 0.02, 0.03), 0.1, 0.0, 0.0),
    LIPS: ((0.45, 0.19, 0.18), 0.4, 0.0, 1.0),
    BROWS: ((0.16, 0.10, 0.07), 0.85, 0.0, 0.0),
    HAIR: ((0.13, 0.08, 0.05), 0.8, 0.0, 0.0),
}
# Per-object looseness (style mode): hair as short, loose flecks.
LOOSENESS = {HAIR: 0.15}

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "face.stamps")
with open(out, "w") as f:
    f.write("# modelling_software_v2 sculpt stamps (v4/objects)\n")
    f.write("# x y z shape_type p0 p1 p2 p3 smoothness op material_slot "
            "looseness qx qy qz qw object_id\n")
    f.write("# Generated by make_face.py -- a bust with blue eyes.\n")
    for obj, (colour, rough, metal, waxy) in COATS.items():
        f.write("#coat %d %.3f %.3f %.3f %.3f %.3f %.3f\n" %
                (obj, *colour, rough, metal, waxy))
    for obj, shape, c, p, k, op, q in stamps:
        f.write("%.5f %.5f %.5f %d %.5f %.5f %.5f %.5f %.5f %d 0 %.3f %.6f %.6f %.6f %.6f %d\n"
                % (*c, shape, *p, k, op, LOOSENESS.get(obj, 0.0), *q, obj))
print("wrote %d stamps to %s" % (len(stamps), out))
