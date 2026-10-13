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


def figure(s, x, z, height, face, lean, mat, smooth=0.04, wide=False, stoop=0.0,
           arms_up=False):
    """A featureless, too-tall standing figure in a layer of its own;
    face = yaw it turns towards, lean = how far it tips sideways. wide
    scales its width with its height too (the shadow men keep their
    original, narrower build); stoop bends its upper body forward (-z of
    its facing) and down; arms_up raises its arms out to the sides."""
    l = s.layer(smooth=smooth)
    k = height / 2.4
    w = k if wide else 1.0
    cos, sin = math.cos(face), math.sin(face)

    def at(dx, y, dz=0.0):
        bend = max(0.0, -y - 1.0)
        dz -= stoop * bend * bend
        y += stoop * 0.5 * bend * bend
        dx *= w
        dz *= w
        return (x + dx * cos + dz * sin + lean * -y * 0.05, y * k, z - dx * sin + dz * cos)

    s.prim(l, "sphere", at(0, -2.2), mat, params=(0.15 * k,))
    s.capsule_between(l, at(0, -2.0), at(0, -1.1), 0.17 * k, mat)
    s.capsule_between(l, at(-0.11, -1.05), at(-0.12, 0.0), 0.075 * k, mat)
    s.capsule_between(l, at(0.11, -1.05), at(0.12, 0.0), 0.075 * k, mat)
    if arms_up:
        s.capsule_between(l, at(-0.18, -1.92), at(-0.95, -2.7), 0.06 * k, mat)
        s.capsule_between(l, at(0.18, -1.92), at(0.95, -2.7), 0.06 * k, mat)
    else:
        # Arms too long, hanging clear of the body.
        s.capsule_between(l, at(-0.27, -1.9), at(-0.36, -0.6), 0.06 * k, mat)
        s.capsule_between(l, at(0.27, -1.9), at(0.36, -0.6), 0.06 * k, mat)


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
    s.write()


# --- glowingfetalposition ------------------------------------------------
def fetal():
    s = Scene("glowingfetalposition", 0.03,
              "Mel's depressive episodes -- a glowing figure curled on a black plane,\n"
              "god rays falling on it.")
    floor = s.material("floor", (0.02, 0.02, 0.02), roughness=0.4)
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
        figure(s, x, z, height, face, lean, shadow)

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
    e = s.layer()
    for z in (-0.19, 0.19):
        s.prim(e, "sphere", (head_pos[0] + 0.12, head_pos[1] - 0.06, z), eye,
               params=(0.045,))

    s.light("directional", (-0.4, -0.7, -1), (1, 1, 1), 0.9)
    s.light("point", (1.5, -1.5, -2.2), (0.9, 0.95, 1), 0.8)
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
    steel = s.material("spoon", (0.78, 0.78, 0.8), roughness=0.15)

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
    s.write()


def dragbar():
    s = stage("dragbar",
              "Theresa's first date -- a gay bar's stage, a glitter curtain, coloured\n"
              "lights and a mirror ball.", 0.08)
    gold = s.material("gold_strands", (0.95, 0.72, 0.3), roughness=0.12,
                      emissive_colour=(1, 0.7, 0.3), emissive_intensity=0.15)
    silver = s.material("pink_strands", (0.95, 0.55, 0.85), roughness=0.12,
                        emissive_colour=(1, 0.5, 0.8), emissive_intensity=0.15)
    mirror = s.material("mirror_ball", (0.85, 0.85, 0.9), roughness=0.05,
                        emissive_colour=(0.8, 0.85, 1), emissive_intensity=0.8)
    metal = s.material("mic_stand", (0.1, 0.1, 0.11), roughness=0.3)
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
    figure(s, 0.0, 4.15, 3.0, 0.0, 0.0, shadow, stoop=0.45)
    # From behind it, so its shadow reaches out towards us.
    s.light("directional", (0.3, -0.6, 1), (1, 1, 1), 0.5)
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
    rail = s.material("rail", (0.7, 0.72, 0.74), roughness=0.3)
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
    s.write()


# --- hospitalbed ---------------------------------------------------------
def hospitalbed():
    s = Scene("hospitalbed", 0.45,
              "Diego, catatonic -- an empty hospital bed alone in a white void, an IV\n"
              "stand beside it.")
    white = s.material("white", (0.96, 0.96, 0.96), roughness=1)
    metal = s.material("frame", (0.5, 0.53, 0.57), roughness=0.3)
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
    gold = s.material("cross", (0.8, 0.62, 0.28), roughness=0.3)
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
    brass = s.material("lamp_stand", (0.6, 0.48, 0.25), roughness=0.3, casts_shadow=0)
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
    s.write()


os.makedirs(OUT, exist_ok=True)
for build in (ramen, bluehour, moon, fetal, shadowmen, oraborus, soup, icecream,
              flamenco, dragbar, puzzles, cabin, road, trippyroom, shadowdoorway,
              corridor, hospitalbed, deity, church, phone, livingroom):
    build()
