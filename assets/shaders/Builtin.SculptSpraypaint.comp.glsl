#version 450
#extension GL_GOOGLE_include_directive : require

// M3c (Spraypaint): a PAINT-ONLY brush -- unlike Builtin.SculptStamp.
// comp.glsl, this never reads or writes chunk_bricks[]/chunk_brick_
// primitive[] (the SDF distance/per-brick-material data) at all. It
// accumulates a per-VOXEL opacity value into a brand-new buffer
// (ChunkBrickPaintBuffer below -- same size/addressing as chunk_brick_pool_
// buffer_, one float per voxel, CHUNK_BRICK_VOXEL_COUNT per brick) for every
// voxel inside the brush shape's volume, in every touched cell that already
// has a brick. See VulkanRaymarchShader::spray_paint() (vulkan_raymarch_
// shader.cpp) for the CPU-side cell-enumeration/dispatch driver -- it
// mirrors stamp_shape()'s own enumeration almost exactly, with one
// deliberate difference: it only emits a cell whose chunk is ALREADY
// resident (never allocates a new chunk-table slot the way stamp_shape()
// does), since a chunk nothing has ever stamped into has no bricks in it at
// all -- there is no surface, and nothing to paint.
//
// Cells with no existing brick (chunk_indirection < 0) are skipped below --
// the same "no brick -> nothing to touch" idiom Builtin.SculptStamp.
// comp.glsl's is_new/OP_SUBTRACTION early-return already established (see
// its own comment): there is no sentinel "paint on empty space" concept,
// and allocating a brick just to hold paint with no geometry underneath it
// would make a spray over thin air silently conjure a phantom brick for
// every later pass (chunk_indirection/free-list bookkeeping, splat
// regeneration, undo, etc.) to have to account for, for a voxel that isn't
// actually part of any surface.
//
// "Penetrates through geometry instead of coating only the outer shell":
// every voxel is tested against the raw brush-shape distance (eval_brush_
// shape() below) and painted whenever that distance is <= PAINT_THRESHOLD
// (0.0 -- i.e. anywhere INSIDE the shape's own volume), with no additional
// "near the existing surface's zero crossing" test at all. A sphere brush
// therefore paints every voxel of every already-allocated brick that falls
// within its radius -- the voxels just past a thin wall's far face exactly
// as much as the ones on its near face, both sides of a thin shell sitting
// inside the same brush volume, and the solid interior of a thick blob --
// which is what "spraying through" a surface, rather than only tinting the
// side facing the brush, actually means. (Whether a voxel the brush reaches
// has anything rendering it as a surface at all is a separate question the
// existing SDF already answers; this shader paints the ones on it without
// caring which side of that SDF's own zero-crossing they're on.)
//
// One invocation per TOUCHED COARSE CELL (not per voxel), identical dispatch
// shape to Builtin.SculptStamp.comp.glsl -- see its own header comment for
// why a serial per-cell voxel loop is not a bottleneck here either.
layout(local_size_x = 64) in;

#include "Builtin.SdfFieldConfig.inc.glsl"
#include "Builtin.BrushShapes.inc.glsl"

// Mirrors Builtin.SculptStamp.comp.glsl's SHAPE_* constants exactly (same
// lockstep note: must match VulkanRaymarchShader::BrushShape/
// SculptBrushShape value-for-value). Duplicated rather than shared via a
// third include: eval_brush_shape() below is a near-duplicate of SculptStamp.
// comp.glsl's own copy for the exact same "GLSL has no linking" reason
// Builtin.DeferredShade.comp.glsl's header comment gives for duplicating the
// raymarch shader's shading helpers instead of sharing them.
const int SHAPE_SPHERE = 0;
const int SHAPE_BOX = 1;
const int SHAPE_PLANE = 2;
const int SHAPE_TORUS = 3;
const int SHAPE_CAPPED_CYLINDER = 4;
const int SHAPE_CAPPED_CONE = 5;
const int SHAPE_ROUND_BOX = 6;
const int SHAPE_BOX_FRAME = 7;
const int SHAPE_OCTAHEDRON = 8;
const int SHAPE_PYRAMID = 9;
const int SHAPE_HEX_PRISM = 10;
const int SHAPE_ROUND_CONE = 11;
const int SHAPE_CAPSULE = 12;
const int SHAPE_LINK = 13;
const int SHAPE_ELLIPSOID = 14;

float eval_brush_shape(vec3 p, int shape_type, vec4 params) {
    switch (shape_type) {
        case SHAPE_SPHERE:         return sphere_sdf(p, params.x);
        case SHAPE_BOX:            return box_sdf(p, params.xyz);
        case SHAPE_PLANE:          return plane_sdf(p, params.x);
        case SHAPE_TORUS:          return torus_sdf(p, params.x, params.y);
        case SHAPE_CAPPED_CYLINDER:
            return capped_cylinder_sdf(p, params.x, params.y);
        case SHAPE_CAPPED_CONE:
            return capped_cone_sdf(p, params.x, params.y, params.z);
        case SHAPE_ROUND_BOX:
            return round_box_sdf(p, params.xyz, params.w);
        case SHAPE_BOX_FRAME:
            return box_frame_sdf(p, params.xyz, params.w);
        case SHAPE_OCTAHEDRON:     return octahedron_sdf(p, params.x);
        case SHAPE_PYRAMID:        return pyramid_sdf(p, params.x, params.y);
        case SHAPE_HEX_PRISM:      return hex_prism_sdf(p, params.x, params.y);
        case SHAPE_ROUND_CONE:
            return round_cone_sdf(p, params.x, params.y, params.z);
        case SHAPE_CAPSULE:        return capsule_sdf(p, params.x, params.y);
        case SHAPE_LINK:
            return link_sdf(p, params.x, params.y, params.z);
        case SHAPE_ELLIPSOID:      return ellipsoid_sdf(p, params.xyz);
        default:
            return sphere_sdf(p, params.x);
    }
}

// Same StampCell layout as Builtin.SculptStamp.comp.glsl -- the CPU driver
// reuses the identical GpuSculptStampCell struct/chunk_sculpt_cell_buffer_
// scratch buffer, since spray_paint() and stamp_shape() never run
// concurrently (both are synchronous, fence-waited, authoring-time calls).
struct StampCell {
    ivec4 slot_cell;
    vec4 cell_min_size;
};

layout(std430, binding = 0) readonly buffer StampCellBuffer {
    StampCell cells[];
};

layout(std430, binding = 1) readonly buffer ChunkIndirectionBuffer {
    int chunk_indirection[];
};

// NEW -- the per-voxel paint accumulator this milestone adds. Same size and
// brick-index-based addressing as ChunkBrickPoolBuffer (one float per voxel,
// CHUNK_BRICK_VOXEL_COUNT per brick): voxel_index = brick_index *
// CHUNK_BRICK_VOXEL_COUNT + local_apron_offset, identical to how chunk_
// bricks[] is addressed in Builtin.SculptStamp.comp.glsl/sample_chunked_
// field_at_level_slot(). Value is an opacity in [0, 1], 0 = unpainted.
layout(std430, binding = 2) buffer ChunkBrickPaintBuffer {
    float chunk_brick_paint[];
};

// Anywhere at or inside the brush shape's hard surface counts as "within
// its extent" -- see this file's header comment on why this is deliberately
// NOT an abs(dist) < epsilon shell test. 0.0 (not a small negative/positive
// fudge) is exactly "inside the shape", which is the natural, literal
// reading of "penetrates through geometry": the whole enclosed volume gets
// painted, not just a thin band around the shape's boundary.
const float PAINT_THRESHOLD = 0.0;

// Byte-for-byte scalar-only layout (see Builtin.SculptStamp.comp.glsl's own
// push-constant comment for why this convention exists) -- must match
// SculptSpraypaintPushConstants engine-side member for member.
layout(push_constant) uniform PushConstants {
    float center_x;
    float center_y;
    float center_z;
    float param0;
    float param1;
    float param2;
    float param3;
    float voxel_size;
    int shape_type;
    int cell_count;
    float opacity_per_pass; // Added (clamped to [0,1]) to every voxel this
                            // dispatch touches -- see main() below. Builds
                            // up over repeated spray_paint() calls at the
                            // same spot, since each call only ever adds on
                            // top of whatever is already there.
} pc;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= uint(pc.cell_count)) {
        return;
    }

    int chunk_slot = cells[idx].slot_cell.x;
    int local_cell_index = cells[idx].slot_cell.y;
    vec3 cell_min = cells[idx].cell_min_size.xyz;

    int indirection_index = chunk_slot * CHUNK_CELL_COUNT + local_cell_index;
    int brick_index = chunk_indirection[indirection_index];
    if (brick_index < 0) {
        // No brick here at all -- nothing to paint (see header comment).
        return;
    }

    vec3 brush_center = vec3(pc.center_x, pc.center_y, pc.center_z);
    vec4 params = vec4(pc.param0, pc.param1, pc.param2, pc.param3);
    float voxel_size = pc.voxel_size;
    float add = clamp(pc.opacity_per_pass, 0.0, 1.0);

    int base = brick_index * CHUNK_BRICK_VOXEL_COUNT;
    for (int pz = 0; pz < CHUNK_BRICK_APRON_DIM; ++pz) {
        for (int py = 0; py < CHUNK_BRICK_APRON_DIM; ++py) {
            for (int px = 0; px < CHUNK_BRICK_APRON_DIM; ++px) {
                // Same apron/voxel-centre convention as Builtin.SculptStamp.
                // comp.glsl -- see its own comment for the i0/s0 derivation
                // this must match.
                vec3 world_pos = cell_min + (vec3(px, py, pz) - 0.5) * voxel_size;
                float dist = eval_brush_shape(world_pos - brush_center,
                                              pc.shape_type, params);
                if (dist > PAINT_THRESHOLD) {
                    continue;
                }

                int voxel_index = base + px +
                    py * CHUNK_BRICK_APRON_DIM +
                    pz * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM;

                // Accumulate, clamped -- repeated passes over the same spot
                // build up toward fully opaque instead of overshooting past
                // 1.0, and a single pass over fresh (0-opacity) voxels never
                // starts above `add` itself.
                chunk_brick_paint[voxel_index] =
                    clamp(chunk_brick_paint[voxel_index] + add, 0.0, 1.0);
            }
        }
    }
}
