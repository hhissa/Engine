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

// --- Disk-backed brick cache payload (strategy 1: bake a chunk once, ever).
//
// A chunk's baked bricks are gathered into one contiguous, quantized blob
// that the CPU reads back and writes to disk, keyed by a hash of the scene
// content that reaches that chunk. Re-entering the chunk -- in this run or
// a later one -- uploads the blob and scatters it back into freshly
// allocated bricks instead of re-evaluating ~2,000 SDF samples per cell.
//
// Layout, in 32-bit words:
//   [0]                                    brick_count written by the gather
//   [1 .. CHUNK_CELL_COUNT]                cell -> brick ordinal, or -1
//   [.. + CHUNK_CACHE_MAX_BRICKS]          per-brick source primitive index
//   [.. ]                                  voxels, 4 quantized voxels per word
//
// CHUNK_BRICK_VOXEL_COUNT is 18^3 = 5832, which divides by 4 exactly, so a
// brick is a whole number of words and nothing needs padding.
// Must match kChunkCacheMaxBricks engine-side -- see its comment for the
// measurement that set this (470 bricks per chunk average against the old
// 512 cap, so the cache was refusing the expensive half of every scene).
const int CHUNK_CACHE_MAX_BRICKS = 2048;
const int CHUNK_CACHE_BRICK_WORDS = CHUNK_BRICK_VOXEL_COUNT / 4;
const int CHUNK_CACHE_CELLMAP_WORD = 1;
const int CHUNK_CACHE_PRIMITIVE_WORD = CHUNK_CACHE_CELLMAP_WORD + CHUNK_CELL_COUNT;
const int CHUNK_CACHE_VOXEL_WORD =
    CHUNK_CACHE_PRIMITIVE_WORD + CHUNK_CACHE_MAX_BRICKS;
const int CHUNK_CACHE_WORDS =
    CHUNK_CACHE_VOXEL_WORD + CHUNK_CACHE_MAX_BRICKS * CHUNK_CACHE_BRICK_WORDS;

// Half-width of the stored narrow band, in voxels. Distances outside it
// clamp.
//
// MUST COVER generate_splat_points()'s widest keep test, which is what
// this constant is really sized by -- not sphere tracing. That test asks
// `abs(d) > level_spacing * 0.866` to decide a position is too far from
// the surface to be worth a point, and the COARSEST pyramid level's
// spacing is chunk_point_lod_stride(0) = 8 voxels, so it needs honest
// distances out to 8 * 0.866 = 6.93 voxels.
//
// At the old value of 2.0 every restored voxel clamped to +/-2, so that
// test could never fire for the two coarsest levels: they kept EVERY
// sample position in the brick instead of the few near the surface. Each
// restored brick then emitted 64 junk points, drained the shared cluster
// page pool (starving chunks that had nothing wrong with them), and put
// those points on the clamp plateau where the central-difference gradient
// is exactly zero -- so they took the vec3(0,1,0) fallback normal and
// skipped the projection onto the isosurface entirely, leaving cell-sized
// clouds of flat-shaded points floating in mid-air. Live-confirmed in
// sdf_editor once the disk cache had filled: correct on a fresh cache,
// corrupt as soon as restores took over.
//
// So the clamp is NOT safe in both directions, as this comment previously
// claimed: it is safe for consumers that read the value as a step length
// (a shorter step never overshoots), and unsafe for any consumer that
// reads it as "how far away is the surface", because under-stating that
// turns far into near. Anything added downstream that classifies by
// magnitude has to be checked against this band.
//
// 8 bits over +/-8 voxels puts the quantum at 1/16 of a voxel. The other
// sensitive consumer is calc_static_normal(), which finite-differences
// over ~1/6 of a voxel; trilinear interpolation across eight quantized
// samples averages most of the error away. Raise to 16 bits here and in
// the engine mirror if normals ever band on restored chunks.
const float CHUNK_CACHE_BAND_VOXELS = 8.0;

uint quantize_cached_voxel(float d, float band) {
    float n = clamp(d / band, -1.0, 1.0);
    return uint(clamp(round(n * 127.0 + 128.0), 0.0, 255.0));
}

float dequantize_cached_voxel(uint q, float band) {
    return (float(q) - 128.0) * (1.0 / 127.0) * band;
}

// Splat point CLUSTERS (the Dreams-style renderer -- see Builtin.
// ChunkPointSplat.comp.glsl's header for the technique). Surface points are
// generated by Builtin.ChunkVoxelize.comp.glsl right after a brick's voxels
// are populated, and stored in fixed-capacity CLUSTERS: a shared pool of
// equal-sized pages, handed out from a free list exactly like the brick pool
// itself, of which a brick claims as many as its surface actually needs.
//
// That split -- fixed cluster SIZE, variable cluster COUNT per brick -- is
// the whole point. The previous design gave every brick one fixed-size point
// array, which meant a brick whose surface needed more points than the array
// held simply lost the rest, leaving a hole the renderer reads as missing
// geometry (a splat gap shows whatever is behind it; it does NOT fall back
// to marching). It also meant a scheduling unit the size of a brick, when
// what culling and dispatch want is a unit of constant cost. Dreams sized
// its clusters at 256 points for exactly these two reasons.
//
// 256 also has to equal Builtin.ChunkPointSplat.comp.glsl's local_size_x --
// one workgroup per cluster, one thread per point slot -- and match
// kChunkClusterPoints engine-side.
const int CHUNK_CLUSTER_POINTS = 256;

// How many clusters one brick may claim, i.e. its point budget in units of
// clusters. 4 x 256 = 1024 points covers the finest LOD level of a brick
// crossed by two surface sheets; a brick that wants more drops to a coarser
// level rather than truncating (see the generator's own rollback comment).
const int CHUNK_MAX_CLUSTERS_PER_BRICK = 4;
const int CHUNK_POINTS_PER_BRICK =
    CHUNK_CLUSTER_POINTS * CHUNK_MAX_CLUSTERS_PER_BRICK;

// One point is two uints:
//   x = position, a 3x10-bit fraction of the owning cell's box.
//   y = surface normal, octahedral-encoded in 16 bits (upper 16 free --
//       roughness lands here when materials grow per-point).
// The normal is baked because it is FREE here (the generator already has
// the central-difference gradient it used to project the point onto the
// isosurface) and expensive later: without it every splatted pixel has to
// re-evaluate the field to shade, which is most of what the deferred pass
// wants to avoid.
uint pack_point_normal(vec3 n) {
    // Octahedral mapping: project onto the octahedron |x|+|y|+|z|=1, then
    // fold the lower hemisphere out into the corners of the unit square.
    vec3 a = n / max(abs(n.x) + abs(n.y) + abs(n.z), 1e-8);
    vec2 oct = a.z >= 0.0
        ? a.xy
        : (1.0 - abs(a.yx)) * vec2(a.x >= 0.0 ? 1.0 : -1.0,
                                   a.y >= 0.0 ? 1.0 : -1.0);
    uvec2 q = uvec2(clamp(oct * 0.5 + 0.5, 0.0, 1.0) * 255.0 + 0.5);
    return q.x | (q.y << 8);
}
vec3 unpack_point_normal(uint packed) {
    vec2 oct = vec2(float(packed & 0xFFu), float((packed >> 8) & 0xFFu)) *
        (1.0 / 255.0) * 2.0 - 1.0;
    vec3 n = vec3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

const int CHUNK_POINT_LOD_COUNT = 4;
int chunk_point_lod_stride(int level) {
    return 8 >> level; // 8, 4, 2, 1
}

// One cluster's record -- the unit everything downstream of the bake works
// in: the splat pass takes one workgroup per cluster, the cull pass keeps or
// rejects whole clusters, and the shadow pass splats clusters into shadow
// maps. Built only from vec4/uvec4 members so its std430 array stride is
// exactly 64 bytes with no padding surprises (see ChunkVoxelizeBatchSlot
// Buffer's comment in Builtin.ChunkVoxelize.comp.glsl for what happens
// otherwise), and matching GpuChunkCluster engine-side.
struct ChunkCluster {
    // xyz = the owning cell's min corner in RENDER space, w = its cell size.
    // Points are stored as fractions of this box.
    vec4 bbox_min_cell;
    // Bounding cone of the cluster's surface normals: xyz = axis, w =
    // cosine of the half angle. A cluster whose whole cone faces away from
    // the camera cannot contribute a visible splat.
    vec4 normal_cone;
    // Cumulative per-LOD-level point counts for the WHOLE owning brick (not
    // just this cluster) -- level L's point set is the brick's first
    // lod_counts[L] points, which may span several clusters. Duplicated
    // into every cluster of a brick so a cluster needs no back-reference to
    // decide how many of its own points a given level wants.
    uvec4 lod_counts;
    // x = this cluster's offset within the owning brick's point sequence,
    //     so brick-point index = x + local thread id.
    // y = how many of this cluster's CHUNK_CLUSTER_POINTS slots are filled;
    //     0 means the cluster is on the free list and must be skipped.
    // z = owning brick index (material lookup).
    // w = owning chunk's clip level.
    uvec4 meta;
};

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
