#!/usr/bin/env python3
"""Generates SH's cutaway .sdf scenes.

Usage: python3 tools/generate_cutaways.py bin/assets/scenes/cutaways

Re-running overwrites the .sdf files, so hand edits made in the sdf_editor
are lost -- either keep editing here, or stop using this once a scene has
been taken over in the editor. Camera stations and sky boxes live in
kCutaways (games/SH/src/game_scene_state.cpp).

Scenes are authored -Y up like every room: the floor's top is y = 0 and
anything above it has a negative y.
"""
import hashlib
import math
import os
import random
import sys

OUT = sys.argv[1]


# --- rotation helpers: glm::quat(vec3 euler) / glm::eulerAngles(), exactly ---
def quat_from_euler(e):
    cx, cy, cz = (math.cos(a / 2) for a in e)
    sx, sy, sz = (math.sin(a / 2) for a in e)
    return (cx * cy * cz + sx * sy * sz,  # w
            sx * cy * cz - cx * sy * sz,  # x
            cx * sy * cz + sx * cy * sz,  # y
            cx * cy * sz - sx * sy * cz)  # z


def euler_from_quat(q):
    w, x, y, z = q
    pitch = math.atan2(2 * (y * z + w * x), w * w - x * x - y * y + z * z)
    yaw = math.asin(max(-1.0, min(1.0, -2 * (x * z - w * y))))
    roll = math.atan2(2 * (x * y + w * z), w * w + x * x - y * y - z * z)
    return (pitch, yaw, roll)


def rotate(q, v):
    w, x, y, z = q
    # v' = q v q*
    ux, uy, uz = x, y, z
    vx, vy, vz = v
    dot_uv = ux * vx + uy * vy + uz * vz
    dot_uu = ux * ux + uy * uy + uz * uz
    cross = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
    return tuple(2 * dot_uv * u + (w * w - dot_uu) * vv + 2 * w * c
                 for u, vv, c in zip((ux, uy, uz), (vx, vy, vz), cross))


def norm(v):
    l = math.sqrt(sum(c * c for c in v))
    return tuple(c / l for c in v)


def euler_y_to(d):
    """Euler angles rotating local +Y onto direction d (verified)."""
    d = norm(d)
    a = (0.0, 1.0, 0.0)
    dot = sum(p * q for p, q in zip(a, d))
    if dot < -0.9999 and abs(d[0]) + abs(d[2]) > 1e-9:
        # Almost straight down: the half-way quaternion below is unstable
        # here, so build the frame directly instead.
        side = norm((d[1], -d[0], 0.0) if abs(d[0]) > abs(d[2]) else (0.0, d[2], -d[1]))
        return frame_euler(side, d)
    if dot < -0.9999:
        q = (0.0, 1.0, 0.0, 0.0)  # 180 degrees about X
    else:
        c = (a[1] * d[2] - a[2] * d[1], a[2] * d[0] - a[0] * d[2],
             a[0] * d[1] - a[1] * d[0])
        q = norm((1 + dot, *c))
    e = euler_from_quat(q)
    got = rotate(quat_from_euler(e), a)
    assert all(abs(g - t) < 1e-4 for g, t in zip(got, d)), (d, got)
    return e


def euler_x_to(d):
    """Euler angles rotating local +X onto d, keeping local +Y as near to
    world -Y (up) as possible -- for ellipsoids laid along a direction."""
    d = norm(d)
    up = (0.0, -1.0, 0.0)
    # Build an orthonormal frame: X = d, Y = up minus its d component.
    k = sum(p * q for p, q in zip(up, d))
    y = norm(tuple(u - k * c for u, c in zip(up, d)))
    z = (d[1] * y[2] - d[2] * y[1], d[2] * y[0] - d[0] * y[2],
         d[0] * y[1] - d[1] * y[0])
    m = [[d[0], y[0], z[0]], [d[1], y[1], z[1]], [d[2], y[2], z[2]]]
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1) * 2
        q = (0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s,
             (m[1][0] - m[0][1]) / s)
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2
        q = ((m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s,
             (m[0][2] + m[2][0]) / s)
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2
        q = ((m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s,
             (m[1][2] + m[2][1]) / s)
    else:
        s = math.sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2
        q = ((m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s,
             (m[1][2] + m[2][1]) / s, 0.25 * s)
    e = euler_from_quat(q)
    got = rotate(quat_from_euler(e), (1.0, 0.0, 0.0))
    assert all(abs(g - t) < 1e-4 for g, t in zip(got, d)), (d, got)
    return e


# --- scene writer ---------------------------------------------------------
def f(*v):
    return " ".join(f"{x:.4g}" if abs(x) > 1e-9 else "0" for x in v)


class Scene:
    def __init__(self, name, ambient, comment):
        self.name = name
        self.ambient = ambient
        self.comment = comment
        self.materials = {}
        self.layers = []
        self.lights = []
        self.volumetrics = []

    def material(self, key, colour, **props):
        mid = hashlib.sha1(f"{self.name}/{key}".encode()).hexdigest()[:16]
        lines = [f"    id={mid}", f"    base_colour={f(*colour, 1)}"]
        # Without a map the runtime samples its checkerboard -- see
        # MaterialDef::base_map.
        props.setdefault("base_map", "white")
        for k, v in props.items():
            lines.append(f"    {k}={v if isinstance(v, str) else f(*v) if isinstance(v, tuple) else f(v)}")
        self.materials[key] = (mid, lines)
        return key

    def layer(self, op="union", smooth=0.0):
        layer = {"op": op, "smooth": smooth, "prims": []}
        self.layers.append(layer)
        return layer

    def prim(self, layer, type_, pos, mat, rot=(0, 0, 0), params=None,
             extra=None, **more):
        p = {"type": type_, "pos": pos, "rot": rot, "params": params,
             "extra": extra, "mat": mat, "more": more}
        layer["prims"].append(p)

    def capsule_between(self, layer, a, b, r, mat, **more):
        d = tuple(q - p for p, q in zip(a, b))
        length = math.sqrt(sum(c * c for c in d))
        mid = tuple((p + q) / 2 for p, q in zip(a, b))
        self.prim(layer, "capsule", mid, mat, rot=euler_y_to(d),
                  params=(r, length / 2, 0, 0), **more)

    def light(self, type_, vec, colour, intensity):
        self.lights.append((type_, vec, colour, intensity))

    def volumetric(self, type_, pos, rot, params, density, mat):
        self.volumetrics.append((type_, pos, rot, params, density, mat))

    def write(self):
        out = ["#sdf scene file", "version=0.1",
               *(f"# {line}" for line in self.comment.splitlines()),
               f"ambient={f(self.ambient)}", ""]
        for key, (mid, lines) in self.materials.items():
            out += [f"material {key} {{", *lines, "}", ""]
        for i, (type_, vec, colour, intensity) in enumerate(self.lights):
            key = "direction" if type_ == "directional" else "position"
            out += [f"light light{i} {{", f"    type={type_}",
                    f"    {key}={f(*vec)}", f"    colour={f(*colour)}",
                    f"    intensity={f(intensity)}", "}", ""]
        n = 0
        for i, layer in enumerate(self.layers):
            out += [f"layer layer{i} {{", f"    operation={layer['op']}",
                    f"    smoothness={f(layer['smooth'])}", ""]
            for p in layer["prims"]:
                out += [f"    primitive p{n} {{", f"        type={p['type']}",
                        f"        position={f(*p['pos'])}",
                        f"        rotation={f(*p['rot'])}"]
                params = list(p["params"])
                while len(params) < 4:
                    params.append(0)
                if p["extra"] is not None:
                    params[3] = p["extra"]
                out.append(f"        params={f(*params)}")
                for k, v in p["more"].items():
                    out.append(f"        {k}={f(v) if not isinstance(v, str) else v}")
                out += [f"        material={self.materials[p['mat']][0]}",
                        "    }", ""]
                n += 1
            out += ["}", ""]
        for i, (type_, pos, rot, params, density, mat) in enumerate(self.volumetrics):
            out += [f"volumetric volumetric{i} {{", f"    type={type_}",
                    f"    position={f(*pos)}", f"    rotation={f(*rot)}",
                    f"    params={f(*params, 0)}", f"    density={f(density)}",
                    f"    material={self.materials[mat][0]}", "}", ""]
        path = os.path.join(OUT, f"{self.name}.sdf")
        with open(path, "w") as fh:
            fh.write("\n".join(out))
        print(path, sum(len(l["prims"]) for l in self.layers), "primitives")


def round_cone_between(s, layer, a, b, ra, rb, mat, **more):
    """A tapered limb from a (radius ra) to b (radius rb)."""
    d = tuple(q - p for p, q in zip(a, b))
    length = math.sqrt(sum(c * c for c in d))
    mid = tuple((p + q) / 2 for p, q in zip(a, b))
    s.prim(layer, "round_cone", mid, mat, rot=euler_y_to(d),
           params=(ra, rb, length / 2), **more)


def figure(s, x, z, height, face, lean, mat, smooth=None, wide=False, stoop=0.0,
           arms_up=False, arm_length=1.0):
    """A standing figure in a layer of its own, built like an artist's
    mannequin: head and jaw, neck, a chest/waist/pelvis that taper, and
    jointed arms and legs that narrow towards hands and feet. face = yaw it
    turns towards (forward is -z of that), lean = how far it tips sideways.
    wide gives it a full build (the shadow men keep a narrower one); stoop
    bends its upper body forward and down; arms_up raises its arms out to
    the sides; arm_length > 1 lets its arms hang too low."""
    H = height
    build = 1.0 if wide else 0.82
    l = s.layer(smooth=smooth if smooth is not None else 0.012 * H)
    cos, sin = math.cos(face), math.sin(face)

    def at(dx, yn, dz=0.0):
        """dx/dz across/forward of it and yn up it, as fractions of its
        height (yn = -1 is the top of its head, 0 its soles)."""
        bend = max(0.0, -yn * 2.4 - 1.0)  # stooping bends above the hips
        y = yn * H + stoop * 0.5 * bend * bend
        fwd = dz * H - stoop * bend * bend
        side = dx * H * build
        return (x + side * cos + fwd * sin + lean * -y * 0.05, y,
                z - side * sin + fwd * cos)

    def r(v):
        return v * H * build

    yaw = (0, face, 0)
    s.prim(l, "ellipsoid", at(0, -0.935), mat, rot=yaw, params=(r(0.052), 0.062 * H, r(0.058)))
    s.prim(l, "ellipsoid", at(0, -0.9, -0.012), mat, rot=yaw, params=(r(0.04), 0.03 * H, r(0.044)))
    round_cone_between(s, l, at(0, -0.84), at(0, -0.89), r(0.026), r(0.022), mat)
    s.capsule_between(l, at(-0.1, -0.815), at(0.1, -0.815), r(0.034), mat)
    s.prim(l, "ellipsoid", at(0, -0.745), mat, rot=yaw, params=(r(0.105), 0.085 * H, r(0.062)))
    s.prim(l, "ellipsoid", at(0, -0.635), mat, rot=yaw, params=(r(0.078), 0.07 * H, r(0.052)))
    s.prim(l, "ellipsoid", at(0, -0.535), mat, rot=yaw, params=(r(0.094), 0.06 * H, r(0.06)))
    for sgn in (-1, 1):
        shoulder = at(sgn * 0.118, -0.805)
        if arms_up:
            elbow = at(sgn * 0.25, -0.9, -0.01)
            wrist = at(sgn * 0.36, -1.0, -0.02)
        else:
            elbow = at(sgn * 0.138, -0.805 + 0.19 * arm_length, 0.005)
            wrist = at(sgn * 0.148, -0.805 + 0.36 * arm_length, -0.02)
        round_cone_between(s, l, shoulder, elbow, r(0.034), r(0.025), mat)
        round_cone_between(s, l, elbow, wrist, r(0.025), r(0.018), mat)
        d = norm(tuple(b - a for a, b in zip(elbow, wrist)))
        hand = tuple(w + 0.045 * H * c for w, c in zip(wrist, d))
        s.prim(l, "ellipsoid", hand, mat, rot=euler_y_to(d),
               params=(r(0.016), 0.045 * H, r(0.03)))
        hip = at(sgn * 0.052, -0.5)
        knee = at(sgn * 0.058, -0.27, -0.006)
        ankle = at(sgn * 0.06, -0.045)
        round_cone_between(s, l, hip, knee, r(0.056), r(0.036), mat)
        round_cone_between(s, l, knee, ankle, r(0.036), r(0.022), mat)
        s.prim(l, "ellipsoid", at(sgn * 0.062, -0.022, -0.035), mat, rot=yaw,
               params=(r(0.028), 0.022 * H, r(0.068)))
    return at


def hand(s, layer, wrist, along, across, up, size, mat, curl=0.3, spread=1.0):
    """A hand at wrist: palm along `along` (towards the fingertips),
    `across` from thumb side to little finger, `up` out of the back of the
    hand. Four two-jointed fingers and a thumb; curl bends them towards
    the palm. size is the palm's length."""
    # Square the three up: along is kept, up made perpendicular to it, and
    # across rebuilt from both on the side the caller asked for.
    along = norm(along)
    k = sum(x * y for x, y in zip(up, along))
    up = norm(tuple(u - k * a for u, a in zip(up, along)))
    side = (up[1] * along[2] - up[2] * along[1], up[2] * along[0] - up[0] * along[2],
            up[0] * along[1] - up[1] * along[0])
    if sum(x * y for x, y in zip(side, across)) < 0:
        side = tuple(-c for c in side)
    across = side
    def p(a, c, u):
        return tuple(w + a * x + c * y + u * z for w, x, y, z in zip(wrist, along, across, up))
    s.prim(layer, "round_box", p(0.5 * size, 0, 0), mat,
           rot=frame_euler(along, up), params=(0.5 * size, 0.16 * size, 0.42 * size),
           extra=0.12 * size)
    for k, (offset, length) in enumerate(((-0.3, 0.85), (-0.1, 0.95), (0.1, 0.9), (0.3, 0.72))):
        c = offset * size * spread
        base = p(size * 0.95, c, 0.02 * size)
        mid = p(size * (0.95 + 0.5 * length), c * 1.1, -0.14 * size * curl)
        tip = p(size * (0.95 + 0.85 * length) - 0.25 * size * curl, c * 1.15,
                -0.42 * size * curl)
        s.capsule_between(layer, base, mid, 0.105 * size, mat)
        s.capsule_between(layer, mid, tip, 0.095 * size, mat)
    thumb_base = p(0.25 * size, -0.42 * size, -0.05 * size)
    thumb_mid = p(0.6 * size, -0.62 * size * spread, -0.12 * size)
    thumb_tip = p(0.92 * size, -0.62 * size * spread, -0.22 * size - 0.2 * size * curl)
    s.capsule_between(layer, thumb_base, thumb_mid, 0.13 * size, mat)
    s.capsule_between(layer, thumb_mid, thumb_tip, 0.11 * size, mat)


def frame_euler(x_axis, y_axis):
    """Euler angles turning local +X onto x_axis and local +Y onto (the part
    of) y_axis (perpendicular to it)."""
    xa = norm(x_axis)
    k = sum(a * b for a, b in zip(y_axis, xa))
    ya = norm(tuple(u - k * c for u, c in zip(y_axis, xa)))
    za = (xa[1] * ya[2] - xa[2] * ya[1], xa[2] * ya[0] - xa[0] * ya[2],
          xa[0] * ya[1] - xa[1] * ya[0])
    m = [[xa[0], ya[0], za[0]], [xa[1], ya[1], za[1]], [xa[2], ya[2], za[2]]]
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        q4 = math.sqrt(tr + 1) * 2
        q = (0.25 * q4, (m[2][1] - m[1][2]) / q4, (m[0][2] - m[2][0]) / q4,
             (m[1][0] - m[0][1]) / q4)
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        q4 = math.sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2
        q = ((m[2][1] - m[1][2]) / q4, 0.25 * q4, (m[0][1] + m[1][0]) / q4,
             (m[0][2] + m[2][0]) / q4)
    elif m[1][1] > m[2][2]:
        q4 = math.sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2
        q = ((m[0][2] - m[2][0]) / q4, (m[0][1] + m[1][0]) / q4, 0.25 * q4,
             (m[1][2] + m[2][1]) / q4)
    else:
        q4 = math.sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2
        q = ((m[1][0] - m[0][1]) / q4, (m[0][2] + m[2][0]) / q4,
             (m[1][2] + m[2][1]) / q4, 0.25 * q4)
    e = euler_from_quat(q)
    got = rotate(quat_from_euler(e), (1.0, 0.0, 0.0))
    assert all(abs(g - t) < 1e-4 for g, t in zip(got, xa)), (xa, got)
    return e


def steel(s, key="steel", colour=(0.8, 0.81, 0.83), roughness=0.22):
    """Bare metal -- see MaterialDef::metallic. Needs something to reflect:
    a sky box, or lights placed to glint off it."""
    return s.material(key, colour, roughness=roughness, metallic=1)


def yawed(origin, yaw):
    """Maps a point in a frame turned yaw about y and placed at origin to
    world space -- the same turn as rotation=(0, yaw, 0) on a primitive."""
    q = quat_from_euler((0, yaw, 0))

    def at(local):
        r = rotate(q, local)
        return tuple(o + c for o, c in zip(origin, r))
    return at


def gable_house(s, walls, roofs, cx, z, w, tall, depth, mat, roof_mat=None):
    """A box house with a gable roof (ridge along z); its top y returned."""
    s.prim(walls, "box", (cx, -tall / 2, z), mat, params=(w / 2, tall / 2, depth / 2))
    side = w / 2 / math.sqrt(2) * 1.02
    # Flush with the walls front and back -- any overhang shows the lower
    # half of the turned box as a diamond on the gable end.
    s.prim(roofs, "box", (cx, -tall, z), roof_mat or mat, rot=(0, 0, math.pi / 4),
           params=(side, side, depth / 2 - 0.01))
    return tall + w / 2


def limited(cell, count):
    """Keyword arguments repeating a primitive (see SdfRepetitionMode)."""
    return {"repetition": "limited", "repetition_cell": f(*cell),
            "repetition_count": f(*count)}


# --- ramen ---------------------------------------------------------------
def ramen():
    s = Scene("ramen", 0.35, "Theo's Indomie -- a bowl of noodles over a white sky box.")
    bowl = s.material("bowl", (0.92, 0.9, 0.86), roughness=0.3)
    band = s.material("band", (0.62, 0.12, 0.1), roughness=0.3)
    broth = s.material("broth", (0.72, 0.42, 0.14), roughness=0.2)
    noodle = s.material("noodle", (0.97, 0.82, 0.45), roughness=0.6)
    lime = s.material("lime", (0.45, 0.72, 0.18), roughness=0.5)
    pith = s.material("pith", (0.85, 0.92, 0.6), roughness=0.5)
    wood = s.material("chopsticks", (0.8, 0.62, 0.4), base_map="lightwood", uv_scale=0.4)
    table = s.material("table", (1, 1, 1), base_map="lightwoodveneer", uv_scale=1.5)

    # Table top under everything -- its top face at y = 1.0, under the foot
    # -- wide enough that the camera never sees past its edges.
    t = s.layer()
    s.prim(t, "box", (0, 1.06, 0), table, params=(12, 0.06, 12))
    # Bowl: a sphere shell, its top third cut off, on a foot ring.
    b = s.layer(smooth=0.03)
    s.prim(b, "sphere", (0, 0, 0), bowl, params=(1.0,))
    s.prim(b, "capped_cylinder", (0, 0.95, 0), bowl, params=(0.42, 0.05))
    cut = s.layer("subtraction", 0.02)
    s.prim(cut, "box", (0, -1.2, 0), bowl, params=(1.3, 1.05, 1.3))
    s.prim(cut, "sphere", (0, 0, 0), bowl, params=(0.92,))
    # A red band round the outside, just under the rim.
    r = s.layer()
    s.prim(r, "torus", (0, -0.05, 0), band, params=(0.985, 0.025))
    # Shallow broth -- "just enough water to cover the top".
    w = s.layer()
    s.prim(w, "capped_cylinder", (0, 0.08, 0), broth, params=(0.91, 0.04))
    # The crushed noodle brick: a mound of wavy loops.
    n = s.layer(smooth=0.01)
    rng = random.Random(7)
    for i in range(16):
        ang = rng.uniform(0, math.tau)
        rad = rng.uniform(0.0, 0.45)
        pos = (rad * math.cos(ang), rng.uniform(-0.12, 0.0), rad * math.sin(ang))
        rot = (rng.uniform(-0.5, 0.5), rng.uniform(0, math.tau), rng.uniform(-0.5, 0.5))
        s.prim(n, "torus", pos, noodle, rot=rot,
               params=(rng.uniform(0.14, 0.3), 0.035))
    # Half a lime floating in the broth, cut face up. (No subtraction --
    # a subtraction layer would cut into the bowl and noodles too.)
    lw = s.layer()
    # Its rind is under the broth; the cut face and a ring of rind show.
    s.prim(lw, "capped_cylinder", (0.5, 0.03, -0.42), pith, params=(0.17, 0.018))
    s.prim(lw, "torus", (0.5, 0.03, -0.42), lime, params=(0.175, 0.022))
    # Chopsticks resting across the bowl.
    c = s.layer()
    s.capsule_between(c, (-1.25, -0.15, 0.55), (1.15, -0.2, 0.15), 0.035, wood)
    s.capsule_between(c, (-1.25, -0.15, 0.68), (1.15, -0.2, 0.33), 0.035, wood)

    s.light("directional", (-0.4, -1, -0.6), (1, 0.97, 0.9), 0.9)
    s.light("point", (-1.2, -2.2, -1.5), (1, 0.95, 0.85), 1.2)
    # A soft-boiled egg, halved, yolk up.
    white = s.material("egg_white", (0.97, 0.96, 0.92), roughness=0.3)
    yolk = s.material("yolk", (0.98, 0.6, 0.12), roughness=0.2)
    e = s.layer(smooth=0.02)
    s.prim(e, "ellipsoid", (-0.36, -0.01, 0.3), white, rot=(0, 0.5, 0), params=(0.21, 0.07, 0.15))
    ey = s.layer()
    s.prim(ey, "ellipsoid", (-0.36, -0.06, 0.3), yolk, params=(0.09, 0.035, 0.08))
    # Sliced spring onion and chilli flakes over the noodles.
    green = s.material("spring_onion", (0.35, 0.68, 0.22), roughness=0.5)
    chilli = s.material("chilli", (0.75, 0.12, 0.05), roughness=0.6)
    sc = s.layer()
    rng = random.Random(17)
    for _ in range(9):
        a, d = rng.uniform(0, math.tau), rng.uniform(0.05, 0.6)
        s.prim(sc, "torus", (d * math.cos(a), rng.uniform(-0.16, -0.1), d * math.sin(a)), green,
               rot=(rng.uniform(-0.4, 0.4), 0, rng.uniform(-0.4, 0.4)), params=(0.04, 0.013))
    for _ in range(14):
        a, d = rng.uniform(0, math.tau), rng.uniform(0.05, 0.75)
        s.prim(sc, "box", (d * math.cos(a), rng.uniform(-0.15, -0.02), d * math.sin(a)), chilli,
               rot=(0, rng.uniform(0, 3), 0), params=(0.018, 0.012, 0.012))
    # The torn Indomie packet and its foil seasoning sachet on the table.
    wrap = s.material("packet", (0.92, 0.32, 0.12), roughness=0.4)
    stripe = s.material("packet_stripe", (0.98, 0.85, 0.2), roughness=0.4)
    foil = s.material("sachet", (0.85, 0.85, 0.88), roughness=0.3, metallic=1)
    pk = s.layer(smooth=0.01)
    at = yawed((2.0, 0, -0.75), 0.45)
    s.prim(pk, "round_box", at((0, 0.985, 0)), wrap, rot=(0, 0.45, 0),
           params=(0.55, 0.015, 0.38), extra=0.01)
    s.prim(pk, "box", at((0, 0.968, -0.12)), stripe, rot=(0, 0.45, 0), params=(0.5, 0.004, 0.06))
    s.prim(pk, "round_box", at((0.85, 0.99, 0.35)), foil, rot=(0, 0.8, 0),
           params=(0.13, 0.008, 0.09), extra=0.006)
    s.write()


# --- bluehour ------------------------------------------------------------
def bluehour():
    s = Scene("bluehour", 0.12,
              "Mel's blue hour -- a row of house silhouettes against a blue sky.")
    dark = s.material("silhouette", (0.035, 0.045, 0.09), roughness=1)
    ground = s.material("ground", (0.02, 0.025, 0.05), roughness=1)
    g = s.layer()
    # Out past the frame on every side, the camera included -- seeing under
    # its edge shows the sky box below the horizon.
    s.prim(g, "box", (0, 0.1, 14), ground, params=(60, 0.1, 30))
    h = s.layer()
    roofs = s.layer()
    rng = random.Random(3)
    aerial_rng = random.Random(31)
    # Wider than the frame, so the row runs off both edges.
    x = -26.0
    while x < 26.0:
        w = rng.uniform(1.6, 2.8)
        tall = rng.uniform(1.6, 3.2)
        depth = rng.uniform(1.5, 2.2)
        z = 9 + rng.uniform(-0.6, 0.6)
        cx = x + w / 2
        s.prim(h, "box", (cx, -tall / 2, z), dark, params=(w / 2, tall / 2, depth / 2))
        style = rng.random()
        if style < 0.6:
            # Gable roof: a box rotated 45 degrees about the ridge (z) axis.
            side = w / 2 / math.sqrt(2) * 1.02
            s.prim(roofs, "box", (cx, -tall, z), dark, rot=(0, 0, math.pi / 4),
                   params=(side, side, depth / 2 + 0.05))
            top = tall + w / 2
        else:
            top = tall
        if rng.random() < 0.6:
            chx = cx + rng.uniform(-w / 3, w / 3)
            s.prim(roofs, "box", (chx, -(top - 0.1), z), dark,
                   params=(0.12, 0.45, 0.12))
        # TV aerials on some roofs (their own rng, so the row of houses
        # comes out exactly as it always has).
        if aerial_rng.random() < 0.35:
            ax = cx + aerial_rng.uniform(-w / 4, w / 4)
            s.capsule_between(roofs, (ax, -top + 0.05, z), (ax, -top - 0.9, z), 0.03, dark)
            for k in range(3):
                s.capsule_between(roofs, (ax - 0.35 + 0.08 * k, -top - 0.6 - 0.12 * k, z),
                                  (ax + 0.35 - 0.08 * k, -top - 0.6 - 0.12 * k, z), 0.02, dark)
        x += w + rng.uniform(0.0, 0.5)
    # Trees between some houses, and a telephone pole with sagging wires.
    t = s.layer(smooth=0.15)
    for tx, tz, size in [(-19, 7.3, 1.4), (-9.5, 7.2, 1.3), (3.8, 7.4, 1.6),
                         (11, 7, 1.1), (19.5, 7.2, 1.5)]:
        s.capsule_between(t, (tx, 0, tz), (tx, -2.2 * size, tz), 0.09, dark)
        # A small canopy -- a third of the size it started at, which read
        # as smoke plumes -- sat on top of the trunk.
        c = size / 3
        top = -2.2 * size - 0.45 * c
        for k in range(5):
            a = k / 5 * math.tau
            s.prim(t, "sphere", (tx + 0.55 * c * math.cos(a), top + 0.3 * c * math.sin(a * 2),
                                 tz + 0.4 * c * math.sin(a)), dark, params=(0.65 * c,))
    p = s.layer()
    for px in (-5.5, 6.5):
        s.capsule_between(p, (px, 0, 6), (px, -5.2, 6), 0.08, dark)
        s.capsule_between(p, (px - 0.7, -4.8, 6), (px + 0.7, -4.8, 6), 0.05, dark)
    # Two wires sagging between the poles, as short capsules along a curve.
    for off in (-0.55, 0.55):
        pts = []
        for i in range(13):
            u = i / 12
            xx = -5.5 + off + (12 + 0 * off) * u
            yy = -4.75 + 0.9 * 4 * u * (1 - u)
            pts.append((xx, yy, 6))
        for a, b in zip(pts, pts[1:]):
            s.capsule_between(p, a, b, 0.04, dark)

    # Faint light from the sky behind the houses, none from the front.
    s.light("directional", (0, -0.3, 1), (0.5, 0.6, 1.0), 0.25)
    # Insulators on the poles' crossbars, and a few birds on the wires.
    for px in (-5.5, 6.5):
        for dx in (-0.55, 0.55):
            s.capsule_between(p, (px + dx, -4.85, 6), (px + dx, -5.0, 6), 0.045, dark)
    for u, size in ((0.22, 1.0), (0.27, 0.9), (0.64, 1.1), (0.71, 0.95)):
        bx = -5.5 - 0.55 + 12 * u
        by = -4.75 + 0.9 * 4 * u * (1 - u) - 0.1 * size
        s.prim(p, "ellipsoid", (bx, by, 6), dark, rot=(0, 0, 0.3), params=(0.13 * size, 0.08 * size, 0.07 * size))
        s.prim(p, "sphere", (bx + 0.09 * size, by - 0.08 * size, 6), dark, params=(0.05 * size,))
        s.capsule_between(p, (bx - 0.1 * size, by + 0.02, 6), (bx - 0.24 * size, by + 0.08, 6), 0.025, dark)
    # A picket fence along the front gardens.
    fn = s.layer()
    s.prim(fn, "box", (0, -0.45, 7.4), dark, params=(0.04, 0.45, 0.04),
           **limited((0.32, 0, 0), (120, 1, 1)))
    s.prim(fn, "box", (0, -0.62, 7.4), dark, params=(19.2, 0.035, 0.03))
    s.prim(fn, "box", (0, -0.28, 7.4), dark, params=(19.2, 0.035, 0.03))
    # One streetlamp somebody forgot about, still on.
    lamp = s.material("streetlamp", (1, 0.75, 0.4), emissive_colour=(1, 0.7, 0.35),
                      emissive_intensity=1.2, casts_shadow=0)
    lp = s.layer()
    s.capsule_between(lp, (1.5, 0, 6.8), (1.5, -3.3, 6.8), 0.05, dark)
    s.capsule_between(lp, (1.5, -3.3, 6.8), (2.1, -3.45, 6.8), 0.04, dark)
    s.prim(lp, "ellipsoid", (2.15, -3.4, 6.8), lamp, params=(0.14, 0.06, 0.1))
    s.light("point", (2.15, -3.1, 6.6), (1, 0.7, 0.4), 0.35)
    s.write()


# --- moon ----------------------------------------------------------------
def moon():
    s = Scene("moon", 0.04,
              "Pat's recurring dream -- the lunar surface, and a well.")
    regolith = s.material("regolith", (0.6, 0.6, 0.58), uv_scale=3,
                          roughness=1, bump_map="concrete_wall", bump_strength=3)
    stone = s.material("well_stone", (0.95, 0.93, 0.88), base_map="cobblestone",
                       uv_scale=0.6, roughness=1)
    void = s.material("void", (0, 0, 0), roughness=1)
    wood = s.material("well_wood", (0.55, 0.45, 0.35), base_map="woodpanel",
                      uv_scale=0.8, roughness=1)
    g = s.layer(smooth=0.3)
    s.prim(g, "box", (0, 0.5, 6), regolith, params=(16, 0.5, 14),
           displace_amplitude=0.08, displace_frequency=1.3)
    # Crater rims, then the craters themselves.
    craters = [(-4.5, 7, 1.6), (3.5, 10, 2.4), (5.5, 3, 1.0), (-2.5, 13, 3.0),
               (-6.5, 2.5, 0.8), (1.6, 4.6, 0.55)]
    for cx, cz, r in craters:
        s.prim(g, "torus", (cx, 0.05 * r, cz), regolith, params=(r, 0.18 * r))
    c = s.layer("subtraction", 0.25)
    for cx, cz, r in craters:
        s.prim(c, "ellipsoid", (cx, 0.15 * r, cz), regolith, params=(r, 0.45 * r, r))
    # Rocks scattered about.
    rk = s.layer()
    rng = random.Random(11)
    for _ in range(18):
        x, z = rng.uniform(-8, 8), rng.uniform(1, 14)
        rr = rng.uniform(0.05, 0.22)
        s.prim(rk, "ellipsoid", (x, -rr * 0.3, z), regolith,
               rot=(rng.uniform(0, 3), rng.uniform(0, 3), 0),
               params=(rr * 1.3, rr * 0.8, rr))
    # The well: a round stone wall, hollow, over a hole with no bottom.
    w = s.layer(smooth=0.02)
    s.prim(w, "capped_cylinder", (0, -0.35, 1.5), stone, params=(0.75, 0.35))
    hole = s.layer("subtraction", 0.02)
    s.prim(hole, "capped_cylinder", (0, 0.0, 1.5), stone, params=(0.58, 1.5))
    rim = s.layer()
    s.prim(rim, "torus", (0, -0.7, 1.5), stone, params=(0.665, 0.1))
    # Something to read as depth at the bottom of the shaft.
    bottom = s.layer()
    s.prim(bottom, "capped_cylinder", (0, 1.2, 1.5), void, params=(0.6, 0.1))
    # A little gabled roof on two posts, with a crossbeam, so it reads as a
    # well. The roof is a box turned 45 degrees with its lower half cut
    # away -- built before the posts so that cut can't reach them.
    roof = s.layer()
    s.prim(roof, "box", (0, -1.9, 1.5), wood, rot=(0, 0, math.pi / 4),
           params=(0.5, 0.5, 0.6))
    roof_cut = s.layer("subtraction")
    s.prim(roof_cut, "box", (0, -1.4, 1.5), wood, params=(1.2, 0.5, 1.0))
    frame = s.layer()
    for x in (-0.72, 0.72):
        s.capsule_between(frame, (x, -0.6, 1.5), (x, -1.9, 1.5), 0.06, wood)
    s.capsule_between(frame, (-0.85, -1.6, 1.5), (0.85, -1.6, 1.5), 0.045, wood)

    # Harsh, low sun from the side; no sky light at all.
    s.light("directional", (-0.6, -0.35, -1), (1, 1, 0.97), 1.3)
    # A crank on the crossbeam, its rope down the shaft to a bucket.
    rope = s.material("rope", (0.7, 0.6, 0.42), roughness=1)
    iron = s.material("bucket", (0.45, 0.45, 0.47), roughness=0.35, metallic=1)
    s.capsule_between(frame, (0.85, -1.6, 1.5), (1.05, -1.6, 1.5), 0.03, wood)
    s.capsule_between(frame, (1.05, -1.6, 1.5), (1.05, -1.35, 1.5), 0.025, wood)
    rp = s.layer()
    s.prim(rp, "capped_cylinder", (0, -1.6, 1.5), rope, rot=(0, 0, math.pi / 2), params=(0.08, 0.25))
    s.capsule_between(rp, (0, -1.55, 1.42), (0, -0.55, 1.42), 0.012, rope)
    s.prim(rp, "capped_cone", (0, -0.42, 1.42), iron, params=(0.12, 0.11, 0.14))
    s.prim(rp, "torus", (0, -0.55, 1.42), iron, rot=(math.pi / 2, 0, 0), params=(0.12, 0.01))
    # The Earth, low over the horizon.
    earth = s.material("earth", (0.25, 0.45, 0.9), emissive_colour=(0.3, 0.5, 0.95),
                       emissive_intensity=0.5, casts_shadow=0)
    land = s.material("earth_land", (0.35, 0.55, 0.3), emissive_colour=(0.35, 0.5, 0.3),
                      emissive_intensity=0.4, casts_shadow=0)
    cloud = s.material("earth_cloud", (1, 1, 1), emissive_colour=(1, 1, 1),
                       emissive_intensity=0.6, casts_shadow=0)
    ea = s.layer()
    ex, ey, ez = 22, -16, 90
    s.prim(ea, "sphere", (ex, ey, ez), earth, params=(5.0,))
    for dx, dy, rx, ry in ((-1.5, -1, 1.6, 1.2), (1.2, 1.4, 1.2, 1.6), (0.5, -2.8, 1.0, 0.6)):
        s.prim(ea, "ellipsoid", (ex + dx, ey + dy, ez - 4.4), land, params=(rx, ry, 0.9))
    for dx, dy in ((0.0, 0.2), (-2.2, 1.8), (2.4, -1.5)):
        s.prim(ea, "ellipsoid", (ex + dx, ey + dy, ez - 4.75), cloud, params=(1.4, 0.35, 0.4))
    s.write()


# --- glowingfetalposition ------------------------------------------------
def fetal():
    s = Scene("glowingfetalposition", 0.03,
              "Mel's depressive episodes -- a glowing figure curled on a black plane,\n"
              "god rays falling on it.")
    floor = s.material("floor", (0.02, 0.02, 0.02), roughness=0.15)
    glow = s.material("figure", (1, 0.96, 0.85), roughness=1,
                      emissive_colour=(1, 0.93, 0.75), emissive_intensity=0.3)
    ray = s.material("ray", (1, 0.95, 0.8))
    fl = s.layer()
    s.prim(fl, "box", (0, 0.05, 0), floor, params=(12, 0.05, 12))

    # Lying on her right side along x, head at +x and tucked down towards
    # her knees, which are drawn up to her chest on the -z side (the
    # camera's). Everything rests on y = 0.
    b = s.layer(smooth=0.035)
    s.prim(b, "sphere", (0.4, -0.13, -0.11), glow, params=(0.12,))  # head
    s.capsule_between(b, (0.34, -0.15, -0.04), (0.28, -0.16, 0.01), 0.06, glow)  # neck
    s.prim(b, "ellipsoid", (0.1, -0.16, 0.06), glow,
           rot=euler_x_to((1, 0.0, -0.25)), params=(0.25, 0.14, 0.11))  # torso
    s.prim(b, "ellipsoid", (-0.17, -0.14, 0.04), glow, params=(0.13, 0.14, 0.14))  # hips
    # Legs: thighs drawn up to the chest, shins folded back under them.
    for y, r in ((-0.2, 0.065), (-0.08, 0.065)):
        knee = (0.12, y, -0.33)
        s.capsule_between(b, (-0.16, y, 0.0), knee, r, glow)
        s.capsule_between(b, knee, (-0.22, y + 0.005, -0.27), r * 0.75, glow)
    # Arms wrapped round the shins, hands meeting past them.
    for y in (-0.26, -0.13):
        elbow = (0.2, y, -0.22)
        s.capsule_between(b, (0.27, y + 0.03, 0.02), elbow, 0.042, glow)
        s.capsule_between(b, elbow, (-0.02, y + 0.02, -0.42), 0.035, glow)

    # God rays: long, thin, tilted shafts from far above, crossing at her.
    for x, z, tilt, width in ((0.0, 0.0, 0.22, 0.45), (0.5, 0.4, 0.3, 0.2),
                              (-0.6, -0.3, 0.18, 0.25), (1.0, -0.6, 0.26, 0.15)):
        s.volumetric("box", (x - 4 * math.sin(tilt), -4, z), (0, 0, -tilt),
                     (width, 4.2, width * 0.8), 0.18, ray)
    s.light("directional", (0.25, -1, 0), (1, 0.95, 0.8), 0.5)
    # Hair spilling back from her head, hands and bare feet.
    hair = s.material("hair", (0.9, 0.85, 0.72), roughness=1,
                      emissive_colour=(1, 0.9, 0.7), emissive_intensity=0.2)
    hr = s.layer(smooth=0.03)
    s.prim(hr, "ellipsoid", (0.43, -0.15, -0.05), hair, rot=(0, 0.4, 0.3), params=(0.13, 0.11, 0.15))
    s.prim(hr, "ellipsoid", (0.5, -0.08, 0.04), hair, rot=(0, 0.3, 0), params=(0.12, 0.05, 0.16))
    for y in (-0.26, -0.13):
        s.prim(b, "ellipsoid", (-0.06, y + 0.02, -0.45), glow, rot=(0, 0.5, 0), params=(0.035, 0.02, 0.06))
    for y in (-0.2, -0.08):
        s.prim(b, "ellipsoid", (-0.27, y + 0.01, -0.27), glow, rot=(0, -0.3, 0), params=(0.05, 0.03, 0.025))
    # Dust turning in the light.
    mote = s.material("dust", (1, 0.95, 0.8), emissive_colour=(1, 0.95, 0.8),
                      emissive_intensity=1.5, casts_shadow=0)
    dm = s.layer()
    rng = random.Random(13)
    for _ in range(26):
        s.prim(dm, "sphere", (rng.uniform(-0.9, 0.9), rng.uniform(-1.8, -0.3), rng.uniform(-0.6, 0.6)),
               mote, params=(rng.uniform(0.006, 0.012),))
    s.write()


# --- shadowmen -----------------------------------------------------------
def shadowmen():
    s = Scene("shadowmen", 0.75,
              "Mel's first psychotic episode -- shadow men standing about a white plane.")
    white = s.material("white", (0.97, 0.97, 0.96), roughness=1)
    shadow = s.material("shadow", (0.0, 0.0, 0.0), roughness=1)
    fl = s.layer()
    s.prim(fl, "box", (0, 0.05, 8), white, params=(30, 0.05, 30))

    def man(x, z, height, face, lean):
        figure(s, x, z, height, face, lean, shadow, arm_length=1.3)

    # One straight ahead, the rest at the edges -- "in the corner of my eye".
    man(0.0, 9.0, 2.5, 0.0, 0.0)
    man(-4.2, 6.5, 2.7, 0.5, 0.4)
    man(4.6, 5.0, 2.4, -0.6, -0.3)
    man(1.9, 6.0, 2.6, -0.3, 0.0)
    man(-2.2, 11.0, 2.7, 0.2, 0.0)
    man(-7.5, 12.0, 2.8, 0.3, 0.0)
    man(2.6, 14.0, 2.6, 0.0, 0.2)
    man(7.2, 10.5, 2.9, -0.4, 0.0)
    s.light("directional", (0.3, -1, -0.4), (1, 1, 1), 0.5)
    s.write()


# --- oraborus ------------------------------------------------------------
def oraborus():
    s = Scene("oraborus", 0.3,
              "Alex's pocket knife -- a white ouroboros on black. The ring faces the\n"
              "camera (-z); its head is at the top, swallowing the tail.")
    white = s.material("snake", (0.95, 0.95, 0.93), roughness=0.5)
    eye = s.material("eye", (0.01, 0.01, 0.01), roughness=0.1)
    R = 1.2

    def point(theta_deg, radius=R):
        t = math.radians(theta_deg)
        return (radius * math.cos(t), -radius * math.sin(t), 0.0)  # -y is up

    # Head: a skull bulging out past the ring, and open jaws -- upper over
    # the tail tip, lower under it -- along the direction of travel
    # (clockwise, which at the top of the ring is +x).
    head_pos = point(97, R + 0.06)
    tangent = (1.0, -0.05, 0.0)
    hd = s.layer(smooth=0.05)
    s.prim(hd, "ellipsoid", head_pos, white, rot=euler_x_to(tangent),
           params=(0.3, 0.21, 0.24))
    s.prim(hd, "ellipsoid", (head_pos[0] + 0.3, head_pos[1] - 0.04, 0), white,
           rot=euler_x_to((1, 0.12, 0)), params=(0.22, 0.09, 0.16))  # upper jaw
    s.prim(hd, "ellipsoid", (head_pos[0] + 0.26, head_pos[1] + 0.17, 0), white,
           rot=euler_x_to((1, -0.25, 0)), params=(0.19, 0.055, 0.13))  # lower jaw
    scale = s.material("scales", (0.82, 0.82, 0.8), roughness=0.35)
    scales = s.layer(smooth=0.02)
    body = s.layer(smooth=0.07)
    # Tail tip just inside the jaws (top, slightly right), running clockwise
    # round the ring and thickening to the neck at the top-left.
    steps = 90
    start, sweep = 84.0, 338.0
    for i in range(steps + 1):
        u = i / steps
        r = 0.05 + 0.15 * (u ** 0.75)
        if u > 0.92:
            r *= 1 - (u - 0.92) * 1.2  # a little narrower at the neck
        s.prim(body, "sphere", point(start - sweep * u), white, params=(r,))
        # A ridge of scales down its spine, facing the camera.
        if i % 2 == 0 and 0.08 < u < 0.95:
            c = point(start - sweep * u)
            s.prim(scales, "ellipsoid", (c[0], c[1], -r * 0.78), scale,
                   rot=(0, 0, -math.radians(start - sweep * u)), params=(r * 0.5, r * 0.32, r * 0.3))
    e = s.layer()
    for z in (-0.19, 0.19):
        s.prim(e, "sphere", (head_pos[0] + 0.12, head_pos[1] - 0.06, z), eye,
               params=(0.045,))

    s.light("directional", (-0.4, -0.7, -1), (1, 1, 1), 0.9)
    s.light("point", (1.5, -1.5, -2.2), (0.9, 0.95, 1), 0.8)
    # Brow ridges over the eyes and nostrils at the snout.
    for z in (-0.17, 0.17):
        s.prim(hd, "ellipsoid", (head_pos[0] + 0.1, head_pos[1] - 0.13, z), white,
               params=(0.1, 0.035, 0.05))
        s.prim(e, "sphere", (head_pos[0] + 0.48, head_pos[1] - 0.09, z * 0.35), eye, params=(0.018,))
    s.write()

# --- soup ----------------------------------------------------------------
def soup():
    s = Scene("soup", 0.4,
              "Theresa's go-to meal -- tomato soup and toasted sourdough on a\n"
              "gingham tablecloth.")
    cloth = s.material("tablecloth", (1, 1, 1), base_map="gingham", uv_scale=0.45)
    bowl = s.material("bowl", (0.95, 0.93, 0.88), roughness=0.3)
    band = s.material("band", (0.25, 0.4, 0.62), roughness=0.3)
    tomato = s.material("soup", (0.78, 0.2, 0.07), roughness=0.2)
    cream = s.material("cream", (0.97, 0.94, 0.88), roughness=0.4)
    plate = s.material("plate", (0.96, 0.96, 0.94), roughness=0.25)
    crust = s.material("crust", (0.4, 0.22, 0.08), roughness=0.9)
    crumb = s.material("crumb", (0.78, 0.52, 0.26), roughness=1)
    steel = s.material("spoon", (0.85, 0.85, 0.87), roughness=0.12, metallic=1)

    t = s.layer()
    s.prim(t, "box", (0, 1.06, 0), cloth, params=(12, 0.06, 12))
    # Bowl, as the ramen's: a sphere shell with its top cut off.
    b = s.layer(smooth=0.03)
    s.prim(b, "sphere", (0, 0, 0), bowl, params=(1.0,))
    s.prim(b, "capped_cylinder", (0, 0.95, 0), bowl, params=(0.42, 0.05))
    cut = s.layer("subtraction", 0.02)
    s.prim(cut, "box", (0, -1.2, 0), bowl, params=(1.3, 1.05, 1.3))
    s.prim(cut, "sphere", (0, 0, 0), bowl, params=(0.92,))
    r = s.layer()
    s.prim(r, "torus", (0, -0.05, 0), band, params=(0.985, 0.025))
    w = s.layer()
    s.prim(w, "capped_cylinder", (0, 0.0, 0), tomato, params=(0.91, 0.04))
    sw = s.layer(smooth=0.03)
    # A drizzle of cream: a loose spiral of overlapping beads.
    for i in range(40):
        a = i * 0.28
        r = 0.03 + 0.0065 * i
        s.prim(sw, "sphere", (0.12 + r * math.cos(a), -0.04, 0.1 + r * math.sin(a)), cream,
               params=(0.026,))
    # The toast on its plate, to the right.
    pl = s.layer()
    s.prim(pl, "capped_cylinder", (1.95, 0.97, 0.5), plate, params=(0.85, 0.03))
    at = yawed((1.95, 0, 0.5), 0.35)
    tl = s.layer(smooth=0.02)
    s.prim(tl, "round_box", at((0, 0.86, 0)), crust, rot=(0, 0.35, 0),
           params=(0.55, 0.07, 0.45), extra=0.08)
    tt = s.layer()
    s.prim(tt, "box", at((0, 0.79, 0)), crumb, rot=(0, 0.35, 0),
           params=(0.47, 0.012, 0.37))
    # A spoon on the cloth, to the left.
    sp = s.layer(smooth=0.02)
    s.prim(sp, "ellipsoid", (1.0, 0.955, -0.55), steel, rot=(0, -0.6, 0),
           params=(0.2, 0.035, 0.13))
    s.capsule_between(sp, (1.17, 0.965, -0.67), (1.7, 0.96, -1.05), 0.025, steel)

    s.light("directional", (-0.4, -1, -0.6), (1, 0.95, 0.85), 0.9)
    s.light("point", (-1.2, -2.2, -1.5), (1, 0.9, 0.75), 1.2)
    # Butter melting on the toast, crumbs, a basil leaf on the soup.
    butter = s.material("butter", (0.98, 0.88, 0.5), roughness=0.3)
    bu = s.layer(smooth=0.02)
    s.prim(bu, "round_box", at((0.1, 0.76, -0.05)), butter, rot=(0, 0.9, 0),
           params=(0.13, 0.025, 0.1), extra=0.02)
    basil = s.material("basil", (0.2, 0.45, 0.15), roughness=0.5)
    bl = s.layer()
    s.prim(bl, "ellipsoid", (-0.35, -0.045, -0.25), basil, rot=(0, 0.7, 0), params=(0.13, 0.01, 0.06))
    s.prim(bl, "ellipsoid", (-0.25, -0.05, -0.33), basil, rot=(0, 1.9, 0), params=(0.11, 0.01, 0.05))
    cr = s.layer()
    rng = random.Random(23)
    for _ in range(12):
        s.prim(cr, "sphere", (1.95 + rng.uniform(-0.8, 0.8), 0.95, 0.5 + rng.uniform(-0.75, 0.75)),
               crust, params=(rng.uniform(0.012, 0.025),))
    # A napkin under the spoon, salt and pepper behind the bowl.
    napkin = s.material("napkin", (0.96, 0.95, 0.92), roughness=1)
    nk = s.layer()
    s.prim(nk, "round_box", (1.3, 0.985, -0.8), napkin, rot=(0, -0.6, 0),
           params=(0.42, 0.012, 0.3), extra=0.01)
    salt = s.material("salt", (0.95, 0.95, 0.95), roughness=0.1)
    pepper = s.material("pepper", (0.08, 0.08, 0.09), roughness=0.2)
    cap = s.material("shaker_caps", (0.82, 0.82, 0.84), roughness=0.2, metallic=1)
    sh = s.layer(smooth=0.02)
    for (x, z), mat in (((-1.5, 1.3), salt), ((-1.15, 1.6), pepper)):
        s.prim(sh, "capped_cylinder", (x, 0.75, z), mat, params=(0.12, 0.25))
        s.prim(sh, "round_box", (x, 0.45, z), cap, params=(0.125, 0.06, 0.125), extra=0.05)
    s.write()


# --- icecream ------------------------------------------------------------
def icecream():
    s = Scene("icecream", 0.5,
              "Alex's favourite food -- an ice cream cone, melting, against a\n"
              "pastel sky.")
    cone = s.material("cone", (0.85, 0.6, 0.3), roughness=0.9)
    pink = s.material("strawberry", (0.95, 0.5, 0.66), roughness=0.5)
    sprinkle = [s.material(f"sprinkle{i}", c, roughness=0.4) for i, c in
                enumerate([(0.95, 0.85, 0.2), (0.3, 0.6, 0.95), (0.95, 0.95, 0.95),
                           (0.45, 0.85, 0.4), (0.6, 0.3, 0.85)])]
    # The cone, point down: capped_cone's first radius is at -y (the top).
    c = s.layer()
    s.prim(c, "capped_cone", (0, -1.5, 0), cone, params=(0.6, 0.34, 0.02))
    # The scoop, slumping over the rim, with drips running down the cone.
    sc = s.layer(smooth=0.08)
    s.prim(sc, "sphere", (0, -2.32, 0), pink, params=(0.4,))
    s.prim(sc, "torus", (0, -2.1, 0), pink, params=(0.32, 0.08))
    s.capsule_between(sc, (0.26, -2.05, -0.17), (0.2, -1.72, -0.13), 0.05, pink)
    s.prim(sc, "sphere", (0.2, -1.7, -0.13), pink, params=(0.06,))
    s.capsule_between(sc, (-0.22, -2.05, -0.22), (-0.15, -1.55, -0.12), 0.042, pink)
    s.prim(sc, "sphere", (-0.15, -1.53, -0.12), pink, params=(0.052,))
    # A drop falling from the tip.
    d = s.layer()
    s.prim(d, "sphere", (0.02, -0.62, -0.01), pink, params=(0.045,))
    # Sprinkles over the front of the scoop.
    sp = s.layer()
    rng = random.Random(5)
    for i in range(18):
        th = rng.uniform(-1.2, 1.2)
        ph = rng.uniform(0.2, 1.3)
        pos = (0.41 * math.sin(th) * math.cos(ph - 0.6), -2.32 - 0.41 * math.sin(ph - 0.3),
               -0.41 * math.cos(th) * math.cos(ph - 0.6))
        s.prim(sp, "capsule", pos, sprinkle[i % len(sprinkle)],
               rot=(rng.uniform(0, 3), rng.uniform(0, 3), rng.uniform(0, 3)),
               params=(0.016, 0.03))

    s.light("directional", (0.3, -1, -0.6), (1, 1, 1), 0.9)
    s.light("point", (-1.5, -2.5, -2), (1, 0.9, 0.95), 0.8)
    # A cherry on top, a wafer stuck in, ridges up the cone and a paper
    # sleeve round its lower half.
    cherry = s.material("cherry", (0.75, 0.04, 0.08), roughness=0.15)
    stem = s.material("cherry_stem", (0.35, 0.28, 0.1), roughness=0.8)
    wafer = s.material("wafer", (0.92, 0.78, 0.5), roughness=0.8)
    paper = s.material("sleeve", (0.96, 0.96, 0.98), roughness=0.9)
    stripe = s.material("sleeve_stripe", (0.35, 0.55, 0.95), roughness=0.9)
    ch = s.layer(smooth=0.01)
    s.prim(ch, "sphere", (0.04, -2.78, -0.04), cherry, params=(0.08,))
    s.capsule_between(ch, (0.04, -2.85, -0.04), (0.1, -3.0, 0.02), 0.01, stem)
    wf = s.layer()
    s.prim(wf, "box", (-0.2, -2.72, 0.12), wafer, rot=(0.25, 0.4, 0.35), params=(0.05, 0.24, 0.035))
    rg = s.layer()
    for y in (-2.0, -1.85, -1.7):
        rr = 0.34 + (0.02 - 0.34) * (y + 2.1) / 1.2
        s.prim(rg, "torus", (0, y, 0), cone, params=(rr, 0.012))
    sl = s.layer()
    s.prim(sl, "capped_cone", (0, -1.27, 0), paper, params=(0.33, 0.215, 0.05))
    s.prim(sl, "torus", (0, -1.48, 0), stripe, params=(0.15, 0.012))
    s.write()


# --- flamenco / dragbar: one stage, two nights ---------------------------
def stage(name, comment, ambient):
    s = Scene(name, ambient, comment)
    boards = s.material("boards", (0.6, 0.5, 0.42), base_map="woodfloor", uv_scale=0.7,
                        roughness=0.5)
    st = s.layer()
    s.prim(st, "box", (0, 0.25, 0.5), boards, params=(6, 0.25, 4))
    return s


def flamenco():
    s = stage("flamenco",
              "Theresa's flamenco -- an empty red dress on a stand, a chair, one\n"
              "spotlight.", 0.03)
    black = s.material("backdrop", (0.025, 0.02, 0.02), roughness=1)
    red = s.material("dress", (0.72, 0.04, 0.06), roughness=0.6)
    dark_red = s.material("ruffle", (0.45, 0.02, 0.04), roughness=0.6)
    stand = s.material("stand", (0.08, 0.07, 0.06), roughness=0.4)
    wood = s.material("chair", (0.55, 0.35, 0.2), base_map="brownwoodtexture",
                      uv_scale=0.8, roughness=0.6)
    beam = s.material("beam", (1, 0.93, 0.8))
    bd = s.layer()
    s.prim(bd, "box", (0, -3, 4.2), black, params=(7, 3.2, 0.1))
    # The dress form: tripod, pole, neck knob.
    fr = s.layer()
    x, z = 0.5, 0.8
    for k in range(3):
        a = k / 3 * math.tau + 0.3
        s.capsule_between(fr, (x, -0.04, z), (x + 0.4 * math.cos(a), -0.02, z + 0.4 * math.sin(a)),
                          0.025, stand)
    s.capsule_between(fr, (x, -0.04, z), (x, -2.0, z), 0.025, stand)
    s.capsule_between(fr, (x, -1.98, z), (x, -2.1, z), 0.045, stand)
    s.prim(fr, "sphere", (x, -2.14, z), stand, params=(0.065,))
    # Bodice and skirt -- the skirt's narrow end (first radius) at the waist.
    # A fitted bodice over a long bell skirt, ruffled at the hem, with a
    # short train behind.
    d = s.layer(smooth=0.06)
    s.capsule_between(d, (x - 0.18, -1.96, z), (x + 0.18, -1.96, z), 0.06, red)  # shoulders
    s.prim(d, "ellipsoid", (x, -1.7, z), red, params=(0.19, 0.32, 0.14))  # bodice
    top, bottom, r_top, r_bottom = -1.4, -0.05, 0.16, 0.75
    s.prim(d, "capped_cone", (x, (top + bottom) / 2, z), red,
           params=((bottom - top) / 2, r_top, r_bottom))  # skirt
    s.prim(d, "ellipsoid", (x, -0.05, z + 0.4), red, params=(0.55, 0.05, 0.55))  # train
    ru = s.layer(smooth=0.03)
    for y, mat in ((-0.1, dark_red), (-0.32, red), (-0.54, dark_red)):
        rr = r_top + (r_bottom - r_top) * (y - top) / (bottom - top)
        s.prim(ru, "torus", (x, y, z), mat, rot=(0.06, 0, 0.04), params=(rr + 0.01, 0.055))
    # A plain wooden chair beside it, turned to face the dress.
    ch = s.layer()
    at = yawed((-1.3, 0, 0.6), 0.6)
    s.prim(ch, "box", at((0, -0.9, 0)), wood, rot=(0, 0.6, 0), params=(0.42, 0.035, 0.4))
    for dx, dz in ((-0.36, -0.34), (0.36, -0.34), (-0.36, 0.34), (0.36, 0.34)):
        top = -1.95 if dz > 0 else -0.9
        s.capsule_between(ch, at((dx, 0, dz)), at((dx, top, dz)), 0.035, wood)
    for y in (-1.45, -1.85):
        s.prim(ch, "box", at((0, y, 0.34)), wood, rot=(0, 0.6, 0), params=(0.38, 0.06, 0.025))
    # A rose left on the seat.
    rose = s.material("rose", (0.6, 0.02, 0.05), roughness=0.6)
    stem = s.material("stem", (0.12, 0.3, 0.1), roughness=0.8)
    rs = s.layer(smooth=0.01)
    s.capsule_between(rs, at((-0.15, -0.95, -0.1)), at((0.2, -0.95, 0.15)), 0.012, stem)
    s.prim(rs, "sphere", at((-0.18, -0.99, -0.12)), rose, params=(0.06,))
    # The spotlight: a cone of light from overhead onto the dress.
    s.volumetric("capped_cone", (0.4, -3.2, 0.7), (0, 0, 0.06), (3.2, 0.25, 1.5), 0.12, beam)
    s.light("point", (0.3, -4.5, 0.3), (1, 0.92, 0.78), 3.0)
    s.light("directional", (0.1, -1, -0.3), (1, 0.92, 0.8), 0.15)
    # A flower at the neckline; red heels kicked off by the stand; a black
    # lace fan and castanets left on the chair; a fringed shawl over its
    # back.
    s.prim(rs, "sphere", (x + 0.1, -1.94, z - 0.13), rose, params=(0.05,))
    shoe = s.material("shoes", (0.55, 0.03, 0.05), roughness=0.25)
    sh = s.layer(smooth=0.01)
    for sx, sz, syaw in ((1.25, 0.2, 0.4), (1.45, 0.45, 1.3)):
        sat = yawed((sx, 0, sz), syaw)
        s.prim(sh, "ellipsoid", sat((0, -0.05, 0)), shoe, rot=(0, syaw, 0), params=(0.06, 0.04, 0.13))
        s.capsule_between(sh, sat((0, -0.02, 0.11)), sat((0, -0.12, 0.1)), 0.015, shoe)
    lace = s.material("fan", (0.05, 0.03, 0.04), roughness=0.5)
    fan = s.layer()
    pivot = at((0.1, -0.95, -0.15))
    for k in range(9):
        ang = 0.6 + 0.6 * k / 8 * math.pi
        tip = (pivot[0] + 0.28 * math.cos(ang + 0.6), pivot[1] - 0.006, pivot[2] + 0.28 * math.sin(ang + 0.6))
        s.capsule_between(fan, pivot, tip, 0.008, lace)
        s.prim(fan, "box", tuple((p + q) / 2 for p, q in zip(pivot, tip)), lace,
               rot=(0, -(ang + 0.6), 0), params=(0.12, 0.004, 0.03))
    cast = s.material("castanets", (0.25, 0.12, 0.06), roughness=0.3)
    s.prim(fan, "ellipsoid", at((-0.2, -0.96, 0.15)), cast, params=(0.06, 0.025, 0.07))
    s.prim(fan, "ellipsoid", at((-0.08, -0.96, 0.2)), cast, params=(0.06, 0.025, 0.07))
    shawl = s.material("shawl", (0.95, 0.88, 0.75), roughness=1)
    sw = s.layer(smooth=0.02)
    s.prim(sw, "round_box", at((0, -1.9, 0.34)), shawl, rot=(0, 0.6, 0),
           params=(0.42, 0.1, 0.05), extra=0.03)
    for k in range(14):
        fx = -0.38 + 0.76 * k / 13
        s.capsule_between(sw, at((fx, -1.82, 0.38)), at((fx * 1.02, -1.55, 0.4)), 0.008, shawl)
    s.write()


def dragbar():
    s = stage("dragbar",
              "Theresa's first date -- a gay bar's stage, a glitter curtain, coloured\n"
              "lights and a mirror ball.", 0.08)
    gold = s.material("gold_strands", (0.95, 0.72, 0.3), roughness=0.12,
                      emissive_colour=(1, 0.7, 0.3), emissive_intensity=0.15)
    silver = s.material("pink_strands", (0.95, 0.55, 0.85), roughness=0.12,
                        emissive_colour=(1, 0.5, 0.8), emissive_intensity=0.15)
    mirror = s.material("mirror_ball", (0.85, 0.85, 0.9), roughness=0.05, metallic=1,
                        emissive_colour=(0.8, 0.85, 1), emissive_intensity=0.5)
    metal = s.material("mic_stand", (0.55, 0.55, 0.58), roughness=0.2, metallic=1)
    pinkbeam = s.material("pink_beam", (1, 0.3, 0.75))
    bluebeam = s.material("blue_beam", (0.3, 0.75, 1))
    # The glitter curtain: two rows of hanging strands.
    cu = s.layer()
    s.prim(cu, "capsule", (0, -4, 3.9), gold, params=(0.024, 4),
           **limited((0.1, 0, 0), (200, 1, 1)))
    s.prim(cu, "capsule", (0.05, -4, 3.94), silver, params=(0.024, 4),
           **limited((0.1, 0, 0), (200, 1, 1)))
    # Mirror ball, hanging.
    mb = s.layer()
    s.capsule_between(mb, (0, -6.5, 1.5), (0, -3.6, 1.5), 0.02, metal)
    s.prim(mb, "sphere", (0, -3.35, 1.5), mirror, params=(0.25,))
    # A microphone stand front and centre.
    mc = s.layer(smooth=0.01)
    s.prim(mc, "capped_cylinder", (0, -0.015, 0.3), metal, params=(0.22, 0.015))
    s.capsule_between(mc, (0, -0.02, 0.3), (0, -1.55, 0.3), 0.015, metal)
    s.capsule_between(mc, (0, -1.55, 0.3), (0, -1.7, 0.2), 0.022, metal)
    s.prim(mc, "sphere", (0, -1.72, 0.19), metal, params=(0.04,))
    # Two coloured beams crossing at the mic.
    s.volumetric("capped_cone", (-1.6, -3.0, 0.4), (0, 0, -0.5), (3.2, 0.15, 0.9), 0.1, pinkbeam)
    s.volumetric("capped_cone", (1.6, -3.0, 0.4), (0, 0, 0.5), (3.2, 0.15, 0.9), 0.1, bluebeam)
    s.light("point", (-2.8, -3.2, -1), (1, 0.35, 0.75), 2.2)
    s.light("point", (2.8, -3.2, -1), (0.35, 0.75, 1), 2.2)
    s.light("point", (0, -4.5, 2.5), (0.7, 0.4, 1), 1.2)
    # Two queens and the man who took her there, dancing with them --
    # "on stage with the drag queens in about 30 minutes".
    skin_a = s.material("skin_a", (0.78, 0.58, 0.46), roughness=0.6)
    skin_b = s.material("skin_b", (0.5, 0.33, 0.24), roughness=0.6)
    suit = s.material("his_shirt", (0.85, 0.85, 0.82), roughness=0.8)
    trousers = s.material("his_trousers", (0.12, 0.12, 0.15), roughness=0.8)
    sequins = s.material("sequins", (0.95, 0.4, 0.75), roughness=0.15, metallic=1)
    gold_dress = s.material("gold_dress", (0.95, 0.75, 0.35), roughness=0.15, metallic=1)
    hair_a = s.material("wig_platinum", (0.97, 0.93, 0.8), roughness=0.6)
    hair_b = s.material("wig_red", (0.8, 0.15, 0.1), roughness=0.6)
    boa = s.material("boa", (0.95, 0.3, 0.65), roughness=1)

    def queen(qx, qz, face, skin, dress, wig, arms_up):
        h = 2.05
        at = figure(s, qx, qz, h, face, 0.0, skin, wide=True, arms_up=arms_up)
        d = s.layer(smooth=0.03)
        s.prim(d, "ellipsoid", at(0, -0.72), dress, rot=(0, face, 0),
               params=(0.11 * h, 0.1 * h, 0.075 * h))
        mid = at(0, -0.5)
        s.prim(d, "capped_cone", mid, dress, rot=(0, face, 0),
               params=(0.17 * h, 0.085 * h, 0.15 * h))
        w = s.layer(smooth=0.05)
        for dx, dy, dz, rr in ((0, -0.985, 0.02, 0.07), (-0.05, -0.95, 0.03, 0.055),
                               (0.05, -0.95, 0.03, 0.055), (0, -0.93, 0.06, 0.06)):
            s.prim(w, "sphere", at(dx, dy, dz), wig, params=(rr * h,))
        b = s.layer(smooth=0.02)
        for k in range(11):
            t = k / 10 * math.pi
            s.prim(b, "sphere", at(-0.13 * math.cos(t), -0.8 + 0.06 * math.sin(t), -0.04 * math.sin(t)),
                   boa, params=(0.022 * h,))

    queen(-1.35, 1.3, 0.35, skin_a, sequins, hair_a, False)
    queen(1.45, 1.5, -0.4, skin_b, gold_dress, hair_b, True)
    # Him, in his date clothes, arms up mid-dance.
    hx, hz, hface = 0.25, 1.0, 0.1
    him = figure(s, hx, hz, 1.85, hface, 0.0, suit, wide=True, arms_up=True)
    lg = s.layer(smooth=0.02)
    for sgn in (-1, 1):
        round_cone_between(s, lg, him(sgn * 0.052, -0.5), him(sgn * 0.058, -0.27), 0.06 * 1.85,
                           0.042 * 1.85, trousers)
        round_cone_between(s, lg, him(sgn * 0.058, -0.27), him(sgn * 0.06, -0.05), 0.042 * 1.85,
                           0.03 * 1.85, trousers)
    hd = s.layer()
    s.prim(hd, "ellipsoid", him(0, -0.935), skin_a, rot=(0, hface, 0),
           params=(0.056 * 1.85, 0.066 * 1.85, 0.062 * 1.85))
    # Footlights along the front edge of the stage.
    foot = s.material("footlights", (1, 0.85, 0.6), emissive_colour=(1, 0.8, 0.5),
                      emissive_intensity=1.5, casts_shadow=0)
    fl = s.layer()
    s.prim(fl, "ellipsoid", (0, -0.05, -3.35), foot, params=(0.08, 0.05, 0.05),
           **limited((0.7, 0, 0), (15, 1, 1)))
    s.light("point", (0, -0.6, -2.5), (1, 0.8, 0.6), 0.8)
    s.write()


# --- puzzles -------------------------------------------------------------
def puzzles():
    s = Scene("puzzles", 0.3,
              "Pat's hospital -- the same three jigsaws, half done, and a paper cup of\n"
              "pills, under cold light.")
    floor = s.material("floor", (0.9, 0.92, 0.92), base_map="hospitalfloor", uv_scale=1)
    top = s.material("table", (0.68, 0.76, 0.74), roughness=0.3)
    leg = s.material("legs", (0.6, 0.62, 0.65), roughness=0.3)
    tray = s.material("tray", (0.58, 0.52, 0.43), roughness=1)
    paper = s.material("cup", (0.97, 0.97, 0.97), roughness=0.8)
    pills = [s.material(f"pill{i}", c, roughness=0.3) for i, c in
             enumerate([(0.95, 0.95, 0.95), (0.35, 0.55, 0.9), (0.95, 0.6, 0.2)])]
    pics = [s.material(f"puzzle{i}", (0.85, 0.85, 0.85), base_map=f"painting{i + 1}", uv_scale=0.9)
            for i in range(3)]
    fl = s.layer()
    s.prim(fl, "box", (0, 1.55, 0), floor, params=(12, 0.05, 12))
    tb = s.layer()
    s.prim(tb, "round_box", (0, 0.04, 0), top, params=(1.6, 0.04, 1.0), extra=0.02)
    for x in (-1.5, 1.5):
        for z in (-0.9, 0.9):
            s.capsule_between(tb, (x, 0.08, z), (x, 1.5, z), 0.035, leg)
    # Each puzzle: a picture with an empty, cardboard-coloured patch where the
    # missing part goes, and a few loose pieces beside it.
    pz = s.layer()
    covers = s.layer()
    loose = s.layer()
    rng = random.Random(4)
    for i, (x, z, yaw) in enumerate(((-0.85, -0.2, 0.1), (0.1, 0.35, -0.15), (0.95, -0.3, 0.25))):
        at = yawed((x, 0, z), yaw)
        s.prim(pz, "box", at((0, -0.008, 0)), pics[i], rot=(0, yaw, 0), params=(0.38, 0.008, 0.27))
        s.prim(covers, "box", at((0.2, -0.017, -0.08)), tray, rot=(0, yaw, 0),
               params=(0.16, 0.002, 0.17))
        s.prim(covers, "box", at((0.06, -0.017, -0.18)), tray, rot=(0, yaw, 0),
               params=(0.08, 0.002, 0.07))
        for _ in range(5):
            px, pzz = rng.uniform(-0.5, 0.5), rng.uniform(-0.45, -0.3)
            s.prim(loose, "box", at((px, -0.004, pzz)), pics[i],
                   rot=(0, rng.uniform(0, math.pi), 0), params=(0.035, 0.004, 0.035))
    # The pill cup, and three pills tipped out next to it.
    cp = s.layer()
    s.prim(cp, "capped_cone", (1.25, -0.09, 0.6), paper, params=(0.09, 0.07, 0.05))
    pi = s.layer()
    for k, (x, z, yaw) in enumerate(((1.0, 0.45, 0.4), (1.08, 0.38, 1.6), (0.96, 0.33, 2.5))):
        s.prim(pi, "capsule", (x, -0.016, z), pills[k], rot=(math.pi / 2, yaw, 0),
               params=(0.016, 0.022))

    s.light("directional", (0.2, -1, -0.4), (0.85, 0.92, 1), 0.55)
    s.light("point", (0, -3, 0), (0.85, 0.95, 1), 0.6)
    # The box lid with its picture, a plastic cup of water, a hospital
    # wristband, and a couple of pieces fallen to the floor.
    lid = s.material("lid", (0.85, 0.85, 0.85), base_map="painting2", uv_scale=0.7)
    card = s.material("box_side", (0.3, 0.45, 0.65), roughness=0.8)
    cupm = s.material("plastic_cup", (0.82, 0.9, 0.95), roughness=0.1)
    water = s.material("water", (0.6, 0.75, 0.85), roughness=0.05)
    band = s.material("wristband", (0.95, 0.95, 0.92), roughness=0.6)
    tag = s.material("wristband_tag", (0.95, 0.85, 0.3), roughness=0.6)
    lt = s.layer()
    s.prim(lt, "box", (-1.2, -0.035, 0.65), card, rot=(0, -0.25, 0), params=(0.3, 0.035, 0.22))
    s.prim(lt, "box", (-1.2, -0.071, 0.65), lid, rot=(0, -0.25, 0), params=(0.29, 0.002, 0.21))
    s.prim(lt, "capped_cone", (1.35, -0.12, -0.55), cupm, params=(0.12, 0.06, 0.075))
    s.prim(lt, "capped_cylinder", (1.35, -0.18, -0.55), water, params=(0.068, 0.005))
    s.prim(lt, "torus", (0.55, -0.02, 0.7), band, params=(0.13, 0.02))
    tg = s.layer()
    s.prim(tg, "box", (0.42, -0.04, 0.62), tag, rot=(0, 0.3, 0), params=(0.06, 0.006, 0.03))
    fp = s.layer()
    for k, (px, pz) in enumerate(((-0.6, -1.3), (0.4, -1.6), (1.2, -1.2))):
        s.prim(fp, "box", (px, 1.496, pz), pics[k], rot=(0, k * 0.9, 0), params=(0.035, 0.004, 0.035))
    s.write()


# --- cabin ---------------------------------------------------------------
def pine(s, layer, trunk_layer, x, z, h, needles, bark, base=0.0):
    s.capsule_between(trunk_layer, (x, base, z), (x, base - h * 0.35, z), 0.06 * h / 3, bark)
    for k, (y0, y1, w) in enumerate(((0.2, 0.55, 0.42), (0.45, 0.8, 0.32), (0.7, 1.0, 0.2))):
        hh = (y1 - y0) * h / 2
        s.prim(layer, "capped_cone", (x, base - (y0 + y1) * h / 2, z), needles,
               params=(hh, 0.02 * h, w * h))


def cabin():
    s = Scene("cabin", 0.25,
              "Theo's daydream -- a cabin by a lake in the woods, in the fog.")
    water = s.material("lake", (0.07, 0.1, 0.13), roughness=0.05)
    grass = s.material("shore", (0.14, 0.18, 0.11), roughness=1)
    needles = s.material("pines", (0.05, 0.11, 0.08), roughness=1)
    bark = s.material("bark", (0.18, 0.12, 0.08), roughness=1)
    logs = s.material("logs", (0.55, 0.38, 0.25), base_map="woodpanel", uv_scale=0.8,
                      roughness=0.9)
    roof = s.material("roof", (0.14, 0.11, 0.1), roughness=1)
    stone = s.material("chimney", (0.6, 0.6, 0.6), base_map="cobblestone", uv_scale=1)
    door = s.material("door", (0.3, 0.2, 0.13), roughness=0.8)
    window = s.material("window", (1, 0.8, 0.5), emissive_colour=(1, 0.75, 0.4),
                        emissive_intensity=1.5)
    fog = s.material("fog", (0.45, 0.5, 0.55))
    g = s.layer()
    s.prim(g, "box", (0, 0.05, 12), water, params=(45, 0.05, 30))
    s.prim(g, "box", (0, 0.0, -6), grass, params=(45, 0.15, 6))
    s.prim(g, "box", (0, 0.0, 30), grass, params=(45, 0.3, 12))
    # The cabin on the far shore, its gable end to us.
    cx, cz = 2.0, 20.5
    h = s.layer()
    roofs = s.layer()
    gable_house(s, h, roofs, cx, cz, 4.0, 2.4, 3.2, logs, roof)
    # Lift it onto the shore (the shore's top is y = -0.3).
    for p in h["prims"] + roofs["prims"]:
        p["pos"] = (p["pos"][0], p["pos"][1] - 0.3, p["pos"][2])
    det = s.layer()
    s.prim(det, "box", (cx + 1.2, -4.2, cz + 0.6), stone, params=(0.22, 0.75, 0.22))
    s.prim(det, "box", (cx, -1.05, cz - 1.62), door, params=(0.35, 0.75, 0.03))
    for wx in (cx - 1.15, cx + 1.15):
        s.prim(det, "box", (wx, -1.8, cz - 1.62), window, params=(0.32, 0.26, 0.03))
    # A jetty out into the water.
    jt = s.layer()
    s.prim(jt, "box", (cx, -0.32, 16.2), logs, params=(0.55, 0.04, 2.2))
    for z in (14.4, 15.8, 17.2):
        for dx in (-0.5, 0.5):
            s.capsule_between(jt, (cx + dx, 0.1, z), (cx + dx, -0.32, z), 0.05, bark)
    # Pines along the far shore, and a couple framing the near one.
    t = s.layer(smooth=0.05)
    tr = s.layer()
    rng = random.Random(9)
    x = -26.0
    while x < 26.0:
        z = rng.uniform(22.5, 34)
        if not (cx - 3.5 < x < cx + 3.5 and z < 25):
            pine(s, t, tr, x, z, rng.uniform(4, 8), needles, bark, base=-0.3)
        x += rng.uniform(0.9, 2.2)
    for x, z, ht in ((-6.5, -1.5, 7), (-8, 1, 6), (7.5, -0.5, 7.5)):
        pine(s, t, tr, x, z, ht, needles, bark, base=-0.15)
    # Low fog over the water.
    s.volumetric("box", (0, -0.5, 14), (0, 0, 0), (45, 0.6, 16), 0.012, fog)
    s.light("directional", (-0.4, -0.6, -1), (0.7, 0.75, 0.9), 0.45)
    s.light("point", (cx, -1.5, cz - 2.4), (1, 0.75, 0.45), 1.2)
    # A canoe tied at the jetty, reeds and stones along the near shore, a
    # lantern at the jetty's end, smoke from the chimney.
    canoe = s.material("canoe", (0.62, 0.14, 0.08), roughness=0.4)
    inside = s.material("canoe_inside", (0.35, 0.22, 0.14), roughness=0.8)
    cn = s.layer(smooth=0.02)
    s.prim(cn, "ellipsoid", (cx + 1.15, 0.02, 16.4), canoe, rot=(0, 0.12, 0), params=(0.32, 0.17, 1.7))
    ci = s.layer()
    s.prim(ci, "ellipsoid", (cx + 1.15, -0.13, 16.4), inside, rot=(0, 0.12, 0), params=(0.25, 0.035, 1.5))
    reed = s.material("reeds", (0.42, 0.45, 0.22), roughness=1)
    rd = s.layer()
    rng2 = random.Random(41)
    for _ in range(40):
        rx, rz = rng2.uniform(-9, 6), rng2.uniform(0.1, 1.2)
        h2 = rng2.uniform(0.5, 1.1)
        s.capsule_between(rd, (rx, 0.05, rz), (rx + rng2.uniform(-0.15, 0.15), -h2, rz + rng2.uniform(-0.1, 0.1)),
                          0.014, reed)
    stone2 = s.material("stones", (0.4, 0.4, 0.38), roughness=0.9)
    sn = s.layer(smooth=0.05)
    for _ in range(12):
        sx, sz = rng2.uniform(-8, 7), rng2.uniform(-0.6, 0.6)
        rr = rng2.uniform(0.12, 0.35)
        s.prim(sn, "ellipsoid", (sx, -0.1, sz), stone2, rot=(0, rng2.uniform(0, 3), 0),
               params=(rr * 1.3, rr * 0.6, rr))
    lantern = s.material("lantern", (1, 0.8, 0.45), emissive_colour=(1, 0.75, 0.4),
                         emissive_intensity=1.5, casts_shadow=0)
    ln = s.layer()
    s.prim(ln, "capped_cylinder", (cx + 0.35, -0.48, 14.3), lantern, params=(0.06, 0.12))
    s.light("point", (cx + 0.35, -0.75, 14.1), (1, 0.75, 0.4), 0.25)
    smoke = s.material("smoke", (0.5, 0.52, 0.55))
    s.volumetric("capped_cone", (cx + 1.35, -6.4, cz + 0.6), (0, 0, -0.25), (1.4, 0.2, 0.7), 0.05, smoke)
    s.write()


# --- road ----------------------------------------------------------------
def road():
    s = Scene("road", 0.35,
              "Alex's road trip -- an empty two-lane road through the desert to the\n"
              "horizon, mesas either side.")
    sand = s.material("sand", (0.8, 0.58, 0.38), roughness=1)
    tar = s.material("asphalt", (0.13, 0.13, 0.14), roughness=0.9)
    yellow = s.material("centre_line", (0.95, 0.75, 0.15), roughness=0.6)
    white = s.material("edge_line", (0.92, 0.92, 0.9), roughness=0.6)
    rock = s.material("rock", (0.68, 0.34, 0.2), roughness=1)
    rock2 = s.material("rock_dark", (0.55, 0.27, 0.17), roughness=1)
    g = s.layer()
    s.prim(g, "box", (0, 0.1, 50), sand, params=(100, 0.1, 100))
    rd = s.layer()
    s.prim(rd, "box", (0, 0.0, 50), tar, params=(2.3, 0.105, 100))
    ln = s.layer()
    # Painted lines, as separate short pieces: one long thin box -- or a
    # repeated dash spanning the same length -- turned the whole frame
    # black (not yet understood; a 2-long box on its own is fine).
    for i in range(20):
        z = 1 + i * 3.6
        s.prim(ln, "box", (0, -0.105, z), yellow, params=(0.08, 0.03, 1.0))
        for x in (-2.0, 2.0):
            s.prim(ln, "box", (x, -0.105, z), white, params=(0.07, 0.03, 1.8))
    # Mesas and buttes: flat-topped rock, stepped in.
    m = s.layer(smooth=0.4)
    for x, z, w, ht, d, mat in ((-32, 62, 14, 10, 6, rock), (30, 72, 8, 14, 6, rock2),
                                (-11, 48, 3, 6, 3, rock2), (55, 52, 16, 8, 8, rock),
                                (-60, 80, 18, 6, 10, rock2)):
        s.prim(m, "round_box", (x, -ht / 2, z), mat, params=(w, ht / 2, d), extra=0.6)
        s.prim(m, "round_box", (x + w * 0.1, -ht - 1.0, z), mat,
               params=(w * 0.65, 1.2, d * 0.8), extra=0.5)
    # A rock pillar, stepped in towards its cap.
    sp = s.layer(smooth=0.3)
    s.prim(sp, "round_box", (14, -3.5, 56), rock, params=(2.2, 3.5, 1.8), extra=0.4)
    s.prim(sp, "round_box", (14.3, -8, 56), rock, params=(1.5, 1.6, 1.3), extra=0.4)
    s.prim(sp, "round_box", (14.1, -9.9, 56), rock2, params=(1.9, 0.4, 1.6), extra=0.2)
    s.light("directional", (-0.5, -0.6, -0.6), (1, 0.9, 0.75), 1.1)
    # Telephone poles marching off beside it, saguaros, a sign, and a
    # tumbleweed caught at the verge.
    wood = s.material("poles", (0.32, 0.24, 0.18), roughness=1)
    cactus = s.material("cactus", (0.3, 0.45, 0.25), roughness=0.9)
    sign = s.material("sign", (0.95, 0.78, 0.1), roughness=0.4)
    post = s.material("sign_post", (0.6, 0.6, 0.62), roughness=0.3, metallic=1)
    weed = s.material("tumbleweed", (0.62, 0.5, 0.32), roughness=1)
    pl = s.layer()
    for k in range(9):
        z = 6 + k * 11
        s.capsule_between(pl, (4.6, 0.0, z), (4.6, -5.5, z), 0.1, wood)
        s.capsule_between(pl, (3.9, -5.1, z), (5.3, -5.1, z), 0.07, wood)
    ct = s.layer(smooth=0.08)
    for x, z, h in ((-6, 9, 3.2), (8, 16, 4.0), (-10, 22, 3.6), (12, 30, 4.4), (-7.5, 38, 3.0)):
        s.capsule_between(ct, (x, 0, z), (x, -h, z), 0.22, cactus)
        s.capsule_between(ct, (x, -h * 0.45, z), (x - 0.7, -h * 0.5, z), 0.15, cactus)
        s.capsule_between(ct, (x - 0.7, -h * 0.5, z), (x - 0.7, -h * 0.8, z), 0.15, cactus)
        s.capsule_between(ct, (x, -h * 0.6, z), (x + 0.6, -h * 0.62, z), 0.14, cactus)
        s.capsule_between(ct, (x + 0.6, -h * 0.62, z), (x + 0.6, -h * 0.85, z), 0.14, cactus)
    sg = s.layer()
    s.capsule_between(sg, (3.2, 0, 12), (3.2, -2.2, 12), 0.04, post)
    s.prim(sg, "box", (3.2, -2.4, 11.95), sign, rot=(0, 0, math.pi / 4), params=(0.38, 0.38, 0.02))
    tw = s.layer()
    for k in range(6):
        s.prim(tw, "torus", (-2.9, -0.35, 7), weed, rot=(k * 0.5, k * 1.1, k * 0.3), params=(0.32, 0.02))
    s.write()


# --- trippyroom ----------------------------------------------------------
def trippyroom():
    s = Scene("trippyroom", 0.35,
              "Mel's drug years -- a room gone wrong: leaning walls, loud colours,\n"
              "shapes repeating off into it, rings on the back wall.")
    floor = s.material("floor", (1, 0.45, 0.8), base_map="floral3", uv_scale=0.8)
    walls = [s.material(f"wall{i}", c, base_map=m, uv_scale=0.7) for i, (c, m) in
             enumerate((((0.6, 1, 0.6), "floral1"), ((0.6, 0.8, 1), "floral5"),
                        ((1, 0.8, 0.4), "floral7"), ((1, 0.6, 1), "floral9")))]
    gem = s.material("gems", (0.2, 1, 0.9), emissive_colour=(0.2, 1, 0.9),
                     emissive_intensity=0.6)
    dot = s.material("dots", (1, 0.9, 0.1), emissive_colour=(1, 0.85, 0.1),
                     emissive_intensity=0.6)
    rings = [s.material(f"ring{i}", c, emissive_colour=c, emissive_intensity=1.0)
             for i, c in enumerate(((1, 0.15, 0.2), (1, 0.6, 0.1), (1, 0.95, 0.2),
                                    (0.2, 0.9, 0.3), (0.2, 0.5, 1), (0.6, 0.2, 1)))]
    r = s.layer()
    s.prim(r, "box", (0, 0.1, 0.5), floor, params=(4.5, 0.1, 5))
    s.prim(r, "box", (0, -1.6, 4.1), walls[0], rot=(0.08, 0, 0.05), params=(4.5, 1.9, 0.1))
    s.prim(r, "box", (-4.1, -1.6, 0.5), walls[1], rot=(0, 0, -0.12), params=(0.1, 1.9, 5))
    s.prim(r, "box", (4.1, -1.6, 0.5), walls[2], rot=(0.06, 0, 0.1), params=(0.1, 1.9, 5))
    s.prim(r, "box", (0, -3.4, 0.5), walls[3], rot=(0.05, 0, -0.04), params=(4.5, 0.1, 5))
    rg = s.layer()
    for i, mat in enumerate(rings):
        s.prim(rg, "torus", (0, -1.7, 3.85), mat, rot=(math.pi / 2, 0, 0),
               params=(0.3 + 0.2 * i, 0.06))
    fl = s.layer()
    s.prim(fl, "octahedron", (0, -1.6, 1.2), gem, rot=(0.4, 0.6, 0), params=(0.16,),
           **limited((1.1, 0.9, 1.1), (7, 3, 5)))
    s.prim(fl, "sphere", (0.55, -1.15, 1.75), dot, params=(0.09,),
           **limited((1.1, 0.9, 1.1), (7, 3, 5)))
    s.light("point", (0, -2.5, 0), (1, 0.4, 0.9), 1.4)
    s.light("point", (2, -1, 2), (0.3, 1, 1), 0.9)
    # Giant spotted mushrooms, eyes hanging in the air watching, and a lava
    # lamp.
    stalk = s.material("stalks", (0.98, 0.92, 0.8), roughness=0.6)
    capm = s.material("mushroom_caps", (0.95, 0.15, 0.3), emissive_colour=(0.9, 0.1, 0.3),
                      emissive_intensity=0.25)
    spot = s.material("spots", (1, 1, 0.95), roughness=0.5)
    mu = s.layer(smooth=0.05)
    for mx, mz, h in ((-2.8, 2.2, 1.6), (-2.0, 3.2, 1.1), (3.0, 2.8, 1.9)):
        s.capsule_between(mu, (mx, 0, mz), (mx, -h, mz), 0.13 * h, stalk)
        s.prim(mu, "ellipsoid", (mx, -h - 0.1, mz), capm, params=(0.55 * h, 0.25 * h, 0.55 * h))
    sp = s.layer()
    rng = random.Random(29)
    for mx, mz, h in ((-2.8, 2.2, 1.6), (-2.0, 3.2, 1.1), (3.0, 2.8, 1.9)):
        for _ in range(6):
            a = rng.uniform(0, math.tau)
            r = rng.uniform(0.15, 0.4) * h
            s.prim(sp, "sphere", (mx + r * math.cos(a), -h - 0.1 - 0.2 * h * (1 - (r / (0.55 * h)) ** 2) ** 0.5,
                                  mz + r * math.sin(a)), spot, params=(0.05 * h,))
    eyew = s.material("eye_white", (0.98, 0.98, 0.96), roughness=0.1)
    iris = s.material("iris", (0.2, 0.55, 0.9), roughness=0.2)
    pupil = s.material("pupil", (0.01, 0.01, 0.01), roughness=0.1)
    ey = s.layer()
    ir = s.layer()
    for ex, eyy, ez, rr in ((-1.2, -2.3, 2.0, 0.22), (1.4, -2.6, 2.4, 0.3), (0.3, -0.9, 3.0, 0.18),
                            (2.2, -1.4, 1.5, 0.16)):
        s.prim(ey, "sphere", (ex, eyy, ez), eyew, params=(rr,))
        s.prim(ir, "sphere", (ex, eyy, ez - rr * 0.62), iris, params=(rr * 0.45,))
        s.prim(ir, "sphere", (ex, eyy, ez - rr * 0.82), pupil, params=(rr * 0.22,))
    base = s.material("lamp_base", (0.75, 0.6, 0.3), roughness=0.2, metallic=1)
    goo = s.material("lava", (1, 0.4, 0.1), emissive_colour=(1, 0.35, 0.05), emissive_intensity=1.2)
    glass = s.material("lamp_glass", (0.9, 0.3, 0.6), roughness=0.05, emissive_colour=(0.9, 0.2, 0.5),
                       emissive_intensity=0.3)
    ll = s.layer()
    s.prim(ll, "capped_cone", (1.8, -0.2, 0.6), base, params=(0.2, 0.2, 0.1))
    s.prim(ll, "capped_cone", (1.8, -0.75, 0.6), glass, params=(0.35, 0.11, 0.07))
    s.prim(ll, "capped_cone", (1.8, -1.15, 0.6), base, params=(0.06, 0.07, 0.04))
    lv = s.layer(smooth=0.03)
    for dy, rr in ((-0.55, 0.06), (-0.85, 0.045), (-0.98, 0.035)):
        s.prim(lv, "sphere", (1.8, dy, 0.6), goo, params=(rr,))
    s.write()


# --- shadowdoorway -------------------------------------------------------
def shadowdoorway():
    s = Scene("shadowdoorway", 0.75,
              "Mel's Drake -- one too-tall shadow figure stooping in a doorway that\n"
              "stands alone on the white plane.")
    white = s.material("white", (0.97, 0.97, 0.96), roughness=1)
    frame = s.material("frame", (0.4, 0.4, 0.4), roughness=0.8)
    door = s.material("door", (0.55, 0.55, 0.54), roughness=0.7)
    shadow = s.material("shadow", (0.0, 0.0, 0.0), roughness=1)
    fl = s.layer()
    s.prim(fl, "box", (0, 0.05, 8), white, params=(30, 0.05, 30))
    fr = s.layer()
    for x in (-0.62, 0.62):
        s.prim(fr, "box", (x, -1.2, 4), frame, params=(0.08, 1.2, 0.12))
    s.prim(fr, "box", (0, -2.44, 4), frame, params=(0.7, 0.08, 0.12))
    # The door, swung open towards us on its left hinge.
    a = 1.45
    dl = s.layer()
    s.prim(dl, "box", (-0.55 + 0.53 * math.cos(a), -1.18, 4 - 0.53 * math.sin(a)), door,
           rot=(0, a, 0), params=(0.53, 1.15, 0.03))
    # Too tall for the frame, so it stoops through it.
    figure(s, 0.0, 4.15, 3.0, 0.0, 0.0, shadow, stoop=0.45, arm_length=1.3)
    # From behind it, so its shadow reaches out towards us.
    s.light("directional", (0.3, -0.6, 1), (1, 1, 1), 0.5)
    # Panels and a brass knob on the door, hinges on the frame, and the
    # dark it came out of behind the frame.
    brass = s.material("knob", (0.85, 0.65, 0.3), roughness=0.25, metallic=1)
    panel = s.material("panels", (0.48, 0.48, 0.47), roughness=0.7)
    dp = s.layer()
    hx, hz = -0.55, 4.0
    door_at = yawed((hx, 0, hz), a)
    for dy in (-1.75, -0.6):
        s.prim(dp, "box", door_at((0.53, dy, -0.035)), panel, rot=(0, a, 0), params=(0.4, 0.42, 0.012))
    s.prim(dp, "sphere", door_at((0.95, -1.05, -0.06)), brass, params=(0.045,))
    for dy in (-2.1, -0.35):
        s.prim(dp, "capped_cylinder", (-0.56, dy, 3.9), brass, params=(0.025, 0.07))
    void = s.material("void", (0, 0, 0), roughness=1)
    vd = s.layer()
    s.prim(vd, "box", (0, -1.2, 4.45), void, params=(0.55, 1.2, 0.12))
    s.write()


# --- corridor ------------------------------------------------------------
def corridor():
    s = Scene("corridor", 0.12,
              "A hospital corridor running on further than it should -- closed doors,\n"
              "strip lights, and dark at the end.")
    floor = s.material("floor", (0.9, 0.92, 0.9), base_map="hospitalfloor", uv_scale=1)
    wall = s.material("walls", (0.84, 0.89, 0.86), roughness=0.6)
    ceiling = s.material("ceiling", (0.9, 0.9, 0.9), base_map="ceilingtiles", uv_scale=1)
    strip = s.material("strip_light", (0.9, 1, 0.95), emissive_colour=(0.9, 1, 0.95),
                       emissive_intensity=2.0)
    door = s.material("doors", (0.62, 0.7, 0.68), roughness=0.5)
    rail = s.material("rail", (0.75, 0.77, 0.8), roughness=0.2, metallic=1)
    dark = s.material("end", (0.0, 0.0, 0.0), roughness=1)
    murk = s.material("murk", (0.0, 0.0, 0.0))
    c = s.layer()
    s.prim(c, "box", (0, 0.05, 25), floor, params=(1.5, 0.05, 30))
    for x in (-1.5, 1.5):
        s.prim(c, "box", (x, -1.45, 25), wall, params=(0.05, 1.45, 30))
    s.prim(c, "box", (0, -2.95, 25), ceiling, params=(1.55, 0.05, 30))
    s.prim(c, "box", (0, -1.45, 55), dark, params=(1.55, 1.5, 0.05))
    d = s.layer()
    s.prim(d, "box", (-1.44, -1.05, 26), door, params=(0.03, 1.05, 0.5),
           **limited((0, 0, 6), (1, 1, 9)))
    s.prim(d, "box", (1.44, -1.05, 29), door, params=(0.03, 1.05, 0.5),
           **limited((0, 0, 6), (1, 1, 8)))
    for x in (-1.41, 1.41):
        s.capsule_between(d, (x, -0.9, -5), (x, -0.9, 55), 0.025, rail)
    l = s.layer()
    s.prim(l, "box", (0, -2.885, 26), strip, params=(0.12, 0.015, 0.6),
           **limited((0, 0, 4), (1, 1, 14)))
    s.volumetric("box", (0, -1.45, 42), (0, 0, 0), (1.45, 1.45, 16), 0.35, murk)
    for z in (2, 10, 18):
        s.light("point", (0, -2.5, z), (0.85, 1, 0.92), 0.7)
    # A gurney left against the wall, a wheelchair, a fire extinguisher,
    # room numbers, and an exit sign hung from the ceiling.
    sheet = s.material("gurney_sheet", (0.95, 0.96, 0.97), roughness=1)
    chrome = s.material("chrome", (0.8, 0.82, 0.85), roughness=0.15, metallic=1)
    vinyl = s.material("wheelchair_seat", (0.1, 0.12, 0.15), roughness=0.6)
    red = s.material("extinguisher", (0.75, 0.05, 0.04), roughness=0.3)
    plate = s.material("room_numbers", (0.25, 0.4, 0.65), roughness=0.5)
    exit_sign = s.material("exit_sign", (0.2, 1, 0.4), emissive_colour=(0.2, 1, 0.4),
                           emissive_intensity=1.6, casts_shadow=0)
    gy = s.layer(smooth=0.02)
    s.prim(gy, "round_box", (-1.05, -0.85, 9.0), sheet, params=(0.32, 0.07, 1.0), extra=0.04)
    s.prim(gy, "box", (-1.05, -0.74, 9.0), chrome, params=(0.33, 0.02, 1.02))
    for gz in (8.15, 9.85):
        for gx in (-1.3, -0.8):
            s.capsule_between(gy, (gx, -0.74, gz), (gx, -0.1, gz), 0.02, chrome)
            s.prim(gy, "sphere", (gx, -0.06, gz), vinyl, params=(0.05,))
    wc = s.layer(smooth=0.01)
    wx, wz = 1.0, 15.0
    for side in (-0.3, 0.3):
        s.prim(wc, "torus", (wx + side, -0.32, wz), chrome, rot=(0, 0, math.pi / 2), params=(0.3, 0.02))
        s.capsule_between(wc, (wx + side, -0.55, wz + 0.25), (wx + side, -1.0, wz + 0.3), 0.015, chrome)
    s.prim(wc, "box", (wx, -0.55, wz), vinyl, params=(0.28, 0.02, 0.24))
    s.prim(wc, "box", (wx, -0.8, wz + 0.27), vinyl, params=(0.28, 0.22, 0.02))
    ex = s.layer(smooth=0.01)
    s.prim(ex, "capped_cylinder", (1.38, -0.75, 12.0), red, params=(0.08, 0.22))
    s.prim(ex, "capped_cylinder", (1.38, -1.0, 12.0), chrome, params=(0.035, 0.04))
    nm = s.layer()
    for k in range(9):
        s.prim(nm, "box", (-1.44, -2.3, 26 + 6 * k + 0.75), plate, params=(0.01, 0.07, 0.16))
    s.prim(nm, "box", (0, -2.7, 21), exit_sign, params=(0.35, 0.12, 0.03))
    s.write()


# --- hospitalbed ---------------------------------------------------------
def hospitalbed():
    s = Scene("hospitalbed", 0.45,
              "Diego, catatonic -- an empty hospital bed alone in a white void, an IV\n"
              "stand beside it.")
    white = s.material("white", (0.96, 0.96, 0.96), roughness=1)
    metal = s.material("frame", (0.75, 0.77, 0.8), roughness=0.25, metallic=1)
    sheets = s.material("sheets", (1, 1, 1), base_map="blueandwhitesheets", uv_scale=1)
    pillow = s.material("pillow", (0.97, 0.97, 0.97), roughness=0.9)
    blanket = s.material("blanket", (0.7, 0.8, 0.9), roughness=1)
    bag = s.material("iv_bag", (0.85, 0.9, 0.95), roughness=0.2)
    fl = s.layer()
    s.prim(fl, "box", (0, 0.05, 0), white, params=(20, 0.05, 20))
    b = s.layer()
    s.prim(b, "box", (0, -0.45, 0), metal, params=(0.5, 0.05, 1.05))
    for x in (-0.45, 0.45):
        for z in (-0.95, 0.95):
            s.capsule_between(b, (x, -0.06, z), (x, -0.45, z), 0.025, metal)
            s.prim(b, "sphere", (x, -0.05, z), metal, params=(0.05,))
    s.prim(b, "box", (0, -0.85, 1.08), metal, params=(0.52, 0.38, 0.03))
    s.prim(b, "box", (0, -0.68, -1.08), metal, params=(0.52, 0.22, 0.03))
    for x in (-0.53, 0.53):
        s.capsule_between(b, (x, -0.92, -0.45), (x, -0.92, 0.45), 0.03, metal)
        for z in (-0.4, 0.4):
            s.capsule_between(b, (x, -0.5, z), (x, -0.92, z), 0.03, metal)
    m = s.layer()
    s.prim(m, "round_box", (0, -0.62, 0), sheets, params=(0.47, 0.11, 1.0), extra=0.04)
    s.prim(m, "round_box", (0, -0.78, 0.78), pillow, params=(0.33, 0.06, 0.17), extra=0.05)
    s.prim(m, "round_box", (0, -0.76, -0.62), blanket, params=(0.46, 0.035, 0.28), extra=0.03)
    iv = s.layer()
    x, z = 0.85, 0.75
    for k in range(4):
        a = k / 4 * math.tau + 0.4
        s.capsule_between(iv, (x, -0.04, z), (x + 0.25 * math.cos(a), -0.03, z + 0.25 * math.sin(a)),
                          0.03, metal)
    s.capsule_between(iv, (x, -0.04, z), (x, -2.0, z), 0.025, metal)
    s.prim(iv, "torus", (x, -2.0, z), metal, rot=(math.pi / 2, 0, 0), params=(0.07, 0.02))
    s.prim(iv, "ellipsoid", (x, -1.8, z - 0.05), bag, params=(0.09, 0.14, 0.04))
    s.light("directional", (0.3, -1, -0.5), (1, 1, 1), 0.6)
    # A heart monitor on its stand, a bedside table with a cup and flowers
    # nobody's looked at, the catheter bag on the rail, a half-drawn curtain.
    case = s.material("monitor_case", (0.85, 0.86, 0.84), roughness=0.4)
    screen = s.material("monitor_screen", (0.02, 0.05, 0.04), roughness=0.1)
    trace = s.material("trace", (0.2, 1, 0.45), emissive_colour=(0.2, 1, 0.45),
                       emissive_intensity=2.0, casts_shadow=0)
    mn = s.layer()
    mx, mz = -0.95, -0.55
    s.capsule_between(mn, (mx, -0.02, mz), (mx, -1.35, mz), 0.025, metal)
    s.prim(mn, "round_box", (mx, -1.55, mz), case, rot=(0, 0.5, 0), params=(0.26, 0.2, 0.08), extra=0.02)
    s.prim(mn, "box", (mx + 0.04, -1.56, mz - 0.075), screen, rot=(0, 0.5, 0), params=(0.21, 0.15, 0.01))
    tr = s.layer()
    at = yawed((mx + 0.04, 0, mz - 0.088), 0.5)
    pts = [(-0.18, 0), (-0.06, 0), (-0.03, -0.08), (0.0, 0.06), (0.03, 0), (0.18, 0)]
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        s.capsule_between(tr, at((ax, -1.56 + ay, 0)), at((bx, -1.56 + by, 0)), 0.006, trace)
    table = s.material("bedside_table", (0.75, 0.7, 0.62), roughness=0.6)
    cupm = s.material("cup", (0.95, 0.95, 0.95), roughness=0.3)
    vase = s.material("vase", (0.6, 0.8, 0.85), roughness=0.05)
    stem = s.material("stems", (0.25, 0.45, 0.2), roughness=0.8)
    bloom = s.material("flowers", (0.95, 0.85, 0.3), roughness=0.6)
    tb = s.layer()
    s.prim(tb, "round_box", (-0.95, -0.35, 0.6), table, params=(0.25, 0.35, 0.22), extra=0.02)
    s.prim(tb, "capped_cone", (-1.05, -0.77, 0.52), cupm, params=(0.07, 0.04, 0.05))
    s.prim(tb, "capped_cylinder", (-0.85, -0.83, 0.68), vase, params=(0.05, 0.13))
    fw = s.layer(smooth=0.01)
    for k in range(5):
        a = k / 5 * math.tau
        tip = (-0.85 + 0.08 * math.cos(a), -1.15 - 0.04 * k % 3, 0.68 + 0.08 * math.sin(a))
        s.capsule_between(fw, (-0.85, -0.9, 0.68), tip, 0.006, stem)
        s.prim(fw, "sphere", tip, bloom, params=(0.035,))
    bag = s.material("catheter_bag", (0.95, 0.88, 0.55), roughness=0.15)
    cb = s.layer(smooth=0.01)
    s.prim(cb, "round_box", (0.58, -0.33, 0.1), bag, params=(0.012, 0.12, 0.09), extra=0.01)
    s.capsule_between(cb, (0.58, -0.45, 0.1), (0.45, -0.68, 0.2), 0.006, bag)
    curtain = s.material("curtain", (0.65, 0.78, 0.82), base_map="bluefabric", uv_scale=1.2)
    cr = s.layer(smooth=0.03)
    s.capsule_between(cr, (-1.6, -2.6, -1.4), (-1.6, -2.6, 1.6), 0.015, metal)
    for k in range(6):
        cz = 0.6 + 0.16 * k
        s.prim(cr, "box", (-1.6 + (0.04 if k % 2 else -0.04), -1.4, cz), curtain, params=(0.03, 1.2, 0.09))
    s.write()


# --- deity ---------------------------------------------------------------
def deity():
    s = Scene("deity", 0.1,
              "Theo, manic -- a glowing giant standing over a tiny town at night.")
    ground = s.material("ground", (0.06, 0.06, 0.07), roughness=1)
    house = s.material("houses", (0.32, 0.3, 0.32), roughness=1)
    roof = s.material("roofs", (0.2, 0.16, 0.16), roughness=1)
    lit = s.material("windows", (1, 0.85, 0.5), emissive_colour=(1, 0.8, 0.4),
                     emissive_intensity=1.2)
    glow = s.material("figure", (1, 0.96, 0.85), roughness=1,
                      emissive_colour=(1, 0.93, 0.75), emissive_intensity=0.55)
    g = s.layer()
    s.prim(g, "box", (0, 0.05, 10), ground, params=(40, 0.05, 30))
    h = s.layer()
    rf = s.layer()
    wn = s.layer()
    rng = random.Random(21)
    for gx in range(-9, 10):
        for gz in range(0, 9):
            x = gx * 0.95 + rng.uniform(-0.2, 0.2)
            z = 0.5 + gz * 1.5 + rng.uniform(-0.3, 0.3)
            if abs(x) < 1.6 and 9 < z < 14 or rng.random() < 0.3:
                continue
            w, t = rng.uniform(0.45, 0.7), rng.uniform(0.35, 0.65)
            gable_house(s, h, rf, x, z, w, t, rng.uniform(0.45, 0.7), house, roof)
            if rng.random() < 0.6:
                s.prim(wn, "box", (x + rng.uniform(-0.1, 0.1), -t * 0.55, z - 0.24), lit,
                       params=(0.06, 0.06, 0.02))
    figure(s, 0.0, 12.0, 9.0, 0.0, 0.0, glow, smooth=0.1, wide=True, arms_up=True)
    s.light("point", (0, -5, 10.5), (1, 0.9, 0.7), 2.5)
    s.light("directional", (0.2, -1, 0.5), (0.5, 0.55, 0.8), 0.15)
    # A halo behind its head; a road through town lit by streetlamps.
    halo = s.material("halo", (1, 0.95, 0.75), emissive_colour=(1, 0.9, 0.6),
                      emissive_intensity=1.4, casts_shadow=0)
    hl = s.layer()
    s.prim(hl, "torus", (0, -9.0 * 0.935, 12.4), halo, rot=(math.pi / 2, 0, 0), params=(1.1, 0.06))
    road = s.material("road", (0.1, 0.1, 0.11), roughness=0.8)
    lampm = s.material("streetlamps", (1, 0.8, 0.5), emissive_colour=(1, 0.75, 0.4),
                       emissive_intensity=1.5, casts_shadow=0)
    pole = s.material("lamp_poles", (0.15, 0.15, 0.16), roughness=0.5)
    rd = s.layer()
    s.prim(rd, "box", (0, -0.003, 6), road, params=(0.6, 0.004, 9))
    lp = s.layer()
    for k in range(6):
        lz = 0.6 + 2.0 * k
        for lx in (-0.75, 0.75):
            if k % 2 == (lx > 0):
                s.capsule_between(lp, (lx, 0, lz), (lx, -0.75, lz), 0.02, pole)
                s.prim(lp, "sphere", (lx, -0.78, lz), lampm, params=(0.045,))
    s.write()


# --- church --------------------------------------------------------------
def church():
    s = Scene("church", 0.08,
              "Pat's church -- pews down a dark nave, one high window, and a shaft of\n"
              "light falling through it.")
    floor = s.material("floor", (0.7, 0.68, 0.65), base_map="marble", uv_scale=0.5)
    stone = s.material("stone", (0.72, 0.68, 0.64), base_map="concrete_wall", uv_scale=0.4)
    wood = s.material("pews", (0.45, 0.28, 0.15), base_map="dark_brown_wood_texture_background_with_design_space",
                      uv_scale=0.8, roughness=0.6)
    cloth = s.material("altar_cloth", (0.95, 0.94, 0.9), roughness=1)
    gold = s.material("cross", (0.95, 0.75, 0.38), roughness=0.25, metallic=1)
    wax = s.material("candles", (0.95, 0.93, 0.85), roughness=0.8)
    beam = s.material("light_shaft", (1, 0.95, 0.82))
    w = s.layer()
    s.prim(w, "box", (0, 0.05, 6), floor, params=(5.3, 0.05, 12.5))
    # Left wall, built round the window gap (z 9.4-10.6, y -5.6..-8.4).
    s.prim(w, "box", (-5, -2.8, 6), stone, params=(0.3, 2.8, 12.5))
    s.prim(w, "box", (-5, -9.2, 6), stone, params=(0.3, 0.8, 12.5))
    s.prim(w, "box", (-5, -7, 1.45), stone, params=(0.3, 1.4, 7.95))
    s.prim(w, "box", (-5, -7, 14.55), stone, params=(0.3, 1.4, 3.95))
    s.prim(w, "box", (5, -5, 6), stone, params=(0.3, 5, 12.5))
    s.prim(w, "box", (0, -5, 18.2), stone, params=(5.3, 5, 0.3))
    s.prim(w, "box", (0, -10.2, 6), stone, params=(5.3, 0.3, 12.5))
    pl = s.layer()
    for x in (-3.6, 3.6):
        s.prim(pl, "capped_cylinder", (x, -5, 6), stone, params=(0.3, 5),
               **limited((0, 0, 4), (1, 1, 4)))
    pw = s.layer()
    for x in (-2.1, 2.1):
        s.prim(pw, "box", (x, -0.45, 4.5), wood, params=(1.5, 0.05, 0.24),
               **limited((0, 0, 1.4), (1, 1, 7)))
        s.prim(pw, "box", (x, -0.85, 4.75), wood, params=(1.5, 0.4, 0.035),
               **limited((0, 0, 1.4), (1, 1, 7)))
        for dx in (-1.45, 1.45):
            s.prim(pw, "box", (x + dx, -0.45, 4.6), wood, params=(0.04, 0.45, 0.3),
                   **limited((0, 0, 1.4), (1, 1, 7)))
    al = s.layer()
    s.prim(al, "box", (0, -0.55, 15), cloth, params=(1.4, 0.55, 0.6))
    # A wooden panel behind the altar, up to the cross.
    s.prim(al, "box", (0, -1.7, 17.75), wood, params=(1.8, 1.7, 0.12))
    s.prim(al, "box", (0, -4.6, 17.85), gold, params=(0.12, 1.3, 0.06))
    s.prim(al, "box", (0, -5.2, 17.85), gold, params=(0.65, 0.12, 0.06))
    # Unlit candles -- tiny emissive flames left a black ring round the
    # altar (each one shadowing its own light).
    for x in (-0.9, 0.9):
        s.prim(al, "capped_cylinder", (x, -1.35, 15), wax, params=(0.07, 0.25))
    # The shaft: from the window down to the floor before the altar,
    # along the same line as the sun.
    win, spot = (-5.0, -7.0, 10.0), (0.1, 0.0, 9.0)
    d = tuple(b - a for a, b in zip(win, spot))
    length = math.sqrt(sum(c * c for c in d))
    s.volumetric("box", tuple((a + b) / 2 for a, b in zip(win, spot)), euler_y_to(d),
                 (0.55, length / 2 + 0.3, 1.0), 0.22, beam)
    s.light("directional", tuple(-c for c in d), (1, 0.92, 0.75), 1.8)
    # A red runner up the aisle, hymn books along the pews, a lectern,
    # lamps hung on chains, and stained glass in the window.
    runner = s.material("runner", (0.5, 0.06, 0.06), roughness=1)
    books = s.material("hymn_books", (0.2, 0.08, 0.06), roughness=0.7)
    rn = s.layer()
    s.prim(rn, "box", (0, -0.006, 8.5), runner, params=(0.65, 0.006, 7.5))
    for x in (-2.1, 2.1):
        s.prim(rn, "box", (x, -0.92, 4.82), books, params=(0.06, 0.09, 0.035),
               **limited((0.45, 0, 1.4), (5, 1, 7)))
    lec = s.layer(smooth=0.02)
    s.prim(lec, "box", (2.3, -0.6, 13.6), wood, params=(0.25, 0.6, 0.2))
    s.prim(lec, "box", (2.3, -1.25, 13.5), wood, rot=(-0.4, 0, 0), params=(0.35, 0.03, 0.25))
    s.prim(lec, "box", (2.3, -1.29, 13.48), cloth, rot=(-0.4, 0, 0), params=(0.22, 0.012, 0.15))
    glow = s.material("lamps", (1, 0.8, 0.5), emissive_colour=(1, 0.75, 0.45),
                      emissive_intensity=1.0, casts_shadow=0)
    chain = s.material("chains", (0.3, 0.28, 0.25), roughness=0.4, metallic=1)
    lm = s.layer()
    for lx in (-2.1, 2.1):
        for lz in (5.0, 11.0):
            s.capsule_between(lm, (lx, -9.9, lz), (lx, -4.3, lz), 0.02, chain)
            s.prim(lm, "capped_cone", (lx, -4.1, lz), chain, params=(0.18, 0.08, 0.22))
            s.prim(lm, "sphere", (lx, -3.95, lz), glow, params=(0.09,))
    stained = [s.material(f"glass{i}", c, emissive_colour=c, emissive_intensity=0.6, casts_shadow=0)
               for i, c in enumerate(((0.8, 0.15, 0.15), (0.2, 0.35, 0.85), (0.9, 0.75, 0.2),
                                      (0.2, 0.6, 0.3)))]
    sg = s.layer()
    for k, (dy, dz) in enumerate(((-0.7, -0.3), (-0.7, 0.3), (0.7, -0.3), (0.7, 0.3))):
        s.prim(sg, "box", (-5.25, -7.0 + dy, 10.0 + dz), stained[k], params=(0.02, 0.65, 0.27))
    s.write()


# --- phone ---------------------------------------------------------------
def phone():
    s = Scene("phone", 0.06,
              "Alex outside the club -- a phone face up on wet pavement, twelve missed\n"
              "calls, the club's doorway glowing behind.")
    wet = s.material("pavement", (0.12, 0.12, 0.14), roughness=0.1,
                     bump_map="concrete_wall", bump_strength=1)
    puddle = s.material("puddle", (0.09, 0.09, 0.11), roughness=0.02)
    body = s.material("phone", (0.03, 0.03, 0.035), roughness=0.2)
    screen = s.material("screen", (0.7, 0.85, 1), emissive_colour=(0.7, 0.85, 1),
                        emissive_intensity=1.4)
    alert = s.material("missed_calls", (1, 0.2, 0.2), emissive_colour=(1, 0.2, 0.2),
                       emissive_intensity=1.8)
    wall = s.material("club_wall", (0.2, 0.17, 0.18), base_map="dirtywall", uv_scale=0.5)
    door = s.material("doorway", (1, 0.25, 0.7), emissive_colour=(1, 0.2, 0.7),
                      emissive_intensity=2.0)
    neon = s.material("neon", (0.3, 0.9, 1), emissive_colour=(0.3, 0.9, 1),
                      emissive_intensity=2.5)
    paper = s.material("cigarette", (0.95, 0.95, 0.92), roughness=0.9)
    ember = s.material("ember", (1, 0.4, 0.1), emissive_colour=(1, 0.35, 0.05),
                       emissive_intensity=2.0)
    g = s.layer()
    s.prim(g, "box", (0, 0.1, 4), wet, params=(20, 0.1, 20))
    pd = s.layer()
    s.prim(pd, "ellipsoid", (-0.9, -0.005, 0.7), puddle, params=(0.8, 0.012, 0.45))
    s.prim(pd, "ellipsoid", (1.1, -0.005, 1.8), puddle, params=(0.6, 0.012, 0.35))
    yaw = 0.35
    at = yawed((0, 0, 1.2), yaw)
    ph = s.layer()
    s.prim(ph, "round_box", at((0, -0.018, 0)), body, rot=(0, yaw, 0),
           params=(0.15, 0.018, 0.31), extra=0.03)
    sc = s.layer()
    s.prim(sc, "box", at((0, -0.037, -0.01)), screen, rot=(0, yaw, 0), params=(0.13, 0.002, 0.27))
    for k in range(4):
        s.prim(sc, "box", at((0, -0.04, 0.18 - k * 0.075)), alert, rot=(0, yaw, 0),
               params=(0.11, 0.002, 0.026))
    cg = s.layer()
    s.capsule_between(cg, (0.45, -0.02, 0.95), (0.62, -0.02, 1.05), 0.018, paper)
    s.prim(cg, "sphere", (0.45, -0.02, 0.95), ember, params=(0.02,))
    w = s.layer()
    s.prim(w, "box", (0, -3, 6.3), wall, params=(12, 3, 0.3))
    dr = s.layer()
    s.prim(dr, "box", (1.6, -1.3, 5.98), door, params=(0.75, 1.3, 0.03))
    s.capsule_between(dr, (0.9, -3.0, 5.95), (2.3, -3.0, 5.95), 0.05, neon)
    s.capsule_between(dr, (0.9, -3.4, 5.95), (2.3, -3.4, 5.95), 0.05, neon)
    s.light("point", (1.6, -1.2, 5.0), (1, 0.3, 0.75), 2.2)
    s.light("point", (1.6, -3.2, 5.3), (0.3, 0.9, 1), 0.9)
    s.light("point", (0, -0.6, 1.2), (0.7, 0.85, 1), 0.25)
    # The camera bump showing past the edge, brass stanchions and a red rope
    # by the club door, a crushed plastic cup and old cigarette butts.
    brass = s.material("stanchions", (0.9, 0.7, 0.35), roughness=0.2, metallic=1)
    rope = s.material("velvet_rope", (0.55, 0.03, 0.08), roughness=0.8)
    st = s.layer(smooth=0.01)
    posts = [(0.4, 5.3), (1.6, 5.0), (2.8, 5.3)]
    for px, pz in posts:
        s.prim(st, "capped_cylinder", (px, -0.03, pz), brass, params=(0.16, 0.03))
        s.capsule_between(st, (px, -0.05, pz), (px, -0.95, pz), 0.03, brass)
        s.prim(st, "sphere", (px, -1.0, pz), brass, params=(0.055,))
    rp = s.layer()
    for (ax, az), (bx, bz) in zip(posts, posts[1:]):
        pts = [(ax + (bx - ax) * u, -0.9 + 0.25 * 4 * u * (1 - u), az + (bz - az) * u) for u in
               (i / 6 for i in range(7))]
        for a, b in zip(pts, pts[1:]):
            s.capsule_between(rp, a, b, 0.03, rope)
    litter = s.layer()
    cupm = s.material("plastic_cup", (0.85, 0.2, 0.2), roughness=0.3)
    s.prim(litter, "capped_cone", (-1.4, -0.06, 2.4), cupm, rot=(math.pi / 2, 0.8, 0), params=(0.12, 0.05, 0.07))
    for bx, bz, by in ((0.8, 1.6, 0.3), (-0.4, 2.1, 1.9), (0.2, 2.6, 0.9)):
        s.capsule_between(litter, (bx, -0.015, bz), (bx + 0.08 * math.cos(by), -0.015, bz + 0.08 * math.sin(by)),
                          0.014, paper)
    s.write()


# --- livingroom ----------------------------------------------------------
def livingroom():
    s = Scene("livingroom", 0.1,
              "Theresa's living room -- his empty armchair, a lamp left on, the piano.")
    floor = s.material("floor", (0.8, 0.7, 0.6), base_map="woodfloor", uv_scale=1)
    rug = s.material("rug", (0.9, 0.75, 0.75), base_map="floral2", uv_scale=0.7)
    paper = s.material("wallpaper", (0.9, 0.85, 0.75), base_map="stainedwallpaper", uv_scale=1)
    ceil = s.material("ceiling", (0.9, 0.9, 0.88), base_map="popcornceiling", uv_scale=1)
    velvet = s.material("armchair", (0.27, 0.38, 0.3), roughness=0.9)
    piano = s.material("piano", (0.35, 0.22, 0.14), base_map="dark_brown_wood_texture_background_with_design_space",
                       uv_scale=0.8, roughness=0.35)
    ivory = s.material("keys", (0.95, 0.93, 0.88), roughness=0.3)
    ebony = s.material("black_keys", (0.03, 0.03, 0.03), roughness=0.3)
    # The lamp's light sits right by its pole and shade, so neither casts a
    # shadow -- from that close they streak the whole room.
    brass = s.material("lamp_stand", (0.85, 0.66, 0.35), roughness=0.25, metallic=1,
                       casts_shadow=0)
    shade = s.material("lamp_shade", (1, 0.88, 0.7), emissive_colour=(1, 0.78, 0.5),
                       emissive_intensity=0.8, casts_shadow=0)
    photo = s.material("photo", (1, 1, 1), base_map="family", uv_scale=3.5)
    r = s.layer()
    s.prim(r, "box", (0, 0.05, 0), floor, params=(5, 0.05, 5))
    s.prim(r, "box", (0, -1.5, 3.6), paper, params=(5, 1.5, 0.1))
    s.prim(r, "box", (-3.6, -1.5, 0), paper, params=(0.1, 1.5, 5))
    s.prim(r, "box", (3.6, -1.5, 0), paper, params=(0.1, 1.5, 5))
    s.prim(r, "box", (0, -3.05, 0), ceil, params=(5, 0.05, 5))
    rg = s.layer()
    s.prim(rg, "box", (-0.3, -0.006, 1.2), rug, params=(1.7, 0.006, 1.2))
    # The armchair, turned a little towards the room.
    yaw = 0.4
    at = yawed((-0.9, 0, 1.6), yaw)
    ch = s.layer(smooth=0.03)
    for local, half, rad in (((0, -0.2, 0), (0.48, 0.18, 0.45), 0.05),
                             ((0, -0.45, -0.02), (0.42, 0.08, 0.4), 0.06),
                             ((0, -0.88, 0.36), (0.48, 0.45, 0.1), 0.08),
                             ((-0.47, -0.62, 0), (0.1, 0.2, 0.45), 0.06),
                             ((0.47, -0.62, 0), (0.1, 0.2, 0.45), 0.06)):
        s.prim(ch, "round_box", at(local), velvet, rot=(0, yaw, 0), params=half, extra=rad)
    # The standard lamp beside it, still on.
    lx, lz = 0.05, 2.3
    lp = s.layer()
    s.prim(lp, "capped_cylinder", (lx, -0.02, lz), brass, params=(0.16, 0.02))
    s.capsule_between(lp, (lx, -0.03, lz), (lx, -1.5, lz), 0.018, brass)
    sh = s.layer()
    s.prim(sh, "capped_cone", (lx, -1.62, lz), shade, params=(0.17, 0.12, 0.24))
    # The upright piano on the right, a photo on top.
    px, pz = 2.0, 3.15
    pn = s.layer()
    s.prim(pn, "box", (px, -0.65, pz), piano, params=(0.78, 0.65, 0.3))
    s.prim(pn, "box", (px, -1.33, pz), piano, params=(0.8, 0.03, 0.33))
    s.prim(pn, "box", (px, -0.72, pz - 0.45), piano, params=(0.76, 0.05, 0.17))
    for dx in (-0.73, 0.73):
        s.capsule_between(pn, (px + dx, -0.02, pz - 0.5), (px + dx, -0.7, pz - 0.5), 0.03, piano)
    ky = s.layer()
    s.prim(ky, "box", (px, -0.785, pz - 0.5), ivory, params=(0.72, 0.012, 0.11))
    s.prim(ky, "box", (px, -0.805, pz - 0.44), ebony, params=(0.011, 0.012, 0.06),
           **limited((0.057, 0, 0), (25, 1, 1)))
    s.prim(ky, "box", (px - 0.3, -1.5, pz + 0.05), photo, rot=(0, -0.2, 0),
           params=(0.15, 0.13, 0.015))
    # Just under the shade: inside it, the shade shadows its own light.
    s.light("point", (lx, -1.3, lz - 0.1), (1, 0.78, 0.5), 1.4)
    s.light("directional", (0.5, -0.6, -1), (0.6, 0.65, 0.8), 0.15)
    # His reading glasses left on the seat, his slippers in front of the
    # chair, a cold cup of tea on a side table, a clock, a curtained window,
    # books stacked on the piano.
    frames = s.material("glasses", (0.15, 0.1, 0.08), roughness=0.3)
    gl = s.layer()
    for dx in (-0.08, 0.08):
        s.prim(gl, "torus", at((dx, -0.54, -0.1)), frames, params=(0.065, 0.012))
    s.capsule_between(gl, at((-0.015, -0.54, -0.1)), at((0.015, -0.54, -0.1)), 0.01, frames)
    slipper = s.material("slippers", (0.35, 0.25, 0.22), roughness=1)
    sl = s.layer(smooth=0.02)
    for dx in (-0.14, 0.14):
        s.prim(sl, "ellipsoid", at((dx, -0.04, -0.75)), slipper, rot=(0, yaw + dx, 0),
               params=(0.09, 0.05, 0.17))
    table = s.material("side_table", (0.4, 0.26, 0.16), base_map="dark_brown_wood_texture_background_with_design_space",
                       uv_scale=0.8, roughness=0.4)
    china = s.material("teacup", (0.96, 0.95, 0.92), roughness=0.15)
    tea = s.material("tea", (0.4, 0.2, 0.08), roughness=0.05)
    st = s.layer()
    tx, tz = -1.85, 1.0
    s.prim(st, "capped_cylinder", (tx, -0.62, tz), table, params=(0.3, 0.025))
    s.capsule_between(st, (tx, -0.6, tz), (tx, -0.02, tz), 0.04, table)
    s.prim(st, "capped_cylinder", (tx, -0.02, tz), table, params=(0.2, 0.02))
    s.prim(st, "capped_cylinder", (tx + 0.05, -0.655, tz - 0.05), china, params=(0.11, 0.008))
    s.prim(st, "capped_cone", (tx + 0.05, -0.71, tz - 0.05), china, params=(0.05, 0.05, 0.07))
    s.prim(st, "torus", (tx + 0.13, -0.72, tz - 0.05), china, rot=(math.pi / 2, 0, 0), params=(0.03, 0.01))
    s.prim(st, "capped_cylinder", (tx + 0.05, -0.75, tz - 0.05), tea, params=(0.062, 0.004))
    clock = s.material("clock_face", (0.95, 0.93, 0.85), roughness=0.3)
    hands = s.material("clock_hands", (0.05, 0.05, 0.05), roughness=0.4)
    ck = s.layer()
    s.prim(ck, "capped_cylinder", (-0.6, -2.2, 3.48), clock, rot=(math.pi / 2, 0, 0), params=(0.22, 0.02))
    s.prim(ck, "torus", (-0.6, -2.2, 3.47), brass, rot=(math.pi / 2, 0, 0), params=(0.22, 0.025))
    s.capsule_between(ck, (-0.6, -2.2, 3.45), (-0.6, -2.36, 3.45), 0.01, hands)
    s.capsule_between(ck, (-0.6, -2.2, 3.45), (-0.5, -2.15, 3.45), 0.012, hands)
    night = s.material("night_window", (0.25, 0.35, 0.6), emissive_colour=(0.2, 0.3, 0.55),
                       emissive_intensity=0.5, casts_shadow=0)
    drape = s.material("curtains", (0.55, 0.2, 0.2), base_map="floral6", uv_scale=0.8)
    wn = s.layer()
    s.prim(wn, "box", (-3.48, -1.7, 0.3), night, params=(0.02, 0.7, 0.8))
    for dz in (-0.95, 0.95):
        s.prim(wn, "box", (-3.42, -1.55, 0.3 + dz), drape, params=(0.05, 1.1, 0.28))
    bk = s.layer()
    for k, c in enumerate(((0.5, 0.15, 0.1), (0.15, 0.3, 0.2), (0.2, 0.2, 0.4))):
        bm = s.material(f"book{k}", c, roughness=0.7)
        s.prim(bk, "box", (px + 0.4, -1.39 - 0.07 * k, pz), bm, rot=(0, 0.1 * k, 0),
               params=(0.17, 0.035, 0.12))
    s.write()



# --- scars_*: chapter 3's "where do you self-harm?" close-ups -------------
# The script asks for "a close up of all the characters' scars". The room
# models are featureless, so each answer cuts to its own close-up of that
# person's body instead -- skin carrying one of the scar diffuse/bump pairs
# in assets/textures/, against black. Built ~3x life size so the skin's
# curve resolves smoothly (see ChapterStaging::world_scale's comment on the
# field's fixed voxel size). The skin tones are a first guess -- the
# models never set one -- so change them freely.
# (A directional light's vector points TOWARDS the light, so -y is from
# above and -z from the camera's side.)
def skin(s, tone, scars, uv_scale=1.2):
    return s.material("skin", tone, base_map=f"{scars}_diffuse",
                      bump_map=f"{scars}_bump", bump_strength=2.0,
                      uv_scale=uv_scale, roughness=0.6)


def scars_stomach():
    s = Scene("scars_stomach", 0.12,
              "Mel's stomach -- her shirt pulled up, the scars across her belly.")
    sk = skin(s, (0.95, 0.8, 0.7), "15_heavy_two_massive", uv_scale=1.6)
    shirt = s.material("shirt", (0.32, 0.34, 0.38), roughness=1)
    jeans = s.material("jeans", (0.2, 0.26, 0.38), base_map="bluefabric", uv_scale=0.8)
    # Upright torso, hips at y = -0.8, ribs at y = -1.6.
    b = s.layer(smooth=0.12)
    s.prim(b, "ellipsoid", (0, -1.2, 0), sk, params=(0.5, 0.6, 0.3))
    s.prim(b, "ellipsoid", (0, -0.85, 0.02), sk, params=(0.56, 0.3, 0.32))
    cut = s.layer("subtraction", 0.03)
    s.prim(cut, "sphere", (0, -1.05, -0.31), sk, params=(0.035,))
    # The shirt rolled up under the ribs, the jeans' waistband at the hips.
    # The shirt bunched up under her ribs, held there.
    c = s.layer(smooth=0.04)
    s.prim(c, "ellipsoid", (0, -1.66, 0.0), shirt, params=(0.53, 0.11, 0.33))
    s.prim(c, "ellipsoid", (0, -1.92, 0.01), shirt, params=(0.52, 0.26, 0.32))
    j = s.layer(smooth=0.02)
    s.prim(j, "ellipsoid", (0, -0.62, 0.02), jeans, params=(0.62, 0.16, 0.36))
    s.prim(j, "ellipsoid", (0, -0.35, 0.02), jeans, params=(0.64, 0.3, 0.38))
    s.light("directional", (0.4, -0.5, -1), (1, 0.95, 0.88), 0.5)
    s.light("directional", (-0.7, -0.3, -0.6), (0.85, 0.9, 1), 0.15)
    # Her hands holding the hem up, forearms coming in from the sides.
    hands = s.layer(smooth=0.03)
    for sgn in (-1, 1):
        wrist = (sgn * 0.5, -1.58, -0.22)
        round_cone_between(s, hands, (sgn * 0.85, -1.0, 0.1), wrist, 0.075, 0.055, sk)
        hand(s, hands, wrist, norm((-sgn * 0.5, -0.6, -0.4)), norm((0, -1, 0.6)),
             norm((sgn * 0.6, 0.2, -0.75)), 0.13, sk, curl=0.8)
    # Brass button, belt loops and fly on the jeans.
    brass = s.material("button", (0.85, 0.62, 0.3), roughness=0.25, metallic=1)
    bt = s.layer()
    s.prim(bt, "capped_cylinder", (0, -0.66, -0.37), brass, rot=(math.pi / 2, 0, 0), params=(0.03, 0.01))
    for x in (-0.32, 0.32):
        s.prim(bt, "box", (x, -0.65, -0.34), jeans, rot=(0, x * 0.8, 0), params=(0.025, 0.07, 0.02))
    s.prim(bt, "box", (0.05, -0.42, -0.375), jeans, params=(0.004, 0.18, 0.008))
    s.write()


def limb_scene(name, comment, tone, scars, cloth_mat, ground):
    """A scene with skin, one cloth material and a ground slab under y = 0;
    cloth_mat and ground are (key, colour, material properties). Returns
    the scene and its skin and cloth materials for the caller to build
    the limb from."""
    s = Scene(name, 0.15, comment)
    sk = skin(s, tone, scars)
    cloth = s.material(cloth_mat[0], cloth_mat[1], **cloth_mat[2])
    gr = s.material(ground[0], ground[1], **ground[2])
    g = s.layer()
    s.prim(g, "box", (0, 0.05, 0), gr, params=(6, 0.05, 6))
    return s, sk, cloth


def scars_forearm():
    s, sk, cloth = limb_scene(
        "scars_forearm", "Diego's forearm, on his bed -- sleeve rolled to the elbow.",
        (0.8, 0.6, 0.45), "18_severe_mixed_chaos",
        ("sleeve", (0.72, 0.8, 0.92), {"roughness": 1}),
        ("sheet", (1, 1, 1), {"base_map": "blueandwhitesheets", "uv_scale": 2.5}))
    a = s.layer(smooth=0.08)
    # Elbow at -x, wrist at +x, inside of the arm facing up.
    s.capsule_between(a, (-0.9, -0.2, 0), (0.55, -0.15, 0), 0.19, sk)
    s.prim(a, "ellipsoid", (-0.55, -0.22, 0), sk, params=(0.4, 0.22, 0.24))
    # The hand, palm up, fingers loosely curled.
    hand(s, a, (0.62, -0.15, 0), (1, 0, 0), (0, 0, 1), (0, 1, 0), 0.42, sk, curl=0.45)
    # The shirt sleeve, rolled.
    c = s.layer(smooth=0.03)
    s.prim(c, "torus", (-1.05, -0.22, 0), cloth, rot=(0, 0, math.pi / 2), params=(0.26, 0.09))
    s.capsule_between(c, (-1.1, -0.22, 0), (-2.4, -0.25, 0), 0.27, cloth)
    s.light("directional", (0.3, -1, -0.5), (1, 0.97, 0.92), 0.5)
    s.light("point", (0.5, -1.4, -1.2), (0.95, 0.95, 1), 0.6)
    # His watch, and two plasters over the newest cuts.
    steel = s.material("watch", (0.8, 0.8, 0.82), roughness=0.18, metallic=1)
    plaster = s.material("plasters", (0.86, 0.68, 0.52), roughness=0.8)
    wt = s.layer()
    s.prim(wt, "torus", (0.42, -0.16, 0), steel, rot=(0, 0, math.pi / 2), params=(0.19, 0.035))
    pl = s.layer()
    for px, pz, yaw in ((-0.15, 0.03, 0.3), (0.2, -0.05, -0.4)):
        s.prim(pl, "round_box", (px, -0.38, pz), plaster, rot=(0, yaw, 0),
               params=(0.11, 0.012, 0.04), extra=0.008)
    s.write()


def scars_wrist():
    s, sk, cloth = limb_scene(
        "scars_wrist", "Alex's wrists, turned up -- every inch of them.",
        (0.92, 0.75, 0.64), "20_severe_maximal",
        ("bracelet", (0.1, 0.1, 0.1), {"roughness": 0.3}),
        ("tiles", (0.85, 0.88, 0.9), {"base_map": "hospitalfloor", "uv_scale": 1.2}))
    # Both forearms side by side, wrists towards the camera (-z).
    a = s.layer(smooth=0.08)
    for x in (-0.3, 0.3):
        s.capsule_between(a, (x * 1.4, -0.2, 1.4), (x, -0.17, 0.0), 0.17, sk)
        # Palm up, thumb out to the side.
        hand(s, a, (x, -0.16, -0.08), (0, 0, -1), (1 if x < 0 else -1, 0, 0), (0, 1, 0),
             0.36, sk, curl=0.5)
    c = s.layer()
    s.prim(c, "torus", (-0.3, -0.17, 0.08), cloth, rot=(math.pi / 2, 0, 0), params=(0.18, 0.02))
    s.light("directional", (0.2, -1, -0.6), (1, 0.97, 0.94), 0.5)
    s.light("point", (0, -1.2, -1.0), (0.95, 0.95, 1), 0.5)
    s.write()


def thighs(s, sk, hem, hem_smooth, y=-0.35):
    """Two thighs seen from above, sat down: hips at +z, knees at -z, the
    shorts'/skirt's hem (material hem) over the hips."""
    a = s.layer(smooth=0.1)
    for x in (-0.36, 0.36):
        s.capsule_between(a, (x, y, 0.9), (x * 0.9, y, -0.7), 0.32, sk)
        s.prim(a, "sphere", (x * 0.9, y + 0.02, -0.82), sk, params=(0.27,))
    h = s.layer(smooth=hem_smooth)
    for x in (-0.36, 0.36):
        s.capsule_between(h, (x, y - 0.02, 1.0), (x, y - 0.02, 1.6), 0.37, hem)


def scars_thigh():
    s = Scene("scars_thigh", 0.12,
              "Theo's thighs, sat on the toilet lid -- where nobody else sees.")
    sk = skin(s, (0.7, 0.5, 0.36), "12_heavy_few_large_fresh")
    shorts = s.material("boxers", (0.15, 0.2, 0.3), base_map="Stripes", uv_scale=0.6)
    seat = s.material("seat", (0.95, 0.95, 0.95), roughness=0.2)
    floor = s.material("floor", (0.7, 0.72, 0.72), base_map="hospitalfloor", uv_scale=1)
    f = s.layer()
    s.prim(f, "box", (0, 1.45, 0), floor, params=(6, 0.05, 6))
    # The toilet: lid down, the bowl under it, a cistern behind.
    t = s.layer(smooth=0.04)
    s.prim(t, "round_box", (0, 0.0, 1.1), seat, params=(0.7, 0.04, 0.8), extra=0.04)
    s.prim(t, "ellipsoid", (0, 0.7, 1.15), seat, params=(0.5, 0.7, 0.62))
    s.prim(t, "round_box", (0, -0.6, 2.05), seat, params=(0.75, 0.55, 0.25), extra=0.05)
    thighs(s, sk, shorts, 0.05, y=-0.36)
    # Directional only: a point light this close blotches the skin with
    # its shadows.
    s.light("directional", (0.2, -1, -0.4), (1, 0.98, 0.95), 0.7)
    s.light("directional", (-0.5, -0.6, -0.6), (0.9, 0.95, 1), 0.3)
    # Shins down from the knees to bare feet on the tiles.
    lg = s.layer(smooth=0.1)
    for x in (-0.36, 0.36):
        round_cone_between(s, lg, (x * 0.9, -0.34, -0.82), (x * 0.95, 1.2, -0.95), 0.26, 0.15, sk)
        s.prim(lg, "ellipsoid", (x, 1.3, -1.2), sk, params=(0.15, 0.1, 0.32))
    s.write()


def scars_thigh_old():
    s = Scene("scars_thigh_old", 0.1,
              "Theresa's thighs, her skirt hitched up, in the armchair -- old scars and new.")
    sk = skin(s, (0.93, 0.8, 0.72), "10_moderate_three_large")
    skirt = s.material("skirt", (1, 1, 1), base_map="floral5", uv_scale=0.6)
    velvet = s.material("armchair", (0.27, 0.38, 0.3), roughness=0.9)
    floor = s.material("carpet", (0.6, 0.45, 0.4), base_map="floral2", uv_scale=1.5)
    f = s.layer()
    s.prim(f, "box", (0, 1.65, 0), floor, params=(6, 0.05, 6))
    c = s.layer(smooth=0.04)
    s.prim(c, "round_box", (0, 0.0, 0.5), velvet, params=(1.1, 0.06, 1.5), extra=0.05)
    s.prim(c, "round_box", (0, 0.8, 0.6), velvet, params=(1.15, 0.8, 1.4), extra=0.08)
    for x in (-1.15, 1.15):
        s.prim(c, "round_box", (x, -0.3, 0.6), velvet, params=(0.18, 0.4, 1.4), extra=0.08)
    thighs(s, sk, skirt, 0.08, y=-0.38)
    s.light("directional", (0.6, -0.8, -0.3), (1, 0.82, 0.6), 0.8)
    s.light("directional", (-0.4, -1, -0.2), (0.7, 0.75, 0.85), 0.25)
    # Shins down to her slippers; her hand resting on her thigh, wedding
    # ring on, cardigan sleeve to the wrist.
    lg = s.layer(smooth=0.1)
    for x in (-0.36, 0.36):
        round_cone_between(s, lg, (x * 0.9, -0.36, -0.82), (x * 0.95, 1.35, -1.0), 0.26, 0.15, sk)
    slipper = s.material("slippers", (0.85, 0.6, 0.65), roughness=1)
    sl = s.layer(smooth=0.04)
    for x in (-0.36, 0.36):
        s.prim(sl, "ellipsoid", (x, 1.45, -1.25), slipper, params=(0.2, 0.14, 0.38))
    wool = s.material("cardigan", (0.45, 0.32, 0.4), roughness=1, bump_map="concrete_wall",
                      bump_strength=0.6)
    gold = s.material("ring", (0.95, 0.75, 0.35), roughness=0.15, metallic=1)
    hd = s.layer(smooth=0.03)
    wrist = (0.62, -0.74, 0.2)
    along = norm((-0.35, 0.02, -1))
    round_cone_between(s, hd, (1.3, -1.0, 1.1), wrist, 0.11, 0.08, sk)
    hand(s, hd, wrist, along, (-1, 0, 0.35), (0, -1, 0), 0.3, sk, curl=0.25)
    sv = s.layer(smooth=0.02)
    round_cone_between(s, sv, (1.4, -1.05, 1.25), (0.72, -0.77, 0.45), 0.15, 0.12, wool)
    rg = s.layer()
    across = norm((-1, 0, 0.35))
    ring_at = tuple(w + 0.3 * 1.02 * a + 0.1 * 0.3 * c - 0.01 for w, a, c in zip(wrist, along, across))
    s.prim(rg, "torus", ring_at, gold, rot=euler_y_to(along), params=(0.036, 0.011))
    s.write()


# --- blades --------------------------------------------------------------
def tip_cutter(layer, s, mat, heel, tip, spine_side, y, thickness=0.05):
    """A box cutting a straight taper from heel (on the spine) down to tip
    (on the edge) -- spine_side is +1/-1, which side of that line to cut."""
    dx, dz = tip[0] - heel[0], tip[1] - heel[1]
    length = math.hypot(dx, dz)
    ux, uz = dx / length, dz / length
    nx, nz = -uz * spine_side, ux * spine_side
    depth = 0.4
    centre = ((heel[0] + tip[0]) / 2 + nx * depth, y, (heel[1] + tip[1]) / 2 + nz * depth)
    s.prim(layer, "box", centre, mat, rot=(0, math.atan2(-uz, ux), 0),
           params=(length / 2 + 0.3, thickness, depth))


def blades():
    s = Scene("blades", 0.28,
              "\"How do you self-harm?\" -- what they each use, laid out on black\n"
              "cloth: a kitchen knife, a pocket knife, razors, and a broken blue and\n"
              "white plate. One set, framed per answer by kCutaways' blades_* ids.")
    steel = s.material("steel", (0.8, 0.81, 0.83), roughness=0.22, metallic=1)
    edge = s.material("edge", (0.9, 0.9, 0.92), roughness=0.08, metallic=1)
    china = s.material("china", (1, 1, 1), base_map="china_blue", uv_scale=1.1,
                       uv_offset=(0.55 - 2.0, 0, 0.55 - 0.1), roughness=0.15)
    handle = s.material("handle", (0.3, 0.18, 0.1), base_map="brownwoodtexture", uv_scale=0.5)
    red = s.material("pocket_knife", (0.6, 0.06, 0.05), roughness=0.3)
    rivet = s.material("rivets", (0.85, 0.85, 0.82), roughness=0.2, metallic=1)
    plastic = s.material("razor", (0.15, 0.35, 0.75), roughness=0.4)
    cloth = s.material("cloth", (0.05, 0.05, 0.06), roughness=1)
    # Everything the cutters below shape comes first -- a subtraction
    # layer cuts every layer before it, so the cloth comes after.
    y = -0.025  # blades lie flat, resting on the cloth's top at y = 0
    b = s.layer()
    # Kitchen knife: blade along +x from its heel at x = -1.75.
    s.prim(b, "box", (-1.25, y, 0.0), steel, params=(0.5, 0.022, 0.12))
    # Its ground edge: a brighter bevel along the cutting side, which the
    # tip cutter below trims along with the blade.
    s.prim(b, "box", (-1.25, y - 0.012, -0.095), edge, rot=(0.35, 0, 0), params=(0.5, 0.01, 0.03))
    # Pocket knife blade, open, at z = 1.0.
    s.prim(b, "box", (-0.05, y + 0.005, 1.0), steel, params=(0.24, 0.02, 0.065))
    s.prim(b, "box", (-0.05, y - 0.007, 0.948), edge, rot=(0.35, 0, 0), params=(0.24, 0.009, 0.018))
    # Double-edged razor blade.
    s.prim(b, "box", (0.3, -0.016, -0.75), steel, rot=(0, 0.3, 0), params=(0.2, 0.016, 0.11))
    # The plate, centre (2.0, 0.1) -- see china's uv_offset.
    s.prim(b, "capped_cylinder", (2.0, -0.03, 0.1), china, params=(0.55, 0.03))
    s.prim(b, "torus", (2.0, -0.06, 0.1), china, params=(0.53, 0.035))
    c = s.layer("subtraction", 0.005)
    tip_cutter(c, s, steel, (-1.2, 0.12), (-0.75, -0.11), 1, y)
    tip_cutter(c, s, steel, (0.0, 1.065), (0.19, 0.935), 1, y)
    # The razor's slot and end holes.
    s.prim(c, "box", (0.3, -0.016, -0.75), steel, rot=(0, 0.3, 0), params=(0.12, 0.05, 0.012))
    for dx in (-0.15, 0.15):  # along the blade's turned length
        s.prim(c, "sphere", (0.3 + dx * math.cos(0.3), -0.016, -0.75 - dx * math.sin(0.3)), steel,
               params=(0.025,))
    # The plate's cracks, and a wedge missing from it.
    for a, r in ((0.4, 0.3), (2.3, 0.28), (4.1, 0.32)):
        s.prim(c, "box", (2.0 + r * math.cos(a), -0.05, 0.1 + r * math.sin(a)), china,
               rot=(0, -a, 0), params=(r + 0.02, 0.1, 0.018))
    s.prim(c, "box", (2.0 + 0.55 * math.cos(5.3), -0.05, 0.1 + 0.55 * math.sin(5.3)), china,
           rot=(0, -5.3 + 0.8, 0), params=(0.3, 0.1, 0.3))
    # The missing wedge, lying a little way off.
    sh = s.layer()
    s.prim(sh, "round_box", (2.85, -0.03, 0.65), china, rot=(0, 0.9, 0),
           params=(0.16, 0.025, 0.09), extra=0.01)
    cl = s.layer(smooth=0.05)
    s.prim(cl, "box", (0.4, 0.05, 0.2), cloth, params=(4, 0.05, 2.5))
    # Ruck the cloth a little so it reads as cloth.
    for x, z, a in ((-2.6, 1.6, 0.4), (1.0, 2.0, -0.2), (3.3, -1.5, 0.9)):
        s.capsule_between(cl, (x - 0.6 * math.cos(a), -0.0, z - 0.6 * math.sin(a)),
                          (x + 0.6 * math.cos(a), -0.0, z + 0.6 * math.sin(a)), 0.06, cloth)
    h = s.layer(smooth=0.01)
    # Kitchen knife handle and bolster.
    s.prim(h, "round_box", (-2.05, -0.05, 0.0), handle, params=(0.3, 0.045, 0.07), extra=0.03)
    s.prim(h, "box", (-1.75, -0.05, 0.0), steel, params=(0.02, 0.055, 0.085))
    for x in (-2.25, -2.05, -1.85):
        s.prim(h, "capped_cylinder", (x, -0.098, 0.0), rivet, params=(0.018, 0.006))
    # Pocket knife handle.
    s.prim(h, "round_box", (-0.52, -0.045, 1.0), red, params=(0.24, 0.04, 0.07), extra=0.03)
    s.prim(h, "capped_cylinder", (-0.3, -0.09, 1.0), rivet, params=(0.025, 0.006))
    # Disposable razor, head towards -x.
    s.capsule_between(h, (0.7, -0.045, -0.2), (1.25, -0.045, 0.05), 0.04, plastic)
    s.prim(h, "round_box", (0.62, -0.05, -0.24), plastic, rot=(0, -0.43, 0),
           params=(0.06, 0.04, 0.16), extra=0.02)
    s.prim(h, "box", (0.58, -0.07, -0.26), steel, rot=(0, -0.43, 0),
           params=(0.025, 0.03, 0.14))
    s.light("directional", (0.3, -1, -0.6), (1, 0.97, 0.94), 0.5)
    s.light("point", (0.0, -1.6, -1.2), (0.95, 0.97, 1), 0.9)
    # Pocket knife: a steel liner along its handle, a nail nick in the
    # blade, a key ring through the end. Disposable razor: rubber grips
    # and the green lubricating strip over its blades.
    dk = s.layer()
    s.prim(dk, "box", (-0.52, -0.012, 1.0), steel, params=(0.235, 0.012, 0.072))
    s.prim(dk, "ellipsoid", (-0.12, y - 0.02, 1.03), handle, params=(0.035, 0.006, 0.012))
    s.prim(dk, "torus", (-0.85, -0.012, 1.0), steel, params=(0.09, 0.012))
    grip = s.material("grip", (0.1, 0.1, 0.12), roughness=0.7)
    strip = s.material("lube_strip", (0.4, 0.8, 0.5), roughness=0.4)
    for k in range(4):
        u = 0.25 + 0.15 * k
        s.prim(dk, "box", (0.7 + 0.55 * u, -0.08, -0.2 + 0.25 * u), grip, rot=(0, -0.43, 0),
               params=(0.02, 0.012, 0.045))
    s.prim(dk, "box", (0.64, -0.075, -0.23), strip, rot=(0, -0.43, 0), params=(0.012, 0.012, 0.14))
    # Small chips of the plate, scattered.
    rng = random.Random(37)
    for _ in range(5):
        cx_, cz_ = 2.0 + rng.uniform(-0.9, 0.9), 0.1 + rng.uniform(-0.9, 0.9)
        if math.hypot(cx_ - 2.0, cz_ - 0.1) > 0.7:
            s.prim(sh, "round_box", (cx_, -0.012, cz_), china, rot=(0, rng.uniform(0, 3), 0),
                   params=(rng.uniform(0.03, 0.06), 0.012, rng.uniform(0.02, 0.04)), extra=0.006)
    # Glints: lights placed past the blades from the camera, so their
    # reflections come back off the steel.
    s.light("point", (-1.3, -1.4, 1.6), (1, 0.98, 0.95), 1.2)
    s.light("point", (0.6, -1.3, 0.6), (1, 0.98, 0.95), 0.8)
    s.write()


# --- bath ----------------------------------------------------------------
def bath():
    s = Scene("bath", 0.05,
              "Mel's bath -- \"surprisingly comfy\": warm water, candles on the rim,\n"
              "the knife beside them.")
    enamel = s.material("tub", (0.95, 0.94, 0.9), roughness=0.15)
    water = s.material("water", (0.55, 0.62, 0.62), roughness=0.05)
    tiles = s.material("tiles", (0.85, 0.88, 0.85), base_map="hospitalfloor", uv_scale=0.8)
    floor = s.material("floor", (0.5, 0.45, 0.4), base_map="woodfloor", uv_scale=1)
    wax = s.material("candles", (0.97, 0.94, 0.85), roughness=0.8)
    flame = s.material("flame", (1, 0.8, 0.4), emissive_colour=(1, 0.75, 0.35),
                       emissive_intensity=0.25, casts_shadow=0)
    steel = s.material("knife", (0.8, 0.81, 0.83), roughness=0.2, metallic=1)
    chrome = s.material("chrome", (0.85, 0.86, 0.88), roughness=0.12, metallic=1)
    handle = s.material("handle", (0.25, 0.15, 0.08), roughness=0.6)
    # The tub, hollowed -- first, so its hollow cuts nothing else.
    t = s.layer(smooth=0.06)
    s.prim(t, "round_box", (0, -0.55, 0), enamel, params=(1.3, 0.45, 0.6), extra=0.25)
    hollow = s.layer("subtraction", 0.08)
    s.prim(hollow, "round_box", (0, -0.62, 0), enamel, params=(1.18, 0.48, 0.48), extra=0.22)
    w = s.layer()
    s.prim(w, "box", (0, -0.75, 0), water, params=(1.15, 0.02, 0.46))
    feet = s.layer(smooth=0.02)
    for x in (-1.05, 1.05):
        for z in (-0.4, 0.4):
            s.prim(feet, "ellipsoid", (x * 1.02, -0.07, z * 1.04), chrome, params=(0.09, 0.08, 0.11))
    room = s.layer()
    s.prim(room, "box", (0, 0.05, 0), floor, params=(5, 0.05, 5))
    s.prim(room, "box", (0, -1.8, 0.75), tiles, params=(5, 1.8, 0.05))
    s.prim(room, "box", (-1.45, -1.8, 0), tiles, params=(0.05, 1.8, 5))
    # Candles along the rim, at the back and the far end.
    c = s.layer()
    f = s.layer()
    for x, z, h in ((-0.95, 0.5, 0.06), (-0.7, 0.52, 0.1), (1.05, 0.5, 0.05),
                    (1.38, 0.2, 0.08)):
        top = -1.0 - 2 * h
        s.prim(c, "capped_cylinder", (x, -1.0 - h, z), wax, params=(0.075, h))
        s.prim(f, "ellipsoid", (x, top - 0.04, z), flame, params=(0.016, 0.035, 0.016))
    # One warm light for the lot, set back above them -- one per candle,
    # this close, blows the whole frame out.
    s.light("point", (0.2, -2.0, 0.2), (1, 0.72, 0.4), 0.05)
    s.light("directional", (0.3, 0.5, -1), (0.45, 0.5, 0.65), 0.12)
    # The knife on the near rim.
    k = s.layer()
    s.prim(k, "box", (0.25, -1.015, -0.52), steel, rot=(0, 0.15, 0), params=(0.22, 0.012, 0.035))
    s.prim(k, "round_box", (-0.07, -1.02, -0.47), handle, rot=(0, 0.15, 0),
           params=(0.12, 0.02, 0.03), extra=0.01)
    # Taps and a spout at the far end, a towel on a rail, a bath mat, and
    # her phone on the floor playing the music.
    tp = s.layer(smooth=0.02)
    s.capsule_between(tp, (-1.38, -1.05, 0.0), (-1.38, -1.35, 0.0), 0.04, chrome)
    s.capsule_between(tp, (-1.38, -1.35, 0.0), (-1.15, -1.32, 0.0), 0.035, chrome)
    for dz in (-0.22, 0.22):
        s.capsule_between(tp, (-1.38, -1.02, dz), (-1.38, -1.14, dz), 0.025, chrome)
        s.capsule_between(tp, (-1.42, -1.16, dz), (-1.34, -1.16, dz), 0.018, chrome)
        s.capsule_between(tp, (-1.38, -1.16, dz - 0.05), (-1.38, -1.16, dz + 0.05), 0.018, chrome)
    towel = s.material("towel", (0.75, 0.55, 0.6), roughness=1, bump_map="officecarpet", bump_strength=0.5)
    tw = s.layer(smooth=0.03)
    s.capsule_between(tw, (0.4, -1.55, 0.67), (1.3, -1.55, 0.67), 0.02, chrome)
    s.prim(tw, "round_box", (0.85, -1.2, 0.63), towel, params=(0.36, 0.38, 0.035), extra=0.03)
    mat = s.material("bath_mat", (0.55, 0.65, 0.75), roughness=1, bump_map="officecarpet", bump_strength=0.8)
    mt = s.layer()
    s.prim(mt, "round_box", (0.1, -0.012, -1.05), mat, params=(0.7, 0.012, 0.35), extra=0.01)
    body = s.material("phone", (0.04, 0.04, 0.05), roughness=0.2)
    glow = s.material("phone_screen", (0.6, 0.7, 1), emissive_colour=(0.6, 0.7, 1),
                      emissive_intensity=0.6, casts_shadow=0)
    ph = s.layer()
    s.prim(ph, "round_box", (0.9, -0.012, -1.2), body, rot=(0, 0.3, 0), params=(0.07, 0.01, 0.14), extra=0.01)
    s.prim(ph, "box", (0.9, -0.023, -1.2), glow, rot=(0, 0.3, 0), params=(0.06, 0.002, 0.125))
    s.write()


# --- urinals -------------------------------------------------------------
def urinals():
    s = Scene("urinals", 0.15,
              "Theo's peeping tom -- a row of urinals in a public toilet.")
    porcelain = s.material("urinal", (0.95, 0.95, 0.93), roughness=0.1)
    tiles = s.material("tiles", (0.8, 0.85, 0.82), base_map="hospitalfloor", uv_scale=0.6)
    floor = s.material("floor", (0.4, 0.42, 0.4), base_map="hospitalfloor", uv_scale=1.2)
    divider = s.material("divider", (0.35, 0.4, 0.45), roughness=0.4)
    pipe = s.material("pipe", (0.82, 0.83, 0.86), roughness=0.12, metallic=1)
    strip = s.material("strip_light", (0.9, 1, 0.95), emissive_colour=(0.9, 1, 0.95),
                       emissive_intensity=2.0)
    xs = (-1.5, 0.0, 1.5)
    u = s.layer(smooth=0.05)
    for x in xs:
        s.prim(u, "round_box", (x, -1.0, 2.75), porcelain, params=(0.3, 0.42, 0.22), extra=0.12)
    cut = s.layer("subtraction", 0.06)
    for x in xs:
        s.prim(cut, "ellipsoid", (x, -1.05, 2.5), porcelain, params=(0.22, 0.34, 0.2))
    r = s.layer()
    s.prim(r, "box", (0, 0.05, 1), floor, params=(4, 0.05, 4))
    s.prim(r, "box", (0, -1.6, 3.05), tiles, params=(4, 1.6, 0.05))
    s.prim(r, "box", (0, -3.1, 1), tiles, params=(4, 0.05, 4))
    for x in xs:
        s.capsule_between(r, (x, -1.45, 2.95), (x, -2.2, 2.95), 0.025, pipe)
    for x in (-0.75, 0.75):
        s.prim(r, "box", (x, -1.15, 2.6), divider, params=(0.025, 0.55, 0.35))
    s.prim(r, "box", (0, -3.0, 2.0), strip, params=(1.4, 0.02, 0.1))
    s.light("point", (0, -2.6, 1.6), (0.88, 1, 0.92), 1.4)
    # Flush valves, blue cakes in the bowls, a hand dryer, a floor drain,
    # and a yellow wet-floor sign.
    cake = s.material("urinal_cakes", (0.3, 0.55, 0.95), roughness=0.6)
    dryer = s.material("hand_dryer", (0.9, 0.9, 0.9), roughness=0.3)
    sign = s.material("wet_floor_sign", (0.98, 0.82, 0.1), roughness=0.4)
    fx = s.layer(smooth=0.01)
    for x in xs:
        s.prim(fx, "round_box", (x, -1.5, 2.93), pipe, params=(0.07, 0.06, 0.06), extra=0.02)
        s.capsule_between(fx, (x + 0.07, -1.5, 2.9), (x + 0.16, -1.53, 2.84), 0.015, pipe)
        s.prim(fx, "ellipsoid", (x, -0.68, 2.62), cake, params=(0.08, 0.025, 0.06))
    s.prim(fx, "round_box", (3.1, -1.25, 2.9), dryer, params=(0.22, 0.18, 0.12), extra=0.05)
    s.prim(fx, "capped_cylinder", (0, -0.002, 1.6), pipe, params=(0.15, 0.004))
    ws = s.layer()
    s.prim(ws, "box", (-2.4, -0.38, 1.0), sign, rot=(0.25, 0.4, 0), params=(0.22, 0.38, 0.012))
    s.prim(ws, "box", (-2.33, -0.38, 1.16), sign, rot=(-0.25, 0.4, 0), params=(0.22, 0.38, 0.012))
    s.write()


# --- firstaid ------------------------------------------------------------
def firstaid():
    s = Scene("firstaid", 0.25,
              "Diego's kit -- \"the proper bandaids and everything\", laid out neat on\n"
              "his bed beside the kitchen knife, wiped clean.")
    sheet = s.material("sheet", (1, 1, 1), base_map="blueandwhitesheets", uv_scale=2.5)
    tin = s.material("tin", (0.9, 0.9, 0.88), roughness=0.3)
    red = s.material("red_cross", (0.75, 0.05, 0.05), roughness=0.4)
    gauze = s.material("gauze", (0.98, 0.98, 0.97), roughness=1)
    bottle = s.material("bottle", (0.45, 0.2, 0.05), roughness=0.1)
    cap = s.material("cap", (0.95, 0.95, 0.95), roughness=0.4)
    plaster = s.material("plasters", (0.86, 0.68, 0.52), roughness=0.8)
    pad = s.material("pads", (1, 1, 1), roughness=1)
    steel = s.material("knife", (0.8, 0.81, 0.83), roughness=0.2, metallic=1)
    handle = s.material("handle", (0.08, 0.08, 0.08), roughness=0.5)
    g = s.layer()
    s.prim(g, "box", (0, 0.05, 0), sheet, params=(5, 0.05, 5))
    k = s.layer()
    s.prim(k, "round_box", (-0.6, -0.12, 0.3), tin, rot=(0, 0.1, 0),
           params=(0.42, 0.12, 0.3), extra=0.03)
    s.prim(k, "box", (-0.6, -0.245, 0.3), red, rot=(0, 0.1, 0), params=(0.18, 0.006, 0.05))
    s.prim(k, "box", (-0.6, -0.245, 0.3), red, rot=(0, 0.1, 0), params=(0.05, 0.006, 0.18))
    # A gauze roll, an antiseptic bottle, plasters and pads in a row.
    s.prim(k, "capped_cylinder", (0.25, -0.13, 0.55), gauze, rot=(math.pi / 2, 0, 0),
           params=(0.13, 0.11))
    s.prim(k, "capped_cylinder", (0.7, -0.3, 0.6), bottle, params=(0.1, 0.3))
    s.prim(k, "capped_cylinder", (0.7, -0.65, 0.6), cap, params=(0.06, 0.06))
    for i in range(4):
        s.prim(k, "round_box", (-0.55 + 0.32 * i, -0.012, -0.3 - 0.03 * i), plaster,
               rot=(0, 0.2 * i - 0.3, 0), params=(0.12, 0.008, 0.04), extra=0.008)
    for i in range(2):
        s.prim(k, "round_box", (0.75 + 0.05 * i, -0.02 - 0.04 * i, -0.15), pad,
               rot=(0, 0.3 * i, 0), params=(0.16, 0.018, 0.16), extra=0.01)
    s.prim(k, "box", (0.05, -0.02, -0.6), steel, rot=(0, -0.1, 0), params=(0.42, 0.016, 0.08))
    s.prim(k, "round_box", (-0.62, -0.04, -0.67), handle, rot=(0, -0.1, 0),
           params=(0.26, 0.035, 0.055), extra=0.02)
    s.light("directional", (0.3, -1, -0.5), (1, 0.98, 0.95), 0.45)
    s.light("point", (0.4, -1.6, -1.0), (0.95, 0.97, 1), 0.3)
    # Scissors and tweezers, a tube of antiseptic cream, cotton balls, and
    # a length of gauze unrolled from the roll.
    loops = s.material("scissor_loops", (0.1, 0.25, 0.6), roughness=0.4)
    tube = s.material("cream_tube", (0.95, 0.95, 0.92), roughness=0.3)
    tube_cap = s.material("tube_cap", (0.2, 0.45, 0.8), roughness=0.3)
    sc = s.layer()
    for a in (0.15, -0.15):
        s.prim(sc, "box", (0.55 + 0.2 * math.cos(a), -0.012, 0.15 + 0.2 * math.sin(a)), steel,
               rot=(0, -a, 0), params=(0.2, 0.01, 0.022))
        s.prim(sc, "torus", (0.25 - 0.05 * math.cos(a), -0.015, 0.15 - 0.12 * math.sin(a * 6)), loops,
               params=(0.055, 0.016))
    s.prim(sc, "box", (1.0, -0.01, 0.35), steel, rot=(0, 0.2, 0), params=(0.18, 0.008, 0.012))
    s.prim(sc, "box", (1.0, -0.01, 0.38), steel, rot=(0, 0.35, 0), params=(0.18, 0.008, 0.012))
    tb = s.layer(smooth=0.02)
    s.capsule_between(tb, (-0.15, -0.045, 0.95), (0.25, -0.045, 1.05), 0.045, tube)
    s.prim(tb, "capped_cylinder", (0.32, -0.045, 1.07), tube_cap, rot=(0, 0, math.pi / 2), params=(0.03, 0.04))
    ct = s.layer(smooth=0.02)
    for cx_, cz_ in ((-1.15, -0.2), (-1.05, -0.32), (-1.22, -0.36)):
        s.prim(ct, "sphere", (cx_, -0.05, cz_), gauze, params=(0.06,))
    gz = s.layer()
    s.prim(gz, "box", (0.25, -0.006, 0.9), gauze, rot=(0, 0.2, 0), params=(0.1, 0.006, 0.3))
    s.write()


# --- chapter 4 -----------------------------------------------------------
def meadow():
    s = Scene("meadow", 0.35,
              "Chapter 4's meadow -- waist-high grass running to the horizon, one\n"
              "tree far off. Framed two ways (see kCutaways): down in the grass at\n"
              "dusk for Mel's \"the tall grass claims us all\", and wide under a\n"
              "clear sky for Theresa's \"years of clear blue days\".")
    soil = s.material("soil", (0.22, 0.2, 0.12), roughness=1)
    grass = s.material("grass", (0.42, 0.45, 0.2), roughness=1)
    dry = s.material("dry_grass", (0.62, 0.55, 0.3), roughness=1)
    deep = s.material("deep_grass", (0.26, 0.34, 0.14), roughness=1)
    g = s.layer()
    s.prim(g, "box", (0, 0.2, 20), soil, params=(60, 0.2, 60))
    # The field: vertical blades tiled on three grids of different spacing
    # and height, so no one row lines up. Repetition happens in the
    # primitive's own frame, so the blades can't be tilted here -- a tilt
    # would tip the whole grid up out of the ground.
    fl = s.layer()
    for (x, z), half, cell, count, mat in (
            ((0.0, 12.0), 0.5, (0.21, 0, 0.23), (110, 1, 110), grass),
            ((0.1, 12.11), 0.65, (0.26, 0, 0.19), (90, 1, 130), dry),
            ((0.05, 12.05), 0.38, (0.17, 0, 0.29), (130, 1, 85), deep)):
        s.prim(fl, "capsule", (x, -half, z), mat, params=(0.018, half),
               **limited(cell, count))
    # Loose tufts in the foreground, each blade leaning its own way -- what
    # the camera is actually down among.
    tf = s.layer()
    rng = random.Random(404)
    for _ in range(170):
        bx, bz = rng.uniform(-3.2, 3.2), rng.uniform(-1.4, 3.5)
        h = rng.uniform(0.7, 1.35)
        lean = (rng.uniform(-0.3, 0.3), rng.uniform(-0.2, 0.25))
        s.capsule_between(tf, (bx, 0.0, bz), (bx + lean[0] * h, -h, bz + lean[1] * h),
                          rng.uniform(0.012, 0.022), rng.choice((grass, dry, dry, deep)))
    # Seed heads on the tallest of them.
    seed = s.material("seed_heads", (0.75, 0.66, 0.42), roughness=1)
    sh = s.layer(smooth=0.01)
    for _ in range(40):
        bx, bz = rng.uniform(-3, 3), rng.uniform(-1, 3.5)
        h = rng.uniform(1.1, 1.4)
        s.prim(sh, "ellipsoid", (bx, -h, bz), seed, params=(0.025, 0.08, 0.025))
    # One tree, alone near the horizon.
    bark = s.material("tree_bark", (0.2, 0.15, 0.1), roughness=1)
    leaves = s.material("tree_leaves", (0.2, 0.3, 0.13), roughness=1)
    tr = s.layer(smooth=0.1)
    s.capsule_between(tr, (7, 0, 34), (7.2, -4.5, 34), 0.35, bark)
    s.capsule_between(tr, (7.1, -3.2, 34), (8.4, -5.0, 34.3), 0.18, bark)
    s.capsule_between(tr, (7.1, -3.4, 34), (5.9, -5.2, 33.8), 0.18, bark)
    cr = s.layer(smooth=0.6)
    for dx, dy, dz, r in ((0, -6.2, 0, 2.4), (-1.6, -5.5, 0.4, 1.8), (1.7, -5.6, -0.2, 1.9),
                          (0.3, -7.3, 0.2, 1.6)):
        s.prim(cr, "ellipsoid", (7.1 + dx, dy, 34 + dz), leaves, params=(r * 1.1, r * 0.8, r))
    s.light("directional", (-0.5, -0.45, -0.7), (1, 0.85, 0.65), 1.0)
    s.write()


def hill():
    s = Scene("hill", 0.3,
              "Diego's hill -- a long grey slope climbing into fog, a figure bent\n"
              "under a crate partway up it, no top in sight.")
    ground = s.material("slope", (0.36, 0.36, 0.34), roughness=1)
    dirt = s.material("path", (0.27, 0.25, 0.22), roughness=1)
    rock = s.material("rocks", (0.3, 0.3, 0.3), roughness=0.9)
    body = s.material("figure", (0.16, 0.16, 0.18), roughness=0.8)
    crate = s.material("crate", (0.42, 0.33, 0.22), base_map="woodpanel", uv_scale=0.6,
                       roughness=0.9)
    fog = s.material("hill_fog", (0.7, 0.72, 0.74))
    a = 0.33  # the slope's pitch: the surface rises tan(a) for every metre in +z
    rise = math.tan(a)

    def on_slope(x, z, lift=0.0):
        return (x, -z * rise - lift, z)

    # The hill: one thick slab pitched up towards +z, its top through the
    # origin.
    g = s.layer()
    s.prim(g, "box", (0, 0.6 * math.cos(a), 0.6 * math.sin(a)), ground, rot=(a, 0, 0),
           params=(14, 0.6, 70))
    # The worn path, in short pieces -- one long thin box past the camera
    # blacks out the whole frame (see the road set).
    pt = s.layer()
    for k in range(18):
        z = -2 + k * 2.6
        s.prim(pt, "box", on_slope(0.05 * math.sin(k), z, 0.01), dirt, rot=(a, 0, 0),
               params=(0.55, 0.02, 1.25))
    rk = s.layer(smooth=0.05)
    rng = random.Random(8)
    for _ in range(22):
        x, z = rng.uniform(-8, 8), rng.uniform(-1, 30)
        if abs(x) < 1.2:
            continue
        r = rng.uniform(0.15, 0.6)
        s.prim(rk, "ellipsoid", on_slope(x, z, r * 0.2), rock, rot=(0, rng.uniform(0, 3), 0),
               params=(r * 1.4, r * 0.7, r))
    # Diego, facing uphill, bent under the crate.
    fz = 3.0
    figure(s, 0.0, fz, 1.75, math.pi, 0.0, body, stoop=0.45, wide=True)
    lift = fz * rise
    for p in s.layers[-1]["prims"]:
        p["pos"] = (p["pos"][0], p["pos"][1] - lift, p["pos"][2])
    cl = s.layer()
    # Strapped across his bent back, between the shoulders and the hips.
    s.prim(cl, "box", (0, -lift - 1.38, fz - 0.12), crate, rot=(0.55, 0, 0),
           params=(0.32, 0.22, 0.3))
    # Fog thickening up the slope until there's nothing to see.
    s.volumetric("box", on_slope(0, 22, 2), (a, 0, 0), (16, 4, 14), 0.05, fog)
    s.volumetric("box", on_slope(0, 44, 3), (a, 0, 0), (18, 6, 12), 0.12, fog)
    s.light("directional", (0.3, -0.8, 0.4), (0.85, 0.87, 0.9), 0.7)
    s.write()


def beach():
    s = Scene("beach", 0.45,
              "Diego's beach -- home, sunny, a towel and an umbrella and nobody\n"
              "else, the sea out to the horizon.")
    sand = s.material("sand", (0.92, 0.82, 0.62), roughness=1)
    wet = s.material("wet_sand", (0.7, 0.6, 0.45), roughness=0.4)
    sea = s.material("sea", (0.1, 0.38, 0.5), roughness=0.05)
    foam = s.material("foam", (0.95, 0.97, 0.98), roughness=0.6)
    g = s.layer()
    s.prim(g, "box", (0, 0.2, 0), sand, params=(60, 0.2, 14))
    s.prim(g, "box", (0, 0.21, 17), wet, params=(60, 0.2, 3))
    s.prim(g, "box", (0, 0.27, 80), sea, params=(80, 0.2, 60))
    fm = s.layer()
    for k in range(12):
        s.prim(fm, "ellipsoid", (-27 + k * 5, 0.04, 20.2 + 0.4 * math.sin(k * 1.7)), foam,
               params=(2.6, 0.04, 0.25))
    # The umbrella: a pole and a striped canopy -- alternating panels.
    pole = s.material("umbrella_pole", (0.92, 0.92, 0.9), roughness=0.4)
    red = s.material("canopy_red", (0.85, 0.2, 0.15), roughness=0.7)
    white = s.material("canopy_white", (0.96, 0.95, 0.92), roughness=0.7)
    ux, uz = -1.6, 4.0
    um = s.layer()
    s.capsule_between(um, (ux, 0.3, uz), (ux + 0.15, -2.3, uz), 0.035, pole)
    cn = s.layer(smooth=0.02)
    for k in range(8):
        t = k * math.pi / 4
        s.prim(cn, "ellipsoid", (ux + 0.15 + 0.65 * math.cos(t), -2.12, uz + 0.65 * math.sin(t)),
               red if k % 2 else white, rot=(0, -t, -0.32),
               params=(0.8, 0.04, 0.42))
    # A towel and a pair of flip-flops in its shade.
    towel = s.material("towel", (0.25, 0.55, 0.75), roughness=1)
    tw = s.layer()
    s.prim(tw, "round_box", (ux + 0.3, -0.01, uz - 0.6), towel, rot=(0, 0.3, 0),
           params=(0.5, 0.012, 0.95), extra=0.01)
    flip = s.material("flip_flops", (0.95, 0.75, 0.15), roughness=0.6)
    ff = s.layer()
    for dx in (0, 0.18):
        s.prim(ff, "round_box", (ux + 1.2 + dx, -0.015, uz - 1.6), flip, rot=(0, 0.15, 0),
               params=(0.06, 0.012, 0.13), extra=0.01)
    # A palm, leaning out over the sand.
    trunk = s.material("palm_trunk", (0.5, 0.4, 0.28), roughness=1)
    frond = s.material("palm_fronds", (0.22, 0.48, 0.18), roughness=0.8)
    pt = s.layer(smooth=0.08)
    pts = [(5.0 + 0.04 * i * i, -0.55 * i, 7.0) for i in range(11)]
    for a_, b_ in zip(pts, pts[1:]):
        s.capsule_between(pt, a_, b_, 0.2 - 0.008 * pts.index(a_), trunk)
    top = pts[-1]
    fr = s.layer(smooth=0.05)
    for k in range(7):
        t = k * 2 * math.pi / 7
        tip = (top[0] + 1.9 * math.cos(t), top[1] + 0.7, top[2] + 1.9 * math.sin(t))
        mid = tuple((p + q) / 2 + (0, -0.3, 0)[i] for i, (p, q) in enumerate(zip(top, tip)))
        s.capsule_between(fr, top, mid, 0.12, frond)
        s.capsule_between(fr, mid, tip, 0.07, frond)
    s.light("directional", (-0.3, -0.9, 0.35), (1, 0.97, 0.9), 1.4)
    s.write()


def easel():
    s = Scene("easel", 0.3,
              "Pat's easel -- a blank canvas under one lamp, brushes and paint\n"
              "waiting on the stool beside it.")
    wood = s.material("easel_wood", (0.5, 0.35, 0.22), base_map="woodpanel", uv_scale=0.5,
                      roughness=0.8)
    canvas = s.material("canvas", (0.95, 0.94, 0.9), roughness=1)
    floor = s.material("studio_floor", (0.25, 0.22, 0.2), base_map="woodpanel", uv_scale=1.2,
                       roughness=0.9)
    f_ = s.layer()
    s.prim(f_, "box", (0, 0.1, 0), floor, params=(8, 0.1, 8))
    e = s.layer(smooth=0.01)
    # Two front legs splayed, one back leg, the ledge and the top clamp.
    s.capsule_between(e, (-0.55, 0, -0.15), (-0.08, -2.0, 0.05), 0.03, wood)
    s.capsule_between(e, (0.55, 0, -0.15), (0.08, -2.0, 0.05), 0.03, wood)
    s.capsule_between(e, (0, 0, 0.75), (0, -1.9, 0.1), 0.03, wood)
    s.prim(e, "box", (0, -0.78, -0.08), wood, params=(0.5, 0.025, 0.06))
    s.prim(e, "box", (0, -1.86, -0.02), wood, params=(0.12, 0.03, 0.04))
    cv = s.layer()
    s.prim(cv, "box", (0, -1.3, -0.04), canvas, rot=(-0.1, 0, 0), params=(0.46, 0.5, 0.02))
    # The stool: a seat on three legs, the paint and a jar of brushes on it.
    seat = s.material("stool", (0.35, 0.25, 0.16), roughness=0.8)
    st = s.layer(smooth=0.01)
    sx, sz = 1.0, -0.3
    s.prim(st, "capped_cylinder", (sx, -0.62, sz), seat, params=(0.22, 0.025))
    for k in range(3):
        t = k * 2 * math.pi / 3
        s.capsule_between(st, (sx + 0.22 * math.cos(t), 0, sz + 0.22 * math.sin(t)),
                          (sx + 0.13 * math.cos(t), -0.6, sz + 0.13 * math.sin(t)), 0.02, seat)
    jar = s.material("jar", (0.8, 0.85, 0.85), roughness=0.05)
    jr = s.layer()
    s.prim(jr, "capped_cylinder", (sx + 0.08, -0.74, sz + 0.05), jar, params=(0.06, 0.1))
    brush = s.material("brush_handles", (0.75, 0.2, 0.15), roughness=0.5)
    br = s.layer()
    for k, (dx, dz) in enumerate(((0.02, 0.0), (-0.02, 0.02), (0.0, -0.025))):
        s.capsule_between(br, (sx + 0.08 + dx, -0.7, sz + 0.05 + dz),
                          (sx + 0.08 + dx * 4 + 0.02 * k, -1.02, sz + 0.05 + dz * 3), 0.008, brush)
    tubes = s.layer(smooth=0.01)
    for k, col in enumerate(((0.15, 0.3, 0.75), (0.9, 0.8, 0.2), (0.85, 0.2, 0.15))):
        m = s.material(f"paint_tube_{k}", col, roughness=0.3)
        s.capsule_between(tubes, (sx - 0.12, -0.665, sz - 0.1 + 0.07 * k),
                          (sx - 0.01, -0.665, sz - 0.08 + 0.07 * k), 0.022, m)
    lamp = s.material("lamp", (1, 0.9, 0.7), emissive_colour=(1, 0.9, 0.7),
                      emissive_intensity=2.0, casts_shadow=0)
    lm = s.layer()
    s.prim(lm, "sphere", (-0.9, -2.9, -1.2), lamp, params=(0.08,))
    s.light("point", (-0.9, -2.8, -1.2), (1, 0.9, 0.75), 2.2)
    s.write()


def twocups():
    s = Scene("twocups", 0.4,
              "Theresa's morning -- two cups set out on the kitchen table, the\n"
              "kettle beside them, one chair pulled out.")
    table = s.material("kitchen_table", (0.55, 0.4, 0.26), base_map="woodpanel", uv_scale=0.7,
                       roughness=0.6)
    china = s.material("cups", (0.96, 0.96, 0.94), roughness=0.2)
    china2 = s.material("cup_blue", (0.35, 0.5, 0.75), roughness=0.2)
    t = s.layer()
    s.prim(t, "box", (0, 0.05, 0), table, params=(1.6, 0.05, 0.9))
    cups = s.layer(smooth=0.01)
    hollow = s.layer("subtraction", 0.005)
    handles = s.layer()
    for x, mat in ((-0.35, china), (0.25, china2)):
        s.prim(cups, "capped_cylinder", (x, -0.1, 0.0), mat, params=(0.09, 0.1))
        s.prim(hollow, "capped_cylinder", (x, -0.13, 0.0), mat, params=(0.078, 0.09))
        s.prim(handles, "torus", (x + 0.105, -0.1, 0.0), mat, rot=(math.pi / 2, 0, 0),
               params=(0.045, 0.012))
        s.prim(handles, "capped_cylinder", (x, -0.005, 0.0), mat, params=(0.13, 0.005))
    # The kettle: a squat body, a lid knob, a spout and a handle over the top.
    steel = s.material("kettle", (0.82, 0.82, 0.84), roughness=0.15, metallic=1)
    kt = s.layer(smooth=0.04)
    kx, kz = -0.9, 0.3
    s.prim(kt, "ellipsoid", (kx, -0.16, kz), steel, params=(0.2, 0.16, 0.2))
    s.prim(kt, "sphere", (kx, -0.33, kz), steel, params=(0.035,))
    s.capsule_between(kt, (kx + 0.15, -0.14, kz), (kx + 0.32, -0.28, kz), 0.03, steel)
    # The handle: a hoop whose lower half is buried in the body.
    kh = s.layer()
    s.prim(kh, "torus", (kx, -0.3, kz), steel, rot=(math.pi / 2, 0, 0), params=(0.14, 0.018))
    # A spoon and a sugar bowl, and the window's light across it all.
    sugar = s.material("sugar_bowl", (0.95, 0.93, 0.88), roughness=0.3)
    sb = s.layer(smooth=0.01)
    s.prim(sb, "ellipsoid", (0.0, -0.06, 0.45), sugar, params=(0.09, 0.07, 0.09))
    sp = s.layer()
    s.capsule_between(sp, (-0.05, -0.006, -0.25), (0.13, -0.006, -0.32), 0.01, steel)
    s.light("directional", (0.6, -0.5, 0.5), (1, 0.9, 0.75), 1.1)
    s.write()

os.makedirs(OUT, exist_ok=True)
for build in (ramen, bluehour, moon, fetal, shadowmen, oraborus, soup, icecream,
              flamenco, dragbar, puzzles, cabin, road, trippyroom, shadowdoorway,
              corridor, hospitalbed, deity, church, phone, livingroom,
              scars_stomach, scars_forearm, scars_wrist, scars_thigh,
              scars_thigh_old, blades, bath, urinals, firstaid,
              meadow, hill, beach, easel, twocups):
    build()
