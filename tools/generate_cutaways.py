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

    # Table top under everything -- its top face at y = 1.0, under the foot.
    t = s.layer()
    s.prim(t, "box", (0, 1.06, 0.6), table, params=(4, 0.06, 3))
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
    s.prim(g, "box", (0, 0.1, 8), ground, params=(30, 0.1, 14))
    h = s.layer()
    roofs = s.layer()
    rng = random.Random(3)
    x = -14.0
    while x < 14.0:
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
    for tx, tz, size in [(-9.5, 7.2, 1.3), (3.8, 7.4, 1.6), (11, 7, 1.1)]:
        s.capsule_between(t, (tx, 0, tz), (tx, -2.2 * size, tz), 0.09, dark)
        for k in range(5):
            a = k / 5 * math.tau
            s.prim(t, "sphere", (tx + 0.55 * size * math.cos(a), -2.6 * size + 0.3 * math.sin(a * 2),
                                 tz + 0.4 * math.sin(a)), dark, params=(0.65 * size,))
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
        # A featureless, too-tall figure; face = yaw it turns towards.
        l = s.layer(smooth=0.04)
        k = height / 2.4
        cos, sin = math.cos(face), math.sin(face)

        def at(dx, y, dz=0.0):
            return (x + dx * cos + dz * sin + lean * -y * 0.05, y * k, z - dx * sin + dz * cos)

        s.prim(l, "sphere", at(0, -2.2), shadow, params=(0.15 * k,))
        s.capsule_between(l, at(0, -2.0), at(0, -1.1), 0.17 * k, shadow)
        s.capsule_between(l, at(-0.11, -1.05), at(-0.12, 0.0), 0.075 * k, shadow)
        s.capsule_between(l, at(0.11, -1.05), at(0.12, 0.0), 0.075 * k, shadow)
        # Arms too long, hanging clear of the body.
        s.capsule_between(l, at(-0.27, -1.9), at(-0.36, -0.6), 0.06 * k, shadow)
        s.capsule_between(l, at(0.27, -1.9), at(0.36, -0.6), 0.06 * k, shadow)

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


os.makedirs(OUT, exist_ok=True)
for build in (ramen, bluehour, moon, fetal, shadowmen, oraborus):
    build()
