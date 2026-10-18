#version 450
#extension GL_GOOGLE_include_directive : require

// M2b: the GPU half of VulkanRaymarchShader::undo_stamp() -- reverts
// exactly one past Builtin.SculptStamp.comp.glsl dispatch using the
// snapshot that dispatch captured into UndoCaptureMetaBuffer/
// UndoCaptureVoxelBuffer (see that shader's own header comment for what got
// captured and why). One invocation per cell the ORIGINAL stamp touched
// (same StampCellBuffer list stamp_shape() already built and kept, re-
// uploaded here unchanged -- see undo_stamp()'s own comment), up to
// whatever UndoCaptureMetaBuffer's change_type actually recorded for it:
//
//   NOOP (0)      -- the original stamp did nothing to this cell (e.g. a
//                     subtraction into empty space) -- nothing to undo.
//   NEW_BRICK (1) -- the original stamp allocated a fresh brick here --
//                     Undo frees it back onto the shared free-list stack
//                     and clears chunk_indirection, exactly mirroring
//                     Builtin.ChunkEvict.comp.glsl's per-brick push (see
//                     its own comment for the idiom this copies) including
//                     returning its splat-point cluster pages, which a
//                     brick can genuinely own by the time Undo runs (the
//                     stamp's own splat-regen pass -- Builtin.
//                     SculptSplatRegen.comp.glsl -- always ran right after
//                     it, allocating cluster pages for whatever the brick
//                     held at stamp time).
//   MODIFIED (2)  -- the original stamp blended into an already-resident
//                     brick -- Undo writes the captured pre-stamp material
//                     and all CHUNK_BRICK_VOXEL_COUNT voxels straight back,
//                     byte for byte. No blending, no shape evaluation: this
//                     is a plain restore.
//
// After this dispatch (with a buffer barrier in between, same as stamp_
// shape()'s own stamp->splat-regen handoff), the caller re-runs Builtin.
// SculptSplatRegen.comp.glsl over the SAME cell list, completely
// unmodified -- its own defensive `if (brick_index < 0) return;` already
// does exactly the right thing for a cell this dispatch just freed (skip
// it, it's empty again), so MODIFIED/NOOP/NEW_BRICK all come out correct
// with no special-casing needed on the splat-regen side.
layout(local_size_x = 64) in;

#include "Builtin.SdfFieldConfig.inc.glsl"

// Mirrors Builtin.SculptStamp.comp.glsl's/Builtin.SculptSplatRegen.comp.
// glsl's identical StampCell struct exactly.
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

layout(std430, binding = 3) buffer ChunkBrickPrimitiveBuffer {
    int chunk_brick_primitive[];
};

layout(std430, binding = 4) buffer ChunkBrickFreeListBuffer {
    int brick_free_list[];
};

layout(std430, binding = 5) buffer ChunkBrickFreeListTopBuffer {
    uint brick_free_top;
};

// The splat point clusters a freed brick owned -- same four bindings and
// same free-on-evict idiom as Builtin.ChunkEvict.comp.glsl's own 4-7 (see
// its header comment for why a freed brick must return these too, or the
// splat pipeline keeps drawing points for geometry that no longer exists).
layout(std430, binding = 6) buffer ChunkBrickClusterBuffer {
    int brick_clusters[];
};
layout(std430, binding = 7) buffer ChunkClusterBuffer {
    ChunkCluster chunk_clusters[];
};
layout(std430, binding = 8) buffer ChunkClusterFreeListBuffer {
    int cluster_free_list[];
};
layout(std430, binding = 9) buffer ChunkClusterFreeTopBuffer {
    uint cluster_free_top;
};

// Mirrors Builtin.SculptStamp.comp.glsl's identical UndoCaptureMeta struct
// exactly -- written there, read (not written) here.
struct UndoCaptureMeta {
    int change_type; // 0 = NOOP, 1 = NEW_BRICK, 2 = MODIFIED.
    int old_material;
    // M3 (Looseness): repurposed from a formerly-unused pad0 -- same
    // change_type==2-only meaning as old_material immediately above.
    float old_looseness;
    int pad1;
};

layout(std430, binding = 10) readonly buffer UndoCaptureMetaBuffer {
    UndoCaptureMeta undo_meta[];
};

layout(std430, binding = 11) readonly buffer UndoCaptureVoxelBuffer {
    float undo_voxels[];
};

// M3 (Looseness): same per-brick buffer Builtin.SculptStamp.comp.glsl
// writes (binding 9 there) -- restored alongside chunk_brick_primitive
// below for a MODIFIED cell, same "no blending, plain restore" contract.
layout(std430, binding = 12) buffer ChunkBrickLoosenessBuffer {
    float chunk_brick_looseness[];
};

// Must match kMaxChunkBricks engine-side exactly -- same cross-check as
// Builtin.SculptStamp.comp.glsl's identical constant.
const int MAX_CHUNK_BRICKS = 16384 * NUM_CHUNK_LEVELS;
// Must match kMaxChunkClusters engine-side -- same constant/comment as
// Builtin.ChunkEvict.comp.glsl's identical MAX_CLUSTERS.
const int MAX_CLUSTERS_UNDO = 131072;

layout(push_constant) uniform PushConstants {
    int cell_count; // How many of StampCellBuffer/UndoCaptureMetaBuffer's
                   // entries are valid this dispatch -- the ORIGINAL
                   // stamp's cell_count, capped at VulkanRaymarchShader::
                   // kMaxSculptUndoCellsPerStamp (see undo_stamp()).
} pc;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= uint(pc.cell_count)) {
        return;
    }

    int change_type = undo_meta[idx].change_type;
    if (change_type == 0) {
        return; // NOOP -- nothing to undo for this cell.
    }

    int chunk_slot = cells[idx].slot_cell.x;
    int local_cell_index = cells[idx].slot_cell.y;
    int indirection_index = chunk_slot * CHUNK_CELL_COUNT + local_cell_index;
    int brick_index = chunk_indirection[indirection_index];
    if (brick_index < 0) {
        // Shouldn't happen (the stamp being undone must have left a real
        // brick here for either NEW_BRICK or MODIFIED) -- defensive only.
        return;
    }

    if (change_type == 1) {
        // NEW_BRICK -- free it back onto the shared pool, exact inverse of
        // Builtin.SculptStamp.comp.glsl's own pop (atomicAdd(-1) there,
        // atomicAdd(+1) here) -- same idiom Builtin.ChunkEvict.comp.glsl
        // uses for a whole-chunk evict, just scoped to one brick.
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
            if (slot < uint(MAX_CLUSTERS_UNDO)) {
                cluster_free_list[slot] = page;
            }
        }
        return;
    }

    // change_type == 2 (MODIFIED): plain restore, no blending.
    chunk_brick_primitive[brick_index] = undo_meta[idx].old_material;
    chunk_brick_looseness[brick_index] = undo_meta[idx].old_looseness;
    int base = brick_index * CHUNK_BRICK_VOXEL_COUNT;
    int voxel_base = int(idx) * CHUNK_BRICK_VOXEL_COUNT;
    for (int i = 0; i < CHUNK_BRICK_VOXEL_COUNT; ++i) {
        chunk_bricks[base + i] = undo_voxels[voxel_base + i];
    }
}
