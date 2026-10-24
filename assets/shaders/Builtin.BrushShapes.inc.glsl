// Analytic brush-shape distance functions + smooth blend operators,
// extracted verbatim from Builtin.SdfSceneCommon.inc.glsl (M0 CSG deletion
// pass) ahead of that file's scene-graph apparatus being deleted. These are
// the pure building blocks a future voxel-brush sculptor evaluates one
// oriented shape at a time against existing voxels -- no domain
// repetition/deformation/parametric-expression baggage, which were specific
// to the deleted CSG primitive-authoring scene graph.
//
// No #define'd buffer bindings are required to include this file (unlike
// the old SdfSceneCommon.inc.glsl) -- every function here is a pure
// function of its arguments, with no SSBO/UBO dependency.

// Rotates p by the INVERSE of unit quaternion q (xyz = vector part, w =
// scalar part) -- i.e. takes a world-space offset into a rotated stamp's
// own local frame, where its shape SDF is defined.
vec3 quat_rotate_inverse(vec4 q, vec3 p) {
    vec3 u = -q.xyz;
    return p + 2.0 * cross(u, cross(u, p) + q.w * p);
}

float sphere_sdf(vec3 p, float radius) {
    return length(p) - radius;
}

float box_sdf(vec3 p, vec3 half_extents) {
    vec3 q = abs(p) - half_extents;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

float plane_sdf(vec3 p, float height) {
    return p.y - height;
}

// Everything below is adapted from Inigo Quilez's SDF primitive catalogue
// (https://iquilezles.org/articles/distfunctions/), re-derived to this
// engine's Y-up local-space convention (radial distance from length(p.xz),
// height along p.y) matching sphere_sdf/box_sdf/plane_sdf above, and
// centered at the origin like every other primitive here (some of the
// source formulas instead run from y=0 to y=height -- shifted by half the
// height where that was the case, noted per function).

float torus_sdf(vec3 p, float major_radius, float minor_radius) {
    vec2 q = vec2(length(p.xz) - major_radius, p.y);
    return length(q) - minor_radius;
}

float capped_cylinder_sdf(vec3 p, float radius, float half_height) {
    vec2 d = abs(vec2(length(p.xz), p.y)) - vec2(radius, half_height);
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
}

float capped_cone_sdf(vec3 p, float half_height, float r1, float r2) {
    vec2 q = vec2(length(p.xz), p.y);
    vec2 k1 = vec2(r2, half_height);
    vec2 k2 = vec2(r2 - r1, 2.0 * half_height);
    vec2 ca = vec2(q.x - min(q.x, (q.y < 0.0) ? r1 : r2), abs(q.y) - half_height);
    vec2 cb = q - k1 + k2 * clamp(dot(k1 - q, k2) / dot(k2, k2), 0.0, 1.0);
    float s = (cb.x < 0.0 && ca.y < 0.0) ? -1.0 : 1.0;
    return s * sqrt(min(dot(ca, ca), dot(cb, cb)));
}

float round_box_sdf(vec3 p, vec3 half_extents, float corner_radius) {
    vec3 q = abs(p) - half_extents + corner_radius;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - corner_radius;
}

float box_frame_sdf(vec3 p, vec3 half_extents, float edge_thickness) {
    vec3 pp = abs(p) - half_extents;
    vec3 q = abs(pp + edge_thickness) - edge_thickness;
    return min(min(
        length(max(vec3(pp.x, q.y, q.z), 0.0)) + min(max(pp.x, max(q.y, q.z)), 0.0),
        length(max(vec3(q.x, pp.y, q.z), 0.0)) + min(max(q.x, max(pp.y, q.z)), 0.0)),
        length(max(vec3(q.x, q.y, pp.z), 0.0)) + min(max(q.x, max(q.y, pp.z)), 0.0));
}

float octahedron_sdf(vec3 p, float s) {
    vec3 q = abs(p);
    float m = q.x + q.y + q.z - s;
    vec3 r;
    if (3.0 * q.x < m) {
        r = q.xyz;
    } else if (3.0 * q.y < m) {
        r = q.yzx;
    } else if (3.0 * q.z < m) {
        r = q.zxy;
    } else {
        return m * 0.57735027;
    }
    float k = clamp(0.5 * (r.z - r.y + s), 0.0, s);
    return length(vec3(r.x, r.y - s + k, r.z - k));
}

// Base is a square of half-extent `base` at p.y == 0, apex at p.y == h -- so
// unlike every other primitive here, a Pyramid's `position` is its base
// center, not its centroid. Every 0.5 in the original (Inigo Quilez's
// sdPyramid(), which hardcodes a unit base) is literally "the base's own
// half-extent" throughout the derivation, so substituting the `base`
// parameter for each one generalizes it to an arbitrary base size without
// changing the formula's structure -- 0.25 was 0.5*0.5, so it becomes
// base*base (renamed b2) the same way.
float pyramid_sdf(vec3 p, float h, float base) {
    float b2 = base * base;
    float m2 = h * h + b2;
    p.xz = abs(p.xz);
    p.xz = (p.z > p.x) ? p.zx : p.xz;
    p.xz -= base;
    vec3 q = vec3(p.z, h * p.y - base * p.x, h * p.x + base * p.y);
    float s = max(-q.x, 0.0);
    float t = clamp((q.y - base * p.z) / (m2 + b2), 0.0, 1.0);
    float a = m2 * (q.x + s) * (q.x + s) + q.y * q.y;
    float bb = m2 * (q.x + base * t) * (q.x + base * t) + (q.y - m2 * t) * (q.y - m2 * t);
    float d2 = (min(q.y, -q.x * m2 - q.y * base) > 0.0) ? 0.0 : min(a, bb);
    return sqrt((d2 + q.z * q.z) / m2) * sign(max(q.z, -p.y));
}

float hex_prism_sdf(vec3 p, float inradius, float half_height) {
    const vec3 k = vec3(-0.8660254, 0.5, 0.57735);
    vec3 q = abs(p);
    vec2 qxz = q.xz - 2.0 * min(dot(k.xy, q.xz), 0.0) * k.xy;
    vec2 d = vec2(
        length(qxz - vec2(clamp(qxz.x, -k.z * inradius, k.z * inradius), inradius)) * sign(qxz.y - inradius),
        q.y - half_height);
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
}

// Source formula runs from p.y == 0 (radius r1) to p.y == height (radius
// r2) -- shifted by +half_height here so it's centered like every other
// primitive instead.
float round_cone_sdf(vec3 p, float r1, float r2, float half_height) {
    p.y += half_height;
    float height = 2.0 * half_height;
    float b = (r1 - r2) / height;
    float a = sqrt(max(1.0 - b * b, 0.0));
    vec2 q = vec2(length(p.xz), p.y);
    float k = dot(q, vec2(-b, a));
    if (k < 0.0) {
        return length(q) - r1;
    }
    if (k > a * height) {
        return length(q - vec2(0.0, height)) - r2;
    }
    return dot(q, vec2(a, b)) - r1;
}

float capsule_sdf(vec3 p, float radius, float half_height) {
    p.y -= clamp(p.y, -half_height, half_height);
    return length(p) - radius;
}

float link_sdf(vec3 p, float half_length, float r1, float r2) {
    vec3 q = vec3(p.x, max(abs(p.y) - half_length, 0.0), p.z);
    return length(vec2(length(q.xy) - r1, q.z)) - r2;
}

// Bound, not exact (like every other non-sphere/non-plane primitive here
// this is fine -- the voxelizer only needs a conservative distance to decide
// which coarse cells to allocate a brick for, and raymarch()'s sphere
// tracing safely under-steps rather than overshoot when d is merely a bound).
float ellipsoid_sdf(vec3 p, vec3 radii) {
    float k0 = length(p / radii);
    float k1 = length(p / (radii * radii));
    return k0 * (k0 - 1.0) / k1;
}

// Rotates v by unit quaternion q.
vec3 rotate_by_quat(vec3 v, vec4 q) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

// Polynomial smooth union (Inigo Quilez) -- blends a and b together across
// a radius of k instead of a hard min(). h receives which side "won" (1 =
// a, 0 = b), used to pick a material at the same time the distance is
// blended. k <= 0 falls back to a plain hard min() (division by k below
// would otherwise blow up).
float smooth_union(float a, float b, float k, out float h) {
    // Outside the blend band (|a-b| >= k) the polynomial below is exactly
    // a plain hard min() -- and taking the exact path there isn't just an
    // optimisation. scene_map() seeds its running distance with 1e30, and
    // feeding that through mix() is catastrophic: GPUs commonly evaluate
    // mix(x, y, h) as x + (y-x)*h, and 1e30 + (a-1e30)*1.0 cancels to 0
    // instead of a, collapsing the whole scene to "surface everywhere"
    // whenever the *first* folded layer is a smooth one.
    if (k <= 0.0001 || abs(a - b) >= k) {
        h = (a < b) ? 1.0 : 0.0;
        return min(a, b);
    }
    h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

// Polynomial smooth subtraction (Inigo Quilez): carves `cutter` out of
// `base`, rounding the new inner edge across a radius of k instead of a
// hard max(-cutter, base). h receives which side "won" (1 = cutter, i.e.
// this point is on the new surface the cut created; 0 = base, i.e. this
// point is unaffected by the cut).
float smooth_subtraction(float cutter, float base, float k, out float h) {
    // Same exact-hard-path guard as smooth_union() above: outside the
    // blend band (|base+cutter| >= k) the polynomial equals a plain hard
    // max(-cutter, base), and the 1e30 empty-scene seed must never reach
    // mix().
    if (k <= 0.0001 || abs(base + cutter) >= k) {
        float neg_cutter = -cutter;
        h = (neg_cutter > base) ? 1.0 : 0.0;
        return max(neg_cutter, base);
    }
    h = clamp(0.5 - 0.5 * (base + cutter) / k, 0.0, 1.0);
    return mix(base, -cutter, h) + k * h * (1.0 - h);
}

// --- Shape modifiers ---------------------------------------------------------
//
// A section rides in a param the round shapes don't otherwise use, so it
// travels everywhere params already go (the GPU stamp list, undo, .stamps
// files) with no new plumbing. Shape ids are SculptBrushShape's.
//
// SECTION (keep only part of the shape): params.w, as 0 = whole, 1 = half,
//   2 = quarter, 3 = eighth, on the shapes brush_shape_sectionable() lists.
//   Each step cuts with one more plane through the shape's own centre --
//   for the round-all-over shapes first across Y (keeping local +Y, which
//   sculpt mode's default brush turn puts up on screen, so a half sphere is
//   a dome), then X, then Z;
//   for the rest X, then Z, then Y (so a half cylinder is a half pipe and
//   a quarter one a rounded corner). Engine-side mirror:
//   crop_cpu::eval_brush_shape() in vulkan_raymarch_shader.cpp.

bool brush_shape_sectionable(int shape) {
    // Sphere, Torus, CappedCylinder, CappedCone, Octahedron, Pyramid,
    // HexPrism, RoundCone, Capsule, Link, Ellipsoid.
    return shape == 0 || shape == 3 || shape == 4 || shape == 5 ||
           (shape >= 8 && shape <= 14);
}

float brush_shape_section(float d, vec3 p, int shape, float code) {
    int cuts = clamp(int(round(code)), 0, 3);
    if (cuts == 0) {
        return d;
    }
    bool round_all_over = shape == 0 || shape == 14 || shape == 8;
    // Distance into each removed half-space, in cut order.
    vec3 cut = round_all_over ? vec3(-p.y, -p.x, -p.z) : vec3(-p.x, -p.z, -p.y);
    d = max(d, cut.x);
    if (cuts >= 2) {
        d = max(d, cut.y);
    }
    if (cuts >= 3) {
        d = max(d, cut.z);
    }
    return d;
}

// The full shape: base distance, then section. p is in the
// stamp's own local frame.
float brush_shape_sdf(vec3 p, int shape, vec4 params) {
    float d;
    switch (shape) {
        case 0:  d = sphere_sdf(p, params.x); break;
        case 1:  d = box_sdf(p, params.xyz); break;
        case 2:  d = plane_sdf(p, params.x); break;
        case 3:  d = torus_sdf(p, params.x, params.y); break;
        case 4:  d = capped_cylinder_sdf(p, params.x, params.y); break;
        case 5:  d = capped_cone_sdf(p, params.x, params.y, params.z); break;
        case 6:  d = round_box_sdf(p, params.xyz, params.w); break;
        case 7:  d = box_frame_sdf(p, params.xyz, params.w); break;
        case 8:  d = octahedron_sdf(p, params.x); break;
        case 9:  d = pyramid_sdf(p, params.x, params.y); break;
        case 10: d = hex_prism_sdf(p, params.x, params.y); break;
        case 11: d = round_cone_sdf(p, params.x, params.y, params.z); break;
        case 12: d = capsule_sdf(p, params.x, params.y); break;
        case 13: d = link_sdf(p, params.x, params.y, params.z); break;
        case 14: d = ellipsoid_sdf(p, params.xyz); break;
        default: d = sphere_sdf(p, params.x); break;
    }
    return brush_shape_sectionable(shape) ? brush_shape_section(d, p, shape, params.w)
                                          : d;
}
