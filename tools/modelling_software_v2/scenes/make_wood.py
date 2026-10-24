#!/usr/bin/env python3
"""Writes wood_plank.sculpt: a flat-sawn wooden plank whose wood texture is
all Spray Paint -- one rounded board, then paint strokes (SculptBrushOp::
ObjectPaint) for the grain.

The grain is the growth rings of a log, cut by the board's face: the log's
axis runs along the board but tilts slightly away from the face, so the
rings meet the face in nested arches ("cathedrals") that close toward one
end, with a knot the rings swirl round. Each ring is light earlywood
fading into a dark band of latewood. The face is worked through on a grid
the size of a voxel or so, and each row's run of one tone is painted as one
thin box reaching through the whole board (so the grain shows on the edges
too). A last pass of short fibres uses paint opacity: one stroke covers
only part of the surface (see paint_covers() in
Builtin.SculptChunkRecompute.comp.glsl; opacity rides in looseness as
stroke seed + (1 - opacity), 0 = opaque).

Run: python3 make_wood.py [OUT]
     (default: bin/assets/library/wood_plank.sculpt, so Search finds it)
"""
import math
import os
import random
import sys

BOX, ROUND_BOX = 1, 6
UNION, PAINT = 2, 5  # SculptBrushOp::ObjectUnion / ObjectPaint
OBJ = 1

# Material slots (remapped to fresh ones on load); BASE is the main look.
BASE, MID, DARK, KNOT, LIGHT = 1, 2, 3, 4, 5
LOOKS = {
    # slot: colour (linear), roughness
    BASE: ((0.33, 0.215, 0.12), 0.62),  # earlywood
    MID: ((0.235, 0.145, 0.07), 0.66),  # earlywood turning to latewood
    DARK: ((0.15, 0.085, 0.04), 0.7),   # latewood
    KNOT: ((0.075, 0.04, 0.02), 0.75),  # the knot's heart
    LIGHT: ((0.37, 0.25, 0.145), 0.6),  # paler earlywood, in broad patches
}

# The board: long along X, tall along Y (world +Y is down), thin in Z,
# its face toward the default camera (which looks along +Z from z = -3).
HX, HY, HZ = 1.0, 0.30, 0.06
EDGE = 0.012
DEPTH = HZ + 0.05      # paint boxes reach through the whole board
# Grid step: exactly one voxel (kCoarseCellSize / 16 = 1/64), on the voxel
# grid, so each paint run covers whole voxels -- off it, the pattern is
# quantised twice and comes out ragged.
CELL = 1.0 / 64.0

# The log: its axis at height AXIS_Y, behind the face by a depth that grows
# along the board, so the rings close in arches toward +X.
AXIS_Y = 0.03
def axis_depth(x):
    return 0.025 + 0.11 * (x + HX)

RING = 0.066           # growth ring spacing
KNOT_AT = (0.42, -0.12)
KNOT_R = 0.075
KNOT_RING = 0.024      # its own rings: a voxel and a half or more each

rng = random.Random(11)
stamps = []  # (centre, shape, params, op, slot, looseness, rotation)
IDENTITY = (0.0, 0.0, 0.0, 1.0)


def wobble(x, y):
    """Rings aren't perfect circles: slow waves along and across."""
    return (0.010 * math.sin(6.1 * x + 2.3 * y + 0.4) +
            0.006 * math.sin(14.3 * x - 4.1 * y + 2.0) +
            0.004 * math.sin(31.0 * x + 9.0 * y + 5.1))


def knot_distance(x, y):
    kx, ky = KNOT_AT
    return math.hypot((x - kx) / 1.35, y - ky)


def tone(x, y):
    """Which slot the face takes at (x, y), or None for the board's own."""
    d = knot_distance(x, y)
    if d < KNOT_R:  # the knot: its own tight rings, dark at the heart
        if d < 0.022:
            return KNOT
        return DARK if (d / KNOT_RING) % 1.0 > 0.5 else MID
    if d < KNOT_R + 0.016:
        return KNOT  # the dark rim round it
    # Distance from the log's axis, pushed out round the knot so the rings
    # swirl about it.
    r = math.hypot(y - AXIS_Y, axis_depth(x)) + wobble(x, y)
    r += 0.08 * math.exp(-(d / 0.14) ** 2)
    f = (r / RING) % 1.0
    if f > 0.82:
        return DARK
    if f > 0.66:
        return MID
    # Earlywood: paler in slow patches along the grain.
    figure = math.sin(2.3 * x + 1.1) * math.sin(9.0 * y + 0.8 * x + 0.3)
    if figure > 0.35:
        return LIGHT
    return None


def paint_box(x0, x1, y, slot, code=0.0, height=CELL):
    # A hair under the cell, so it covers its own voxels and no others.
    stamps.append((((x0 + x1) / 2, y, 0.0), BOX,
                   ((x1 - x0) / 2 - 0.001, height / 2 - 0.001, DEPTH, 0.0),
                   PAINT, slot, code, IDENTITY))


# The board itself.
stamps.append(((0.0, 0.0, 0.0), ROUND_BOX, (HX, HY, HZ, EDGE), UNION, BASE, 0.0,
               IDENTITY))

# The rings: row by row, each run of one tone as one box.
x_first = math.floor(-(HX + 0.02) / CELL)
nx = 2 * -x_first
y_first = math.floor(-(HY + 0.01) / CELL)
ny = 2 * -y_first
for j in range(ny):
    y = (y_first + j + 0.5) * CELL
    run_slot, run_start = None, 0.0
    for i in range(nx + 1):
        x = (x_first + i) * CELL
        slot = tone(x + 0.5 * CELL, y) if i < nx else None
        if slot != run_slot:
            if run_slot is not None:
                paint_box(run_start, x, y, run_slot)
            run_slot, run_start = slot, x

# Fibres: short dashes along the grain, half covering -- the open pores of
# the wood. One stroke each, so each gets its own speckle.
for k in range(150):
    x = round(rng.uniform(-HX, HX) / CELL) * CELL
    y = (round(rng.uniform(-HY, HY) / CELL) + 0.5) * CELL
    if knot_distance(x, y) < KNOT_R + 0.03:
        continue
    length = rng.randint(3, 8) * CELL
    paint_box(x, x + length, y, MID if rng.random() < 0.7 else DARK,
              code=(k + 1) + (1.0 - 0.5))

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "../../../bin/assets/library"))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "wood_plank.sculpt")
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "w") as f:
    f.write("# modelling_software_v2 sculpt stamps (v4/objects)\n")
    f.write("# x y z shape_type p0 p1 p2 p3 smoothness op material_slot "
            "looseness qx qy qz qw object_id\n")
    f.write("# Generated by make_wood.py -- a plank, its wood grain all Spray Paint.\n")
    for slot, (colour, rough) in LOOKS.items():  # BASE first: the main look
        f.write("#look %d %d %.3f %.3f %.3f %.3f 0 0 0\n" % (OBJ, slot, *colour, rough))
    for c, shape, p, op, slot, code, q in stamps:
        f.write("%.5f %.5f %.5f %d %.5f %.5f %.5f %.5f 0 %d %d %.4f %.6f %.6f %.6f %.6f %d\n"
                % (*c, shape, *p, op, slot, code, *q, OBJ))
print("wrote %d stamps to %s" % (len(stamps), out))
