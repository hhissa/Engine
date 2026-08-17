// Shared analytic scene-SDF evaluation, included (via
// GL_GOOGLE_include_directive, which glslc enables) by BOTH
// Builtin.RaymarchVoxelize.comp.glsl (to bake the field) and
// Builtin.RaymarchShader.comp.glsl (to re-evaluate material provenance
// per-pixel at hit points -- see scene_map()'s nearest_primitive out
// param). Factored out precisely so those two can never drift apart: a
// primitive type or combine rule added to one but not the other would
// otherwise render with the wrong material even though its shape baked
// fine.
//
// The including shader must #define these buffer binding slots before the
// #include (they differ between the two pipelines' descriptor set layouts):
//
//   SDF_PRIMITIVE_BUFFER_BINDING   -- Primitive primitives[]
//   SDF_LAYER_BUFFER_BINDING       -- Layer layers[]
//   SDF_PARAM_EXPR_BUFFER_BINDING  -- ParamExpr param_exprs[]

// One entry per registered static primitive (see GeometryConfig,
// engine-side) -- position_type.xyz is its world-space position,
// position_type.w its PrimitiveType as a float; params is interpreted
// per-type (see primitive_sdf below). rotation is a unit quaternion
// (x,y,z,w) rotating the primitive's local space into world space --
// primitive_sdf() applies its inverse to a sample point before evaluating
// box/sphere_sdf, so this is read for every type but only actually changes
// the result for a Box (a sphere is rotation-invariant, and a plane's
// rotation is never set to anything but identity engine-side -- see
// GeometryConfig::plane()/add_plane()).
struct Primitive {
    vec4 position_type;
    vec4 params;
    vec4 rotation;
    // x = the accumulated uniform scale for formula-driven param slots
    // (Geometry::param_expr_scale engine-side -- see resolve_params()
    // below for how it's applied). yzw = this primitive's emissive
    // radiance (Material::emissive_colour * emissive_intensity,
    // pre-multiplied engine-side) -- (0,0,0) for a non-emissive material.
    // Only Builtin.RaymarchShader.comp.glsl's render pass reads yzw (added
    // straight into a hit's shaded colour, making the primitive glow
    // regardless of incoming light -- see main() there); the voxelize
    // pass never touches them, it only cares about shape.
    vec4 expr_scale;
    // Domain repetition (Inigo Quilez, https://iquilezles.org/articles/
    // sdfrepetition/): x = RepetitionMode as a float (0=None default; see
    // the REPEAT_* constants below), yzw = cell spacing per axis
    // (Infinite/Limited: X/Y/Z; Rectangular: X/Z, Y unused). Left at its
    // zero-init default (None) for a volumetric primitive, which never
    // repeats -- see rebuild_static_scene(), engine-side.
    vec4 repeat_mode_cell;
    // xyz: Limited/Rectangular: instance count per axis (X/Y/Z; Rectangular
    // uses X/Z only). Rotational: x = copy count n. Ignored entirely
    // whenever repeat_mode_cell.x == REPEAT_NONE or REPEAT_INFINITE.
    // w: a conservative world-space bounding-sphere radius around
    // position_type.xyz, precomputed engine-side (see
    // VulkanRaymarchShader::rebuild_static_scene()'s compute_bounding_
    // radius()) -- how far this primitive's own surface (and, if
    // repeated, its farthest repeated copy) can possibly reach. scene_map()
    // below uses this for a cheap pre-check that skips the full
    // primitive_sdf() evaluation (rotation, domain repetition, deformation,
    // shape function, parametric-attribute VM) wherever it plainly can't
    // matter. UNBOUNDED_BOUNDING_RADIUS (see below) means "never skip this
    // one" -- used for a Plane, an infinitely-repeating primitive, or one
    // with any active parametric-attribute formula, none of which have a
    // safe static bound. Was unused padding before this field existed;
    // repurposed rather than growing every GpuPrimitive by another vec4.
    vec4 repeat_count;
    // Domain deformation (Inigo Quilez, https://iquilezles.org/articles/
    // distfunctions/ "Deforming" section) -- see evaluate_primitive_at()
    // below for exactly how each is applied. x = twist (radians per
    // world-unit of local Y), y = bend (radians per world-unit of local
    // X), z = displace_amplitude (a length), w = displace_frequency (a
    // sin() rate). All zero-init to 0 except this engine's own
    // displace_frequency default of 20 -- see rebuild_static_scene(),
    // engine-side -- meaning a volumetric primitive (which never sets this
    // field) gets deform = (0,0,0,0), i.e. no deformation at all.
    vec4 deform;
};

layout(binding = SDF_PRIMITIVE_BUFFER_BINDING) readonly buffer PrimitiveBuffer {
    Primitive primitives[];
};

// One entry per registered layer (see GeometrySystem::SceneLayer,
// engine-side), in evaluation order. op_smoothness.x is the LayerOperation
// as a float (0=union, 1=subtraction), op_smoothness.y its smoothness;
// range.x/range.y are the start index and count of this layer's primitives
// within PrimitiveBuffer above (primitives are uploaded grouped by layer).
struct Layer {
    vec4 op_smoothness;
    ivec4 range;
};

layout(binding = SDF_LAYER_BUFFER_BINDING) readonly buffer LayerBuffer {
    Layer layers[];
};

// One compiled "parametric attribute" formula -- a primitive parameter
// authored as e.g. "0.1 + 0.1*p.y" instead of a fixed number (see
// engine/src/resources/expression.h for the compiler/supported syntax,
// and evaluate_expr()/resolve_params() below for how this bytecode gets
// interpreted). instruction_count == 0 means this slot has no formula;
// resolve_params() then falls back to the primitive's plain params.xyz/
// (extra) constant for it. 4 consecutive entries per primitive (indices
// primitive_index*4 + 0/1/2/3 -> params.x/y/z/extra_param) in
// ParamExprBuffer below -- matches GpuParamExpr engine-side exactly (plain
// scalar arrays, so std430 packs this the same way that C struct is laid
// out, with no manual offset bookkeeping needed on either side).
const int MAX_EXPR_INSTRUCTIONS = 16;
struct ParamExpr {
    int op[MAX_EXPR_INSTRUCTIONS];
    float operand[MAX_EXPR_INSTRUCTIONS];
    int instruction_count;
};

layout(binding = SDF_PARAM_EXPR_BUFFER_BINDING) readonly buffer ParamExprBuffer {
    ParamExpr param_exprs[];
};

// Opcodes -- must match ExprOp in engine/src/resources/expression.h exactly.
const int OP_CONST = 0;
const int OP_VAR_X = 1;
const int OP_VAR_Y = 2;
const int OP_VAR_Z = 3;
const int OP_ADD = 4;
const int OP_SUB = 5;
const int OP_MUL = 6;
const int OP_DIV = 7;
const int OP_NEG = 8;
const int OP_ABS = 9;
const int OP_SIN = 10;
const int OP_COS = 11;
const int OP_SQRT = 12;
const int OP_MIN = 13;
const int OP_MAX = 14;
const int OP_POW = 15;
const int OP_CLAMP = 16;

// A small stack-machine interpreter for the bytecode compile_expression()
// (engine-side) produces -- see expression.h's grammar comment for the
// source syntax this bytecode came from. `p` is the primitive's local-space
// sample point (feeds the expression's p.x/p.y/p.z variables).
float evaluate_expr(ParamExpr e, vec3 p) {
    float stack[MAX_EXPR_INSTRUCTIONS];
    int sp = 0;
    for (int i = 0; i < e.instruction_count; ++i) {
        int op = e.op[i];
        if (op == OP_CONST) {
            stack[sp++] = e.operand[i];
        } else if (op == OP_VAR_X) {
            stack[sp++] = p.x;
        } else if (op == OP_VAR_Y) {
            stack[sp++] = p.y;
        } else if (op == OP_VAR_Z) {
            stack[sp++] = p.z;
        } else if (op == OP_NEG) {
            stack[sp - 1] = -stack[sp - 1];
        } else if (op == OP_ABS) {
            stack[sp - 1] = abs(stack[sp - 1]);
        } else if (op == OP_SIN) {
            stack[sp - 1] = sin(stack[sp - 1]);
        } else if (op == OP_COS) {
            stack[sp - 1] = cos(stack[sp - 1]);
        } else if (op == OP_SQRT) {
            stack[sp - 1] = sqrt(max(stack[sp - 1], 0.0));
        } else if (op == OP_ADD) {
            sp--; stack[sp - 1] = stack[sp - 1] + stack[sp];
        } else if (op == OP_SUB) {
            sp--; stack[sp - 1] = stack[sp - 1] - stack[sp];
        } else if (op == OP_MUL) {
            sp--; stack[sp - 1] = stack[sp - 1] * stack[sp];
        } else if (op == OP_DIV) {
            sp--; stack[sp - 1] = stack[sp - 1] / stack[sp];
        } else if (op == OP_MIN) {
            sp--; stack[sp - 1] = min(stack[sp - 1], stack[sp]);
        } else if (op == OP_MAX) {
            sp--; stack[sp - 1] = max(stack[sp - 1], stack[sp]);
        } else if (op == OP_POW) {
            sp--; stack[sp - 1] = pow(stack[sp - 1], stack[sp]);
        } else if (op == OP_CLAMP) {
            sp -= 2; stack[sp - 1] = clamp(stack[sp - 1], stack[sp], stack[sp + 1]);
        }
    }
    return sp > 0 ? stack[0] : 0.0;
}

// Resolves primitive_index's effective params for local-space point p:
// starts from its plain constant params.xyz/w, then overrides whichever
// slots have a compiled formula (see ParamExpr above), evaluated fresh at
// p -- so a tapered/twisted/etc. shape's parameter genuinely varies per
// sample point, not just once per primitive.
//
// expr_scale is the primitive's accumulated uniform scale (engine-side
// scale_scene() -- 1 when never scaled). Plain constants were already
// multiplied engine-side, but a formula is an authored *function* from a
// local point to a length, so scaling the shape by s means s*f(p/s):
// evaluate at the point mapped back into the authored (unscaled) local
// space, then scale the resulting length. Applied only to formula slots --
// applying it to the pre-scaled constants too would double-scale them.
vec4 resolve_params(int primitive_index, vec4 base_params, vec3 p, float expr_scale) {
    vec4 result = base_params;
    int base = primitive_index * 4;
    vec3 authored_p = p / expr_scale;
    ParamExpr ex = param_exprs[base + 0];
    if (ex.instruction_count > 0) result.x = evaluate_expr(ex, authored_p) * expr_scale;
    ex = param_exprs[base + 1];
    if (ex.instruction_count > 0) result.y = evaluate_expr(ex, authored_p) * expr_scale;
    ex = param_exprs[base + 2];
    if (ex.instruction_count > 0) result.z = evaluate_expr(ex, authored_p) * expr_scale;
    ex = param_exprs[base + 3];
    if (ex.instruction_count > 0) result.w = evaluate_expr(ex, authored_p) * expr_scale;
    return result;
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

// ---------------------------------------------------------------------------
// Domain repetition (Inigo Quilez, https://iquilezles.org/articles/
// sdfrepetition/): a repeated primitive is evaluated at one or more
// *candidate* points -- copies of the local-space sample point folded back
// toward the origin cell -- and the smallest resulting distance wins,
// exactly the article's "evaluate sdf at every candidate instance, take the
// min" pattern. Each candidate re-runs resolve_params()/the primitive's own
// shape function fresh (see evaluate_primitive_at() below), so a
// parametric-attribute formula still varies correctly per repeated instance
// instead of being computed once at the original point.
// ---------------------------------------------------------------------------

// Sentinel Primitive::repeat_count.w value meaning "this primitive has no
// safe static bound -- scene_map()'s cull_radius pre-check must never skip
// it." Must exceed any cull_radius a caller could plausibly pass (the
// voxelize pass's fine-sample loop uses a value on the order of a handful
// of world units -- see Builtin.RaymarchVoxelize.comp.glsl -- so this only
// needs to clear that by a wide margin, not represent literal infinity).
// Matches VulkanRaymarchShader::kUnboundedBoundingRadius engine-side.
const float UNBOUNDED_BOUNDING_RADIUS = 1e8;

const int REPEAT_NONE = 0;
const int REPEAT_INFINITE = 1;
const int REPEAT_LIMITED = 2;
const int REPEAT_ROTATIONAL = 3;
const int REPEAT_RECTANGULAR = 4;

// Dispatches to the primitive-specific distance function by type -- the
// same switch primitive_sdf() used to do directly; factored out so every
// repeat_*() function below can call it once per candidate point.
float evaluate_shape(int type, vec3 local, vec4 params) {
    if (type == 0) {
        return sphere_sdf(local, params.x);
    } else if (type == 1) {
        return box_sdf(local, params.xyz);
    } else if (type == 2) {
        return plane_sdf(local, params.x);
    } else if (type == 3) {
        return torus_sdf(local, params.x, params.y);
    } else if (type == 4) {
        return capped_cylinder_sdf(local, params.x, params.y);
    } else if (type == 5) {
        return capped_cone_sdf(local, params.x, params.y, params.z);
    } else if (type == 6) {
        return round_box_sdf(local, params.xyz, params.w);
    } else if (type == 7) {
        return box_frame_sdf(local, params.xyz, params.w);
    } else if (type == 8) {
        return octahedron_sdf(local, params.x);
    } else if (type == 9) {
        // params.y <= 0 covers scenes saved before this param existed (the
        // generic "params=x y z w" writer already emits 0 for an unset
        // slot -- see geometry_bounding_radius()'s identical fallback,
        // engine-side) -- falls back to the old hardcoded half-extent so
        // those scenes' pyramids render exactly as before.
        return pyramid_sdf(local, params.x, params.y > 0.0 ? params.y : 0.5);
    } else if (type == 10) {
        return hex_prism_sdf(local, params.x, params.y);
    } else if (type == 11) {
        return round_cone_sdf(local, params.x, params.y, params.z);
    } else if (type == 12) {
        return capsule_sdf(local, params.x, params.y);
    } else if (type == 13) {
        return link_sdf(local, params.x, params.y, params.z);
    }
    return ellipsoid_sdf(local, params.xyz);
}

// Resolves index's parametric-attribute params fresh at candidate point r,
// then evaluates its shape there -- the one place every repeat_*() function
// below actually samples the primitive, so a formula-driven param (e.g.
// radius = "0.1 + 0.1*p.y") varies per repeated instance rather than being
// computed once at the original, unrepeated point. Also the one place
// domain deformation (see Primitive::deform above) is applied, for exactly
// the same reason -- so a repeated instance is deformed the same way as an
// unrepeated one instead of the warp only ever affecting whichever single
// candidate point primitive_sdf() used to call this with directly.
//
// Order (Inigo Quilez, https://iquilezles.org/articles/distfunctions/
// "Deforming"): twist warps r.xz by an angle proportional to r.y, then
// bend warps the twisted result's .xy by an angle proportional to its own
// (already-twisted) x -- so the two compose, matching
// opCheapBend(opTwist(primitive)) -- then the shape is evaluated at that
// final warped point. Displacement is instead a post-evaluation offset
// added to the returned distance, computed from the *original* r (not the
// twisted/bent point) -- mirrors opDisplace(primitive, p)'s own p being
// whatever its caller passed in, unmodified by any operator primitive
// itself might further wrap. Both are approximate (non-exact) distance
// perturbations -- see the article's own warning -- but this engine's
// primary render pass marches a coarse baked voxel field rather than
// sphere-tracing the analytic distance directly (see
// Builtin.RaymarchVoxelize.comp.glsl), which is far more tolerant of that
// than a naive real-time sphere-tracer would be.
float evaluate_primitive_at(int index, int type, vec3 r, vec4 prim_params, float expr_scale) {
    vec4 params = resolve_params(index, prim_params, r, max(expr_scale, 1e-6));

    vec4 deform = primitives[index].deform;
    vec3 q = r;
    if (deform.x != 0.0) { // twist, around local Y
        float c = cos(deform.x * q.y);
        float s = sin(deform.x * q.y);
        q = vec3(c * q.x + s * q.z, q.y, -s * q.x + c * q.z);
    }
    if (deform.y != 0.0) { // bend, around local Z
        float c = cos(deform.y * q.x);
        float s = sin(deform.y * q.x);
        q = vec3(c * q.x + s * q.y, -s * q.x + c * q.y, q.z);
    }

    float d = evaluate_shape(type, q, params);
    if (deform.z != 0.0) { // displacement
        d += deform.z * sin(deform.w * r.x) * sin(deform.w * r.y) * sin(deform.w * r.z);
    }
    return d;
}

// Infinite repetition every cell.axis units, independently per axis -- an
// axis with cell.axis <= 0 is left unrepeated (every candidate keeps that
// axis's original coordinate), so a primitive can repeat along e.g. just X,
// or X and Z, while staying a single instance along Y. Checks every
// neighbour tile toward local on every axis (8 = 2^3 candidates) so a
// neighbouring instance larger than one cell can't be missed and produce a
// wrong (too-large) distance -- see the article's "correct repetition"
// section. Cheaper single-sample shortcuts exist (mirroring, or folding into
// one quadrant) but only hold for shapes symmetric about every repeated
// axis, which isn't true of every primitive type in this engine (e.g. Box/
// RoundBox/BoxFrame/Pyramid/Link can have different extents per axis), so
// the always-correct, 8-candidate approach is used unconditionally instead.
float repeat_infinite(int index, int type, vec3 local, vec4 prim_params, float expr_scale,
                      vec3 cell) {
    bvec3 axis_active = greaterThan(cell, vec3(1e-5));
    vec3 safe_cell = mix(vec3(1.0), cell, axis_active); // 1.0 placeholder keeps an inactive axis' divide safe
    vec3 id = round(local / safe_cell);
    vec3 o = mix(vec3(0.0), sign(local - safe_cell * id), axis_active); // never step off an inactive axis

    float d = 1e30;
    for (int k = 0; k < 2; ++k) {
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                vec3 rid = id + vec3(i, j, k) * o;
                vec3 r = mix(local, local - safe_cell * rid, axis_active);
                d = min(d, evaluate_primitive_at(index, type, r, prim_params, expr_scale));
            }
        }
    }
    return d;
}

// Finite/limited repetition: exactly repeat_infinite() above, but each
// axis's candidate instance id is additionally clamped to a fixed range
// first, so the tiling stops after count.axis copies instead of continuing
// forever -- see the article's "correct" limited-repetition section (the
// alternative -- boolean-intersecting the infinite tiling against a
// bounding box -- produces distance-field discontinuities that look fine
// but are wrong for anything relying on accurate distances). An axis with
// count.axis <= 1 keeps exactly one, centered instance (id clamped to 0),
// matching a plain unrepeated primitive along that axis; cell.axis <= 0
// leaves the axis alone entirely, same convention as repeat_infinite().
float repeat_limited(int index, int type, vec3 local, vec4 prim_params, float expr_scale,
                     vec3 cell, vec3 count) {
    bvec3 axis_active = greaterThan(cell, vec3(1e-5));
    vec3 safe_cell = mix(vec3(1.0), cell, axis_active);
    vec3 half_span = max(count - 1.0, 0.0) * 0.5; // count copies centered on 0: ids in [-half_span, half_span]
    vec3 id = round(local / safe_cell);
    vec3 o = mix(vec3(0.0), sign(local - safe_cell * id), axis_active);

    float d = 1e30;
    for (int k = 0; k < 2; ++k) {
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                vec3 rid = clamp(id + vec3(i, j, k) * o, -half_span, half_span);
                vec3 r = mix(local, local - safe_cell * rid, axis_active);
                d = min(d, evaluate_primitive_at(index, type, r, prim_params, expr_scale));
            }
        }
    }
    return d;
}

// Rotational/angular repetition of n evenly-spaced copies around the
// primitive's own local Y axis (compose with the primitive's `rotation` to
// repeat around any axis/orientation instead) -- see the article's "angular
// repetition" section, adapted from its 2D (x,y) form to this engine's
// XZ ground plane. Evaluates the two angular neighbour wedges either side of
// local (in XZ), same "check the neighbour, don't assume the current wedge
// is the closest instance" reasoning as the linear repetition techniques
// above, just in angle space instead of distance space. n < 2 disables
// repetition (falls back to the plain unrepeated point).
float repeat_rotational(int index, int type, vec3 local, vec4 prim_params, float expr_scale,
                       int n) {
    if (n < 2) {
        return evaluate_primitive_at(index, type, local, prim_params, expr_scale);
    }
    float sector = 6.283185307 / float(n);
    float angle = atan(local.z, local.x);
    float id = floor(angle / sector);

    float d = 1e30;
    for (int i = 0; i < 2; ++i) {
        float a = sector * (id + float(i));
        float c = cos(a);
        float s = sin(a);
        // Rotates local's XZ by -a so wedge i lands back onto wedge 0's
        // angular range, leaving Y untouched.
        vec3 r = vec3(c * local.x + s * local.z, local.y, -s * local.x + c * local.z);
        d = min(d, evaluate_primitive_at(index, type, r, prim_params, expr_scale));
    }
    return d;
}

// Rectangular (grid) repetition confined to the local XZ ground plane --
// count.x by count.y copies (of the XZ plane, so count.y here means "along
// Z") spaced cell.x/cell.y apart; Y is always left untouched. Distinct from
// repeat_limited() above mainly in locking Y (a plain 2-axis grid is the
// common case -- tiling columns/paving across the ground) and in its
// simpler 2-value cell/count. Uses the same neighbour-checked, clamped-id
// technique as repeat_limited() rather than the article's alternative
// single-evaluation "fold into one quadrant" trick -- that trick requires
// the wrapped SDF to be symmetric under swapping its two repeated axes (true
// of e.g. Sphere/Torus/Capsule, whose XZ cross-section is circular, but not
// of Box/RoundBox/BoxFrame/Pyramid/Link, whose extents can differ per axis),
// and this engine's primitive catalogue includes several of those, so the
// always-correct approach was used instead.
float repeat_rectangular(int index, int type, vec3 local, vec4 prim_params, float expr_scale,
                         vec2 cell, vec2 count) {
    bvec2 axis_active = greaterThan(cell, vec2(1e-5));
    vec2 safe_cell = mix(vec2(1.0), cell, axis_active);
    vec2 half_span = max(count - 1.0, 0.0) * 0.5;
    vec2 xz = local.xz;
    vec2 id = round(xz / safe_cell);
    vec2 o = mix(vec2(0.0), sign(xz - safe_cell * id), axis_active);

    float d = 1e30;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            vec2 rid = clamp(id + vec2(i, j) * o, -half_span, half_span);
            vec2 r = mix(xz, xz - safe_cell * rid, axis_active);
            vec3 candidate = vec3(r.x, local.y, r.y);
            d = min(d, evaluate_primitive_at(index, type, candidate, prim_params, expr_scale));
        }
    }
    return d;
}

// Transforms world point p into primitive index's own local space (world
// position subtracted, then rotated by the inverse of its rotation, exactly
// like primitive_sdf() below does internally before evaluating a shape
// function) -- exposed separately for callers that need the local point
// itself rather than just a distance, e.g. Builtin.RaymarchShader.comp.glsl's
// accumulate_volumetrics(), which projects it onto a texture UV to give a
// volumetric primitive's glow a shape-relative pattern instead of a flat
// tint.
vec3 primitive_local_space(int index, vec3 p) {
    Primitive prim = primitives[index];
    vec3 local = p - prim.position_type.xyz;
    int type = int(prim.position_type.w);
    if (type != 2) {
        vec4 inverse_rotation = vec4(-prim.rotation.xyz, prim.rotation.w);
        local = rotate_by_quat(local, inverse_rotation);
    }
    return local;
}

float primitive_sdf(int index, vec3 p) {
    Primitive prim = primitives[index];
    vec3 local = p - prim.position_type.xyz;
    int type = int(prim.position_type.w);
    if (type != 2) {
        // Rotate the sample point into the primitive's own unrotated local
        // space via the inverse rotation -- the conjugate, since
        // prim.rotation is always unit-length. Skipped for a plane (type
        // == 2): it's always the horizontal y=height plane and never
        // rotates (see GeometryConfig::plane()/add_plane()).
        vec4 inverse_rotation = vec4(-prim.rotation.xyz, prim.rotation.w);
        local = rotate_by_quat(local, inverse_rotation);
    }
    // Domain repetition (see repeat_*() above) happens here, in the
    // primitive's own local space, before params/the shape function are
    // evaluated -- each repeat_*() mode internally resolves params and
    // evaluates the shape once per repeated candidate point itself (see
    // evaluate_primitive_at()), rather than this function doing it once up
    // front, so a parametric-attribute formula still varies correctly per
    // repeated instance. The max() inside evaluate_primitive_at() guards
    // division by a zero expr_scale (only possible if an uploaded primitive
    // somehow left it unset).
    int repeat_mode = int(prim.repeat_mode_cell.x);
    if (repeat_mode == REPEAT_INFINITE) {
        return repeat_infinite(index, type, local, prim.params, prim.expr_scale.x,
                               prim.repeat_mode_cell.yzw);
    } else if (repeat_mode == REPEAT_LIMITED) {
        return repeat_limited(index, type, local, prim.params, prim.expr_scale.x,
                              prim.repeat_mode_cell.yzw, prim.repeat_count.xyz);
    } else if (repeat_mode == REPEAT_ROTATIONAL) {
        return repeat_rotational(index, type, local, prim.params, prim.expr_scale.x,
                                 int(prim.repeat_count.x));
    } else if (repeat_mode == REPEAT_RECTANGULAR) {
        return repeat_rectangular(index, type, local, prim.params, prim.expr_scale.x,
                                  prim.repeat_mode_cell.yw, prim.repeat_count.xz);
    }
    return evaluate_primitive_at(index, type, local, prim.params, prim.expr_scale.x);
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

// Evaluates the whole layered scene at p: every primitive, in every layer,
// is folded into the running result one at a time using *its own layer's*
// operation/smoothness (see the file header comment) -- a layer's
// operation isn't a single one-shot combine of "the layer's shape" against
// the scene, it's applied to *every* primitive the layer holds. This
// matters once a layer holds more than one primitive: a subtraction layer
// with 3 cutters smooth-carves each of them into the scene independently,
// rather than first hard-merging the 3 cutters into one blob and only
// smoothing where that blob meets the rest of the scene -- and a union
// layer with several primitives smooth-blends all of them together, not
// just hard-merging them before ever reaching another layer's smoothness.
// nearest_primitive receives which primitive is nearest at the winning
// side of the very last combine (-1 if the scene is empty), used to pick
// a material -- at brick-allocation time in the voxelize pass, and again
// per-pixel at hit points in the render pass.
//
// cull_radius is a cheap-pre-check budget, not a scene radius: a primitive
// is skipped entirely (never even reaching primitive_sdf()'s rotation/
// domain-repetition/deformation/shape-function/parametric-VM work) once
// its bounding sphere (Primitive::repeat_count.w) provably can't bring it
// within cull_radius of p. This is safe regardless of primitive/layer fold
// order: a shape's SDF is never smaller (in magnitude, for a point outside
// its bounding sphere) than the bounding sphere's own signed distance --
// standard bounding-volume-SDF reasoning (see e.g. Inigo Quilez's
// "bounding" article) -- so a primitive with `distance(p, position) -
// bounding_radius > cull_radius` genuinely has `primitive_sdf(idx, p) >
// cull_radius` too, and folding a value that large into a union/
// subtraction already at or below cull_radius changes nothing (smooth_
// union()/smooth_subtraction() both collapse to an exact hard min/max once
// the two operands are farther apart than the blend smoothness -- see
// their own comments) as long as the caller's cull_radius is itself a
// genuine upper bound on the scene's true nearest distance at p (callers
// that can't guarantee that, e.g. the render pass's single-point per-pixel
// query, pass UNBOUNDED_BOUNDING_RADIUS instead, disabling the check
// entirely -- see the callers in Builtin.RaymarchVoxelize.comp.glsl/
// Builtin.RaymarchShader.comp.glsl for which do which and why).
float scene_map(vec3 p, int layer_count, float cull_radius, out int nearest_primitive) {
    float running = 1e30;
    int running_material = -1;

    for (int layer_i = 0; layer_i < layer_count; ++layer_i) {
        Layer layer = layers[layer_i];
        int op = int(layer.op_smoothness.x);
        float smoothness = layer.op_smoothness.y;
        int start = layer.range.x;
        int count = layer.range.y;

        for (int i = 0; i < count; ++i) {
            int idx = start + i;

            float bounding_radius = primitives[idx].repeat_count.w;
            if (bounding_radius < UNBOUNDED_BOUNDING_RADIUS) {
                float closest_possible =
                    distance(p, primitives[idx].position_type.xyz) - bounding_radius;
                if (closest_possible > cull_radius) {
                    continue; // can't matter here -- skip the expensive evaluation
                }
            }

            float d = primitive_sdf(idx, p);

            float h;
            float combined = (op == 1) ? smooth_subtraction(d, running, smoothness, h)
                                       : smooth_union(d, running, smoothness, h);
            running_material = (h > 0.5) ? idx : running_material;
            running = combined;
        }
    }

    nearest_primitive = running_material;
    return running;
}
