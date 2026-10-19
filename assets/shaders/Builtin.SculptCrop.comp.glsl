#version 450
#extension GL_GOOGLE_include_directive : require

// M4 (Crop/Cutout): the one whole-volume boolean filter pass both tools
// share -- see VulkanRaymarchShader::crop_volume() (vulkan_raymarch_shader.
// cpp), which drives this dispatch. Unlike Builtin.SculptStamp.comp.glsl
// (one invocation per cell a brush's AABB touches), the CPU side here
// enumerates EVERY resident brick in the WHOLE volume (see crop_volume()'s
// own comment for the enumeration/batching mechanics) -- Crop/Cutout are
// whole-volume operations, not scoped to one shape's AABB like every prior
// stamp/spray/undo operation has been (see the plan file's §4 table,
// /home/h-i/.claude/plans/joyful-skipping-giraffe.md).
//
// The keep-inside identity (verified against smooth_subtraction()'s actual
// signature in Builtin.BrushShapes.inc.glsl -- `cutter, base, k, out h`,
// computing max(-cutter, base) in the hard-edged case): smooth_subtraction()
// carves `cutter` out of `base`, keeping base AND outside-cutter. Feeding
// cutter = -crop_sdf flips that into base AND inside-crop_sdf (max(-(-crop_
// sdf), base) = max(crop_sdf, base), the ordinary SDF INTERSECTION identity)
// -- i.e. "keep only what's inside the crop shape". That is pc.keep_outside
// == 0 (Crop's own effect, and the "live" half of Cutout -- see
// crop_volume()'s own header comment for why Cutout's complementary
// "outside" counterpart is instead computed on the CPU from a pre-op
// snapshot, not a second GPU pass with this flag flipped). pc.keep_outside
// != 0 feeds cutter = +crop_sdf instead -- the ordinary subtraction
// identity (base MINUS crop_sdf, i.e. "keep only what's outside the crop
// shape") -- wired here for symmetry/completeness (Builtin.BrushShapes.
// inc.glsl's own comment calls this "the mirror operation") even though no
// current call site drives it with keep_outside=1 on the LIVE volume.
//
// Reuses chunk_sculpt_undo_restore_set_'s EXISTING descriptor set/layout
// wholesale (see VulkanRaymarchShader::chunk_sculpt_undo_restore_set_
// layout_'s own comment, vulkan_raymarch_shader.h) -- its 12-binding list
// already has every buffer this shader needs (StampCellBuffer/
// chunk_indirection/chunk_brick_pool/the brick free list/the brick-cluster
// and cluster-pool buffers needed to free an emptied brick's splat pages --
// see EMPTY-BRICK FREEING below); bindings 3 (chunk_brick_primitive) and 10/
// 11 (the undo-capture scratch) are simply never referenced by this shader
// -- no new descriptor set/layout/pool needed, same "a different pipeline
// reading the same bound buffers" reuse chunk_sculpt_checkpoint_restore_
// pipeline_ already established for chunk_sculpt_stamp_set_layout_.
//
// EMPTY-BRICK FREEING: a brick that ends up with every apron voxel strictly
// positive (no isosurface crossing possible anywhere inside it, apron
// included) holds no surface at all after this pass -- freeing it back onto
// the shared brick-free-list stack (and returning its splat-cluster pages to
// their own free list, exactly mirroring Builtin.ChunkEvict.comp.glsl's own
// per-brick cleanup) keeps the pool from filling with dead space as a large
// Crop/Cutout discards a big fraction of the volume. Decided yes (not left
// for a future pass) -- see crop_volume()'s own header comment for the full
// writeup of why.
layout(local_size_x = 64) in;

#include "Builtin.SdfFieldConfig.inc.glsl"
#include "Builtin.BrushShapes.inc.glsl"

// Mirrors Builtin.SculptStamp.comp.glsl's eval_brush_shape() exactly (same
// shape constants/param layout) -- duplicated rather than shared via a
// third include because the two shaders' surrounding bindings/push-constant
// blocks differ enough that a shared .inc.glsl would need to thread shape
// dispatch through yet another indirection for no real benefit at this
// shader count. Any shape added to one must be added to the other (same
// lockstep note Builtin.SculptStamp.comp.glsl's own copy carries).
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

// Identical layout to Builtin.SculptStamp.comp.glsl's StampCell -- see its
// own comment. The CPU side (crop_volume()) fills one entry per RESIDENT
// brick in the whole volume, not per AABB-touched cell.
struct StampCell {
    ivec4 slot_cell;
    vec4 cell_min_size;
};

layout(std430, binding = 0) readonly buffer StampCellBuffer {
    StampCell cells[];
};

layout(std430, binding = 1) buffer ChunkIndirectionBuffer {
    int chunk_indirection[];
};

layout(std430, binding = 2) buffer ChunkBrickPoolBuffer {
    float chunk_bricks[];
};

layout(std430, binding = 4) buffer ChunkBrickFreeListBuffer {
    int brick_free_list[];
};

layout(std430, binding = 5) buffer ChunkBrickFreeListTopBuffer {
    uint brick_free_top;
};

layout(std430, binding = 6) buffer ChunkBrickClusterBuffer {
    int brick_clusters[];
};

layout(std430, binding = 7) buffer ChunkClusterBuffer {
    ChunkCluster chunk_clusters[];
};

layout(std430, binding = 8) buffer ChunkClusterFreeListBuffer {
    int cluster_free_list[];
};

layout(std430, binding = 9) buffer ChunkClusterFreeListTopBuffer {
    uint cluster_free_top;
};

// Must match kMaxChunkBricks/kMaxChunkClusters engine-side exactly -- same
// manual-sync-point convention every other sculpt-family shader already
// uses (see Builtin.SculptStamp.comp.glsl's identical MAX_CHUNK_BRICKS).
// Specialization constant 0: the engine passes its VRAM-sized
// max_chunk_bricks_ here at pipeline creation; 16384 per level is only the default.
layout(constant_id = 0) const int MAX_CHUNK_BRICKS = 16384 * NUM_CHUNK_LEVELS;
const int MAX_CLUSTERS = 131072;

// Plain scalars throughout, same convention/reasoning as every other
// sculpt-family push-constant block (see Builtin.SculptStamp.comp.glsl's
// own comment on why -- a prior milestone's real, hard-to-diagnose bug from
// exactly this kind of struct drifting out of byte-sync).
layout(push_constant) uniform PushConstants {
    float center_x;
    float center_y;
    float center_z;
    float param0;
    float param1;
    float param2;
    float param3;
    float smoothness;
    int shape_type;
    int cell_count;
    int keep_outside; // bool as int -- 0 = keep inside (Crop's own effect,
                      // and Cutout's live half), 1 = keep outside (wired
                      // for symmetry -- see this file's header comment).
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
        // Shouldn't happen -- the CPU side only ever enumerates cells that
        // were resident (occupied) at the moment it built this batch.
        // Defensive only.
        return;
    }

    vec3 crop_center = vec3(pc.center_x, pc.center_y, pc.center_z);
    vec4 params = vec4(pc.param0, pc.param1, pc.param2, pc.param3);
    const float voxel_size = COARSE_CELL_SIZE / float(CHUNK_BRICK_DIM);

    int base = brick_index * CHUNK_BRICK_VOXEL_COUNT;
    float min_dist = 1e30;
    for (int pz = 0; pz < CHUNK_BRICK_APRON_DIM; ++pz) {
        for (int py = 0; py < CHUNK_BRICK_APRON_DIM; ++py) {
            for (int px = 0; px < CHUNK_BRICK_APRON_DIM; ++px) {
                vec3 world_pos = cell_min + (vec3(px, py, pz) - 0.5) * voxel_size;
                float crop_sdf = eval_brush_shape(world_pos - crop_center,
                                                  pc.shape_type, params);
                float cutter = (pc.keep_outside != 0) ? crop_sdf : -crop_sdf;

                int voxel_index = base + px +
                    py * CHUNK_BRICK_APRON_DIM +
                    pz * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM;

                float old_dist = chunk_bricks[voxel_index];
                float unused_h;
                float out_dist = smooth_subtraction(cutter, old_dist,
                                                     pc.smoothness, unused_h);
                chunk_bricks[voxel_index] = out_dist;
                min_dist = min(min_dist, out_dist);
            }
        }
    }

    if (min_dist > 0.0) {
        // Every apron voxel reads strictly outside -- no surface left in
        // this brick at all. Free it, exactly mirroring Builtin.ChunkEvict.
        // comp.glsl's own per-brick cleanup (push the brick back onto the
        // free list, clear its indirection entry, return every splat-cluster
        // page it owned to the shared cluster-free list).
        uint write_index = atomicAdd(brick_free_top, 1u);
        if (write_index < uint(MAX_CHUNK_BRICKS)) {
            brick_free_list[write_index] = brick_index;
        }
        chunk_indirection[indirection_index] = -1;

        for (int i = 0; i < CHUNK_MAX_CLUSTERS_PER_BRICK; ++i) {
            int list_index = brick_index * CHUNK_MAX_CLUSTERS_PER_BRICK + i;
            int page = brick_clusters[list_index];
            if (page < 0) {
                continue;
            }
            brick_clusters[list_index] = -1;
            chunk_clusters[page].meta = uvec4(0);
            uint slot = atomicAdd(cluster_free_top, 1u);
            if (slot < uint(MAX_CLUSTERS)) {
                cluster_free_list[slot] = page;
            }
        }
    }
}
