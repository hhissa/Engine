// Single source of truth for the *shape* of the baked sparse voxel field --
// included by every shader that either bakes it (Builtin.RaymarchVoxelize.
// comp.glsl), samples it (Builtin.RaymarchShader.comp.glsl via
// Builtin.BakedFieldCommon.inc.glsl), or gather-rays against it
// (Builtin.ProbeBake.comp.glsl via the same). Before this file existed, these
// six constants were hand-copied verbatim into all three .comp.glsl files
// with a "must match exactly" comment at each copy -- easy to let drift.
// Must still match kCoarseDim/kBrickDim in vulkan_raymarch_shader.cpp exactly
// (C++ can't #include a GLSL file, so that side stays a documented manual
// sync point -- see the comment on kCoarseDim there).
//
// MAX_BRICKS is deliberately NOT here: only Builtin.RaymarchVoxelize.comp.
// glsl needs it (to bound-check a newly allocated brick index), so it stays
// declared locally there rather than forcing every includer to carry it.
const int COARSE_DIM = 128;
const int BRICK_DIM = 8;
// Each brick stores a 1-voxel apron on every side, evaluated directly from
// the SDF at each apron voxel's true world position -- see brick population
// in Builtin.RaymarchVoxelize.comp.glsl for why (seamless trilinear sampling
// across brick boundaries with no cross-brick communication).
const int BRICK_APRON_DIM = BRICK_DIM + 2;
const int BRICK_VOXEL_COUNT = BRICK_APRON_DIM * BRICK_APRON_DIM * BRICK_APRON_DIM;
const float BOUNDS = 16.0;
const float COARSE_CELL_SIZE = (2.0 * BOUNDS) / float(COARSE_DIM);

// Per-chunk voxel grid shape for the new chunked/streamed field (Builtin.
// ChunkVoxelize.comp.glsl / Builtin.ChunkedFieldCommon.inc.glsl) -- a
// SEPARATE field from the COARSE_DIM/BOUNDS one above, with its own buffers,
// deliberately kept fully independent (not sharing any GPU buffer with the
// fixed-cube field above) so this new, less-battle-tested path can never
// corrupt or interfere with the one sdf_editor/games rely on -- see
// VulkanRaymarchShader::synchronous_full_bake_mode_'s comment engine-side.
// A level-0 chunk is CHUNK_COARSE_DIM^3 coarse cells (same COARSE_CELL_SIZE
// fine-voxel resolution as the fixed-cube field, just partitioned into
// movable chunks instead of one static grid) -- CHUNK_WORLD_SIZE world units
// per axis. Must match kChunkCoarseDim in vulkan_raymarch_shader.cpp exactly.
const int CHUNK_COARSE_DIM = 16;
const int CHUNK_CELL_COUNT = CHUNK_COARSE_DIM * CHUNK_COARSE_DIM * CHUNK_COARSE_DIM;
const float CHUNK_WORLD_SIZE = float(CHUNK_COARSE_DIM) * COARSE_CELL_SIZE;

// A chunk's own brick resolution -- deliberately NOT BRICK_DIM above, even
// though every level uses the SAME cell layout otherwise. Every clip level
// shares this one value, but a level's own cell (chunk_level_world_size(
// level)/CHUNK_COARSE_DIM) grows 2x per level while this doesn't -- so a
// coarse level's voxel_size (cell_size/CHUNK_BRICK_DIM) grows right along
// with it, e.g. level 4 at kNumLevels=5 has cell_size=4.0 -> voxel_size=
// 4.0/8=0.5 at the OLD BRICK_DIM=8. A primitive thinner than that spacing
// (routine for architectural detail -- thin walls, panels, a repeated
// ceiling's own ridges) doesn't just render blocky there, it can fail to
// show a surface crossing between adjacent voxel samples AT ALL, reading
// as the geometry going missing/losing hits specifically as the camera
// moves far enough that it falls under a coarser level's responsibility --
// a real, reported symptom, not a hypothetical one. Doubled to 16 here
// (voxel_size=4.0/16=0.25 at that same level 4, matching the fixed-cube
// field's own uniform resolution) so every level -- especially the
// coarser ones this problem is specific to -- stays fine enough for
// ordinary architectural-scale detail. Kept as its OWN constant (not
// raising BRICK_DIM itself) so the fixed-cube field sdf_editor's
// synchronous-authoring fallback and every game rely on keeps its
// existing memory footprint exactly -- only the chunked field's own
// separate brick pool (chunk_brick_pool_buffer_ engine-side) pays for
// this. Must match kChunkBrickDim in vulkan_raymarch_shader.cpp exactly.
const int CHUNK_BRICK_DIM = 16;
const int CHUNK_BRICK_APRON_DIM = CHUNK_BRICK_DIM + 2;
const int CHUNK_BRICK_VOXEL_COUNT =
    CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM;
// The toroidal chunk-lookup table's side length (see ChunkTableBuffer,
// Builtin.ChunkedFieldCommon.inc.glsl) -- CHUNK_TABLE_DIM^3 total slots,
// each holding either -1 (no resident chunk) or the gpu_slot of whichever
// chunk currently owns that wrapped coordinate. Must match kChunkTableDim in
// vulkan_raymarch_shader.cpp exactly.
const int CHUNK_TABLE_DIM = 16;

// Phase 4 (clipmap LOD): how many nested chunk levels the streamed field
// keeps resident around the camera, and how many chunks out (Chebyshev
// distance) each level's own streaming window reaches -- both must match
// kNumLevels/kStreamRadiusChunks in vulkan_raymarch_shader.cpp exactly.
// chunk_level_world_size() below is the actual doubling formula
// (ChunkStreamingManager/VulkanRaymarchShader use the identical formula
// engine-side, see chunk_world_size() in chunk_types.h) -- level 0 is
// CHUNK_WORLD_SIZE wide, level 1 is 2x that, level 2 is 4x, etc., so total
// coverage grows exponentially with level even though every level keeps
// the exact same (2*STREAM_RADIUS_CHUNKS+1)^3 chunk *count* resident.
const int NUM_CHUNK_LEVELS = 5;
const int STREAM_RADIUS_CHUNKS = 1;

float chunk_level_world_size(int level) {
    return CHUNK_WORLD_SIZE * pow(2.0, float(level));
}
