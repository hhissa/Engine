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
    // below for how it's applied); yzw unused padding.
    vec4 expr_scale;
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

// Base is a unit square at p.y == 0 (fixed by the source formula, not a
// free parameter), apex at p.y == h -- so unlike every other primitive
// here, a Pyramid's `position` is its base center, not its centroid.
float pyramid_sdf(vec3 p, float h) {
    float m2 = h * h + 0.25;
    p.xz = abs(p.xz);
    p.xz = (p.z > p.x) ? p.zx : p.xz;
    p.xz -= 0.5;
    vec3 q = vec3(p.z, h * p.y - 0.5 * p.x, h * p.x + 0.5 * p.y);
    float s = max(-q.x, 0.0);
    float t = clamp((q.y - 0.5 * p.z) / (m2 + 0.25), 0.0, 1.0);
    float a = m2 * (q.x + s) * (q.x + s) + q.y * q.y;
    float b = m2 * (q.x + 0.5 * t) * (q.x + 0.5 * t) + (q.y - m2 * t) * (q.y - m2 * t);
    float d2 = (min(q.y, -q.x * m2 - q.y * 0.5) > 0.0) ? 0.0 : min(a, b);
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
    // Resolved *after* the rotation above, at this exact sample point --
    // see resolve_params()/evaluate_expr() for what "resolved" means (a
    // parametric attribute's formula is evaluated fresh per sample, not
    // once per primitive, so a tapered/twisted shape's parameter genuinely
    // varies along it). The max() guards division by a zero expr_scale
    // (only possible if an uploaded primitive somehow left it unset).
    vec4 params = resolve_params(index, prim.params, local,
                                 max(prim.expr_scale.x, 1e-6));
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
        return pyramid_sdf(local, params.x);
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
float scene_map(vec3 p, int layer_count, out int nearest_primitive) {
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
