#version 450
#extension GL_GOOGLE_include_directive : require

// Chunked-field voxelizer -- Phase 3a of the clipmap streaming work (see
// ChunkKey/ChunkRecord, engine/src/systems/chunk_types.h). One dispatch of
// this bakes an entire BATCH of chunks' CHUNK_COARSE_DIM^3 coarse cells
// each into the SEPARATE chunked-field buffers (ChunkIndirectionBuffer/
// ChunkBrickPoolBuffer/ChunkBrickPrimitiveBuffer below), at whichever
// gpu_slot each entry in ChunkVoxelizeBatchSlotBuffer says -- unlike Builtin.
// RaymarchVoxelize.comp.glsl's single dispatch over the whole fixed
// COARSE_DIM^3 grid, this is meant to be dispatched once per FRAME (Phase
// 6: not once per chunk any more -- see VulkanRaymarchShader::update_
// streaming()'s own comment), covering every chunk that frame's camera-
// driven streaming and/or dirty-scene rebaking decided needs a fresh bake.
//
// Deliberately a full parallel copy of RaymarchVoxelize.comp.glsl's
// structure rather than a shared/parameterized function: sharing would mean
// this new, less-battle-tested path and the existing one both depend on the
// same code, so a bug introduced here could reach back and affect
// sdf_editor/games -- see Builtin.SdfFieldConfig.inc.glsl's comment on why
// CHUNK_COARSE_DIM etc. are a separate field entirely. The actual analytic
// SDF evaluation (scene_map()) IS still shared, via Builtin.SdfSceneCommon.
// inc.glsl below, exactly like the old voxelizer -- that file has no
// COARSE_DIM/chunk awareness at all, it just evaluates the scene at a given
// world point, so sharing it carries none of that risk.
//
// One invocation per chunk-local coarse cell (CHUNK_COARSE_DIM^3), times
// however many chunks this dispatch's batch covers -- see main()'s own
// comment for how Z encodes both.
// 8x8x1, NOT 4x4x4, and post-build.sh VERIFIES this in the compiled SPIR-V.
// Z is one cell row per invocation, so a batch's Z range is exactly
// CHUNK_COARSE_DIM per entry and main() can recover batch_index from it by
// division. Same 64 threads per group either way.
//
// THE ENGINE'S DISPATCH MATH DEPENDS ON THIS EXACT SHAPE. voxelize_chunk_
// batch() issues (2, 2, rows) workgroups for a 16^3 chunk; against a 4x4x4
// shape that covers only a quarter of each chunk's X and Y and mis-maps
// batch_index entirely, which costs most of the scene's bricks and reports
// nothing. That is why it is checked at build time rather than trusted.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "Builtin.SdfFieldConfig.inc.glsl"

// Must match kMaxChunkBricks in vulkan_raymarch_shader.cpp exactly -- this
// is the SAME brick pool capacity concept as Builtin.RaymarchVoxelize.comp.
// glsl's MAX_BRICKS, just governing the chunked field's own, separate,
// smaller pool (see ChunkBrickPoolBuffer below), SHARED across every clip
// level (see NUM_CHUNK_LEVELS, Builtin.SdfFieldConfig.inc.glsl). 16384 is
// the per-level share -- must match the literal kMaxChunkBricksPerLevel in
// vulkan_raymarch_shader.cpp's kMaxChunkBricks = kMaxChunkBricksPerLevel *
// kNumLevels exactly (see that constant's own comment for why 16384, not
// the original, too-tight 4096) -- NUM_CHUNK_LEVELS itself already has to
// match kNumLevels there too, so multiplying by it here rather than
// hardcoding the product keeps this in sync automatically if kNumLevels/
// NUM_CHUNK_LEVELS ever changes together -- only the per-level 16384 needs
// updating by hand on both sides.
const int MAX_BRICKS = 16384 * NUM_CHUNK_LEVELS;

// Must match kMaxChunkClusters engine-side (and the identical constant in
// Builtin.ChunkPointSplat.comp.glsl / Builtin.ChunkEvict.comp.glsl) -- the
// size of the shared cluster page pool this shader allocates from.
const int MAX_CLUSTERS = 131072;

// How many cluster pages are held back for FIRST pages only.
//
// A brick claims up to CHUNK_MAX_CLUSTERS_PER_BRICK pages, greedily, in bake
// order. Without a reserve the bricks baked first can drain the pool and
// leave later ones with nothing -- and a brick with NO points does not just
// render at a coarser LOD, it vanishes: it splats nothing, it posts no
// conservative near bound (that mechanism needs a cluster record to hang
// off), so its pixels take whatever splatted behind it.
//
// Extra pages are only granted while the free list is still deeper than
// this, so the reserve can only ever be spent on first pages. A brick
// refused a second page rolls its current LOD level back and keeps a
// complete, coarser point set -- the same graceful path an over-budget level
// already takes.
//
// SIZING THIS RIGHT MATTERS MORE THAN IT LOOKS. It was briefly MAX_BRICKS,
// on the reasoning that every brick-pool slot might one day want a page.
// That is true but useless: it locked away 62% of the pool permanently, so
// as soon as a scene's own bricks had consumed the rest, every subsequently
// baked brick was capped at a single page -- one cluster, the coarsest
// complete LOD level. The chunks baked first kept their four pages and
// looked right; every chunk RE-baked after a scene edit dropped to the
// coarse set, losing exactly the fine detail that distinguishes a smooth
// blend's rounding and a subtraction's cut edge from a plain union. "It
// only works on the first bake" was the shape of that bug.
//
// The real requirement is much smaller: a brick claims its first page
// immediately after claiming the brick itself, so the only bricks that can
// be holding a slot WITHOUT a page are the ones being baked right now, in
// this dispatch. Reserving a comfortable multiple of a batch's worth of
// bricks covers that with room to spare, and leaves the rest of the pool
// free for the extra pages that carry the detail.
const int CLUSTER_FIRST_PAGE_RESERVE = 16384;

// Sized kMaxResidentChunks * CHUNK_CELL_COUNT engine-side -- one
// CHUNK_CELL_COUNT-sized dense sub-block per resident chunk slot, indexed by
// each batch entry's chunk_slot (see ChunkVoxelizeBatchSlotBuffer below).
// Slot assignment (which world chunk owns which slot) is a CPU-side scheduling
// decision (see ChunkStreamingManager) this shader has no awareness of --
// it just writes wherever each entry's chunk_slot points.
layout(binding = 0) buffer ChunkIndirectionBuffer {
    int chunk_indirection[];
};

layout(binding = 1) buffer ChunkBrickPoolBuffer {
    float chunk_bricks[];
};

// Unconditionally incremented every time a cell decides it needs a brick,
// regardless of whether the free-list pop below actually succeeds -- mirrors
// Builtin.RaymarchVoxelize.comp.glsl's own BrickCounterBuffer exactly (see
// its comment): lets the CPU side detect "the chunked pool's demand exceeded
// its capacity" after a bake by comparing this against MAX_BRICKS, the same
// diagnostic check check_brick_overflow() already does for the old field.
// [0] = brick demand (see above). [1..] are bake-cost counters, tallied only
// when push.collect_stats is set -- what a chunk's milliseconds are actually
// spent on, which is otherwise pure guesswork from outside the shader:
//   [1] scene evaluations at cell centres (the "does this cell need a brick"
//       test -- one per CHUNK_CELL_COUNT cell, whether it gets a brick or not)
//   [2] scene evaluations at sub-block centres (the rejection probe below)
//   [3] voxels whose distance came from a real scene evaluation
//   [4] voxels filled arithmetically instead, from a rejected sub-block
//   [5] splat point-pyramid positions visited
//   [6] voxels COPIED from an aliased cell instead of evaluated at all
layout(binding = 2) buffer ChunkBrickDemandBuffer {
    uint chunk_brick_demand[];
};

// A stack of free brick indices (see BrickFreeListTopBuffer below for the
// stack pointer) -- CPU-initialized to [0, 1, 2, ..., MAX_BRICKS-1] with the
// top pointer at MAX_BRICKS every time the chunked field starts fresh (see
// VulkanRaymarchShader's chunked-field init), then popped from here instead
// of Builtin.RaymarchVoxelize.comp.glsl's monotonic atomicAdd -- this is
// what makes an evicted chunk's bricks reusable by a future chunk bake (a
// later phase pushes freed indices back onto this same stack on eviction;
// this shader only ever pops).
layout(binding = 3) buffer BrickFreeListBuffer {
    int free_list[];
};

// How many entries free_list currently holds, from the top. Popping is
// atomicAdd(free_list_top, -1): the returned (pre-decrement) value is how
// many entries existed before this pop, so a fetched value of N means the
// entry this invocation claimed is free_list[N-1], and popping when N was
// already 0 wraps to a huge unsigned value -- both handled in main() below,
// exactly mirroring how the old path's brick_index >= MAX_BRICKS overflow
// check works today.
layout(binding = 4) buffer BrickFreeListTopBuffer {
    uint free_list_top;
};

// The analytic scene SDF -- identical shared include to Builtin.
// RaymarchVoxelize.comp.glsl (see this file's own header comment for why
// this one dependency IS shared safely).
#define SDF_PRIMITIVE_BUFFER_BINDING 5
#define SDF_LAYER_BUFFER_BINDING 7
#define SDF_PARAM_EXPR_BUFFER_BINDING 8
#include "Builtin.SdfSceneCommon.inc.glsl"

layout(binding = 6) buffer ChunkBrickPrimitiveBuffer {
    int chunk_brick_primitive[];
};

// One dispatch now bakes an entire BATCH of chunks at once (see this
// shader's own header comment update below and VulkanRaymarchShader::
// update_streaming()'s comment for why): CPU-side chunk-streaming
// decisions are completely unchanged (still ChunkStreamingManager, still
// pure CPU, still unit-tested), but instead of one vkCmdDispatch+push-
// constants pair per chunk, the CPU now writes every chunk this frame
// needs (re)baked into these two buffers once and issues a SINGLE
// dispatch sized to cover all of them -- collapsing what used to be
// dozens of small dispatch/barrier pairs (one per chunk) into one.
// Indexed by batch_index (see main() below), NOT chunk_slot --
// batch_index is this entry's position in THIS dispatch's batch, the
// chunk_slot VALUE it reads out of ChunkVoxelizeBatchSlotBuffer is still
// which ChunkIndirectionBuffer/ChunkBrickPoolBuffer sub-block that
// entry's chunk owns, exactly as before.
//
// TWO parallel arrays, not one array of a 5-scalar struct: std430 rounds
// a STRUCT's base alignment (and so its array stride) up to vec4 (16
// bytes) regardless of its members' own sizes -- see Builtin.
// SdfSceneCommon.inc.glsl's Primitive/Layer for why every struct in this
// codebase's SSBOs is already built from vec4/ivec4 fields only, never a
// bare scalar mix like {int, float, float, float, float} (20 bytes: the
// engine-side upload would write entries packed at 20-byte intervals,
// but the GPU would read them back at a rounded-up 32-byte stride --
// silent, severe corruption of every entry after the first, not a crash).
// A plain int[] and a plain vec4[] each have a well-defined, padding-free
// std430 stride (4 and 16 bytes, matching their C++ mirrors' sizeof()
// exactly), so splitting into two arrays sidesteps the rule entirely
// instead of fighting it with manual padding fields.
layout(binding = 9) buffer ChunkVoxelizeBatchSlotBuffer {
    int batch_chunk_slot[];
};
// xyz = chunk_world_min (this chunk's min corner in RENDER space), w =
// chunk_cell_size (this chunk's own fine-voxel cell size) -- packed
// together since both are per-entry and this keeps it to two buffers
// total rather than three.
layout(binding = 10) buffer ChunkVoxelizeBatchDataBuffer {
    vec4 batch_chunk_data[];
};

// --- Splat point clusters (step 2; see CHUNK_CLUSTER_POINTS in Builtin.
// SdfFieldConfig.inc.glsl for the layout and why clusters exist at all).
// The pool works exactly like the brick pool above: a free list of equal
// sized pages, popped here and pushed back by Builtin.ChunkEvict.comp.glsl.
// A brick claims one page per 256 points its surface needs.

// kMaxChunkClusters * CHUNK_CLUSTER_POINTS uvec2s -- point x is a packed
// position, y a packed normal (see pack_point_normal()).
layout(binding = 11) buffer ChunkClusterPointBuffer {
    uvec2 cluster_points[];
};

// One record per cluster page (see ChunkCluster). meta.y == 0 marks a page
// on the free list, which is what lets every consumer walk the whole pool
// without a residency structure of its own.
layout(binding = 12) buffer ChunkClusterBuffer {
    ChunkCluster chunk_clusters[];
};

// Cluster page free list, same stack discipline (and same top-down pop
// arithmetic) as BrickFreeListBuffer above.
layout(binding = 13) buffer ChunkClusterFreeListBuffer {
    int cluster_free_list[];
};
layout(binding = 14) buffer ChunkClusterFreeTopBuffer {
    uint cluster_free_top;
};

// Which cluster pages each brick owns: CHUNK_MAX_CLUSTERS_PER_BRICK entries
// per brick, -1 for unused. Written here, read by Builtin.ChunkEvict.comp.
// glsl -- freeing a brick has to know which pages went with it, and a page
// record's own brick_index is not enough to search by without scanning the
// whole pool.
layout(binding = 15) buffer ChunkBrickClusterBuffer {
    int brick_clusters[];
};

// Per-chunk primitive candidate lists (see chunk_candidate_buffer_'s comment
// engine-side). candidate_range[batch_index] is (start, count, _, _) into
// chunk_candidates; a count of -1 means the CPU could not produce a usable
// list for that chunk (too many overlaps, or an unbounded primitive), and
// the whole scene is folded instead. Each entry packs a primitive index in
// its low 16 bits and that primitive's LAYER index in its high 16 -- walking
// a candidate list means a sample can no longer take its operation and
// smoothness from an enclosing layer loop, so each candidate has to carry
// where it belongs.
layout(binding = 16) readonly buffer ChunkCandidateBuffer {
    int chunk_candidates[];
};
// One entry per ChunkCandidateBuffer entry, same index. xyz = the LOCAL-
// space offset of whichever repetition instance this entry names; w != 0
// marks the entry as instance-resolved, meaning the shader must evaluate
// that single copy directly (primitive_sdf_at_instance()) instead of
// running the primitive's own repeat_*() fold.
//
// w == 0 is the unresolved fallback, behaviourally identical to what this
// shader did before candidates carried instances at all: used for infinite
// and rotational repetition, for a primitive with no repetition to resolve,
// and whenever the CPU declined to split the copies apart (several reach
// the chunk AND the owning layer has a non-zero smoothness, where folding
// them one at a time would blend them into each other rather than take the
// min repeat_limited() takes).
layout(binding = 18) readonly buffer ChunkCandidateOffsetBuffer {
    vec4 candidate_offset[];
};

// One entry per cell of every batch entry: -1 means "bake this cell
// normally", anything else is the GLOBAL cell index (slot *
// CHUNK_CELL_COUNT + cell) of another cell that bakes to bit-identical
// voxel data -- possibly in another chunk, possibly baked frames ago.
//
// Repeated architecture makes most of a coarse chunk redundant. A level-4
// chunk is 64 world units of 4-unit cells; a 10-unit repetition period puts
// cells 5 apart in X and Z into the same position relative to the tiling, so
// they see exactly the same scene and evaluate to exactly the same 18^3
// distances. Out of 256 cells in an XZ plane, only about 25 are distinct.
// The CPU works out which (see VulkanRaymarchShader::build_cell_alias_map())
// and this pass turns that into a memory copy instead of ~2,000 scene
// evaluations per cell.
//
// Resolved in TWO dispatches, because an alias has to read a brick the
// representative wrote and invocations within one dispatch have no ordering:
// push.pass 0 bakes representatives, push.pass 1 copies aliases, with a
// barrier between.
layout(binding = 19) readonly buffer ChunkCellAliasBuffer {
    int cell_alias[];
};

layout(binding = 17) readonly buffer ChunkCandidateRangeBuffer {
    ivec4 candidate_range[];
};

layout(push_constant) uniform PushConstants {
    int layer_count;
    float max_smoothness; // See CULL_RADIUS_CELLS below -- same role as the
                         // old voxelizer's identical push constant. Scene-
                         // wide, so unlike batch_chunk_slot/batch_chunk_data
                         // above, this stays a plain push constant -- every
                         // chunk in the batch shares the same value.
    int batch_count;      // How many batch_chunk_slot[]/batch_chunk_data[]
                         // entries this dispatch actually populated --
                         // gl_WorkGroupID.z ranges wider than this (see
                         // main()'s own comment), so every invocation must
                         // bounds-check against it.
    int batch_offset;     // Where this submission's region of the batch
                         // arrays starts. The arrays hold one region per
                         // async ring slot so that a new batch can be
                         // written while an older one is still being read
                         // by the GPU -- see the buffers' creation comment
                         // engine-side.
    int pass;             // 0 = bake representative cells, 1 = copy aliased
                         // cells from them. See ChunkCellAliasBuffer.
                         //
                         // MUST STAY IN THIS POSITION. The block is matched
                         // field-for-field against ChunkVoxelizePush
                         // Constants engine-side; dropping or reordering one
                         // int silently shifts every field after it (this
                         // has happened, and it presented as missing bricks
                         // with nothing logged).
    int collect_stats;    // Non-zero to tally the bake-cost counters in
                         // ChunkBrickDemandBuffer -- see its comment. Off
                         // in normal operation; the atomics are cheap
                         // individually but there are millions of them.
} push;

// Same rationale/tuning as Builtin.RaymarchVoxelize.comp.glsl's identical
// constant -- see its own comment.
const float CULL_RADIUS_CELLS = 6.0;

// scene_map() restricted to one chunk's candidate list -- a deliberate
// parallel of the shared one in Builtin.SdfSceneCommon.inc.glsl (which this
// file still uses for the fallback), not a replacement for it.
//
// Same result, same fold order: primitives are uploaded grouped by layer in
// ascending layer order, and a candidate list is built by walking them in
// that same order, so iterating the list folds exactly the sequence
// scene_map() would have folded -- minus the ones that provably cannot
// reach this chunk. That is the whole saving: no per-sample bounding-sphere
// test against every primitive in the scene, because the question was
// already answered once for the entire chunk.
float scene_map_candidates(vec3 p, int start, int count,
                           out int nearest_primitive) {
    float running = 1e30;
    int running_material = -1;
    for (int i = 0; i < count; ++i) {
        int packed = chunk_candidates[start + i];
        int idx = packed & 0xFFFF;
        int layer_i = (packed >> 16) & 0xFFFF;
        int op = int(layers[layer_i].op_smoothness.x);
        float smoothness = layers[layer_i].op_smoothness.y;

        vec4 offset = candidate_offset[start + i];
        float d = offset.w != 0.0
            ? primitive_sdf_at_instance(idx, p, offset.xyz)
            : primitive_sdf(idx, p);
        float h;
        float combined = (op == 1) ? smooth_subtraction(d, running, smoothness, h)
                                   : smooth_union(d, running, smoothness, h);
        running_material = (h > 0.5) ? idx : running_material;
        running = combined;
    }
    nearest_primitive = running_material;
    return running;
}

// Splat point generation for one brick, factored out of main() so both the
// bake path and the ALIAS path can run it (see cell_alias below): an aliased
// cell copies its neighbour's voxels rather than evaluating them, but it
// still needs its own points, because a cluster record carries the owning
// cell's absolute box and two cells are by definition in different places.
//
// Deliberately regenerates rather than sharing pages. Sharing would need the
// cluster page pool refcounted and the record-to-page relationship broken
// 1:1, which is most of the splat pipeline; regenerating costs only brick
// reads, which is nothing next to the scene evaluations the alias skipped.
void generate_splat_points(int brick_index, int chunk_slot, vec3 cell_min,
                           float chunk_cell_size, float voxel_size, bool stats) {
    // --- Splat point generation into cluster pages. ---
    // Builds the progressive LOD pyramid described by CHUNK_POINTS_
    // PER_BRICK (Builtin.SdfFieldConfig.inc.glsl): one pass per
    // level, coarsest stride first, each pass adding only the sample
    // positions its stride introduces that a COARSER stride didn't
    // already emit (that "already covered" test is just `every axis
    // index is a multiple of the previous, coarser stride`). Level
    // L's point set is then the brick's first lod_counts[L] points,
    // wherever across this brick's pages they fall.
    //
    // Distances come straight from the voxels the loop above just
    // wrote (same invocation, program order -- no barrier needed),
    // and each kept candidate is projected onto the isosurface by a
    // single Newton step along the central-difference gradient --
    // the apron guarantees every +-1 neighbor read stays in bounds.
    // That same gradient is the point's baked normal, so normals
    // cost nothing here and save the shading pass from re-evaluating
    // the field per splatted pixel.
    //
    // The keep test scales with the LEVEL's own spacing, so a coarse
    // level still finds the surface everywhere a fine one does (it
    // just describes it with fewer, wider-spaced points) -- and
    // because that threshold SHRINKS with the stride, a position a
    // coarse level rejected can never be one a finer level wanted,
    // which is what makes skipping "already covered" positions safe
    // rather than a hole in the finer set.
    //
    // A LEVEL THAT DOESN'T FIT IS ROLLED BACK WHOLE, never left half
    // emitted -- whether it ran out of this brick's cluster budget
    // or the shared pool ran dry. Emitting a level partway leaves a
    // contiguous slab of the brick (the tail of this z-major walk)
    // with no points while the rest has them, and neither consumer
    // can recover from that: both trust the nearest splat on a
    // pixel, so a patch that lost its points is shaded, or primed
    // past, by whatever splatted BEHIND it -- the near surface just
    // disappears. Rolling back returns the pages that level claimed
    // and leaves the brick with a complete, gap-free point set at a
    // coarser spacing, which the splat pass reads off lod_counts.
    int page_ids[CHUNK_MAX_CLUSTERS_PER_BRICK];
    int page_count = 0;
    int point_count = 0;
    uvec4 lod_counts = uvec4(0);
    vec3 normal_sum = vec3(0.0);

    for (int level = 0; level < CHUNK_POINT_LOD_COUNT; ++level) {
        int stride = chunk_point_lod_stride(level);
        // What the previous (coarser) pass already covered; for the
        // first pass nothing has been, which `stride * 2` beyond
        // the brick can never match.
        int covered = stride * 2;
        float level_spacing = voxel_size * float(stride);
        int level_start = point_count;   // rollback point
        int level_start_pages = page_count;
        bool overflowed = false;
        for (int pz = 0; pz < CHUNK_BRICK_DIM && !overflowed; pz += stride) {
            for (int py = 0; py < CHUNK_BRICK_DIM && !overflowed; py += stride) {
                for (int px = 0; px < CHUNK_BRICK_DIM && !overflowed; px += stride) {
                    if (level > 0 && (px % covered) == 0 && (py % covered) == 0 &&
                        (pz % covered) == 0) {
                        continue; // a coarser level already emitted this position
                    }
                    if (stats) { atomicAdd(chunk_brick_demand[5], 1u); }
                    ivec3 s = ivec3(px, py, pz) + ivec3(1);
                    int base = brick_index * CHUNK_BRICK_VOXEL_COUNT;
                    #define BRICK_AT(ix, iy, iz) chunk_bricks[base + (ix) + \
                        (iy) * CHUNK_BRICK_APRON_DIM + \
                        (iz) * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM]
                    float d = BRICK_AT(s.x, s.y, s.z);
                    if (abs(d) > level_spacing * 0.8660254) {
                        continue;
                    }
                    if (point_count >= CHUNK_POINTS_PER_BRICK) {
                        overflowed = true; // this brick's own budget
                        break;
                    }
                    if ((point_count % CHUNK_CLUSTER_POINTS) == 0) {
                        // Time for another page. Same top-down pop as
                        // the brick free list above, including the
                        // undo of the wraparound decrement when the
                        // stack is already empty -- plus the reserve
                        // that keeps a first page available for every
                        // brick (see CLUSTER_FIRST_PAGE_RESERVE).
                        uint floor_count = point_count == 0
                            ? 0u
                            : uint(CLUSTER_FIRST_PAGE_RESERVE);
                        uint free_before = atomicAdd(cluster_free_top, 0xFFFFFFFFu);
                        if (free_before == 0u || free_before > uint(MAX_CLUSTERS) ||
                            free_before <= floor_count) {
                            atomicAdd(cluster_free_top, 1u);
                            overflowed = true; // pool dry, or this
                                              // brick already has
                                              // its guaranteed page
                            break;
                        }
                        page_ids[page_count] = cluster_free_list[free_before - 1u];
                        ++page_count;
                    }
                    vec3 grad = vec3(
                        BRICK_AT(s.x + 1, s.y, s.z) - BRICK_AT(s.x - 1, s.y, s.z),
                        BRICK_AT(s.x, s.y + 1, s.z) - BRICK_AT(s.x, s.y - 1, s.z),
                        BRICK_AT(s.x, s.y, s.z + 1) - BRICK_AT(s.x, s.y, s.z - 1));
                    #undef BRICK_AT
                    vec3 p = cell_min + (vec3(px, py, pz) + 0.5) * voxel_size;
                    float grad_len = length(grad);
                    vec3 normal = vec3(0.0, 1.0, 0.0);
                    if (grad_len > 1e-6) {
                        normal = grad / grad_len;
                        p -= normal * d;
                    }
                    // Pack as a 3x10-bit fraction of the cell -- the
                    // projection can step slightly outside it, so
                    // clamp; the splat pass's own conservative
                    // margins absorb the clamp error.
                    vec3 frac = clamp((p - cell_min) / chunk_cell_size, 0.0, 1.0);
                    uvec3 q = uvec3(frac * 1023.0 + 0.5);
                    int page = page_ids[point_count / CHUNK_CLUSTER_POINTS];
                    int slot = page * CHUNK_CLUSTER_POINTS +
                        (point_count % CHUNK_CLUSTER_POINTS);
                    cluster_points[slot] = uvec2(
                        q.x | (q.y << 10) | (q.z << 20),
                        pack_point_normal(normal));
                    normal_sum += normal;
                    ++point_count;
                }
            }
        }
        if (overflowed) {
            // Hand back only the pages THIS level claimed; earlier
            // levels' points (and the partially filled page they may
            // sit in) stay exactly as they were.
            while (page_count > level_start_pages) {
                --page_count;
                uint slot = atomicAdd(cluster_free_top, 1u);
                if (slot < uint(MAX_CLUSTERS)) {
                    cluster_free_list[slot] = page_ids[page_count];
                }
            }
            point_count = level_start;
            for (int rest = level; rest < CHUNK_POINT_LOD_COUNT; ++rest) {
                lod_counts[rest] = uint(level_start);
            }
            break;
        }
        lod_counts[level] = uint(point_count);
    }

    // Normal cone over the brick's points, shared by all its
    // pages: the axis is the mean normal, accumulated as the points
    // were generated.
    //
    // The half angle is NOT measured. Doing so needs a second pass
    // over the points that were just written, and those live in a
    // HOST-VISIBLE buffer -- so that pass reads a few hundred
    // scattered dwords back across PCIe per brick, hundreds of
    // bricks per chunk. Measured, that read-back was most of a bake
    // that ran 100-300ms per chunk and stalled the async queue for
    // seconds at a time on every chunk boundary.
    //
    // A cone of -1 means "spans every direction", which the cull
    // pass reads as "never cull this cluster" (its slack exceeds 90
    // degrees, and the test is guarded for exactly that). So the
    // trade is: give up backface rejection of whole clusters, keep
    // the per-POINT backface test that already runs in the splat
    // pass, and get the bake back. Restore this the day the point
    // pool is device-local -- then the read is cheap and the cone
    // is worth measuring again.
    vec3 cone_axis = normal_sum;
    float cone_len = length(cone_axis);
    cone_axis = cone_len > 1e-6 ? cone_axis / cone_len : vec3(0.0, 1.0, 0.0);
    float cone_cos = -1.0;

    // meta.w carries BOTH this cluster's clip level (low 16 bits)
    // and the gpu_slot of the chunk that owns it (high 16). The
    // slot is what Builtin.ChunkClusterCull.comp.glsl needs to
    // answer "has this chunk's bake actually finished and been
    // published?" -- the whole cluster pool is swept flat every
    // frame, with no residency structure, so without a back
    // reference to the owning chunk there is no way for the cull
    // to tell a finished page from one this very bake is still
    // filling in. See CHUNK_MAX_SLOTS there. The clip level was
    // already stored here and is kept (nothing reads it yet, but
    // it costs nothing to preserve); slot fits easily, the pool is
    // NUM_CHUNK_LEVELS * kMaxResidentChunks entries.
    int clip_level = int(round(log2(chunk_cell_size / COARSE_CELL_SIZE)));
    uint level_and_slot =
        uint(clip_level) | (uint(chunk_slot) << 16);
    for (int i = 0; i < CHUNK_MAX_CLUSTERS_PER_BRICK; ++i) {
        int page = i < page_count ? page_ids[i] : -1;
        brick_clusters[brick_index * CHUNK_MAX_CLUSTERS_PER_BRICK + i] = page;
        if (page < 0) {
            continue;
        }
        int first = i * CHUNK_CLUSTER_POINTS;
        chunk_clusters[page].bbox_min_cell = vec4(cell_min, chunk_cell_size);
        chunk_clusters[page].normal_cone = vec4(cone_axis, cone_cos);
        chunk_clusters[page].lod_counts = lod_counts;
        // meta LAST, behind a barrier: meta.y != 0 is what marks a
        // page live to every consumer, and the cull/splat passes
        // now read this pool WHILE a bake is still writing it (the
        // graphics queue no longer stalls on the bake -- see
        // VulkanRaymarchShader::publish_completed_bakes()). Without
        // the barrier a reader could see the "live" flag land
        // before the bbox and point counts it depends on, and splat
        // a cluster described by whatever the page held before.
        memoryBarrierBuffer();
        chunk_clusters[page].meta = uvec4(
            uint(first),
            uint(min(CHUNK_CLUSTER_POINTS, point_count - first)),
            uint(brick_index), level_and_slot);
    }
}

void main() {
    bool stats = push.collect_stats != 0;

    // Z carries TWO things, packed by the dispatch shape (CPU issues
    // (CHUNK_COARSE_DIM/4, CHUNK_COARSE_DIM/4, (CHUNK_COARSE_DIM/4) *
    // batch_count) workgroups -- see VulkanRaymarchShader::update_
    // streaming()'s own comment): which chunk-local Z coarse cell (low
    // CHUNK_COARSE_DIM of it, same as X/Y always were) and which entry in
    // batch_chunk_slot[]/batch_chunk_data[] this invocation's chunk is
    // (the rest, divided out).
    int batch_index = int(gl_GlobalInvocationID.z) / CHUNK_COARSE_DIM;
    if (batch_index >= push.batch_count) {
        return; // dispatch is always a whole multiple of CHUNK_COARSE_DIM
               // groups in Z, so this only trims true overshoot, never
               // splits a real chunk's own Z range.
    }
    ivec3 local_cell = ivec3(gl_GlobalInvocationID.xy,
                             int(gl_GlobalInvocationID.z) % CHUNK_COARSE_DIM);
    if (local_cell.x >= CHUNK_COARSE_DIM || local_cell.y >= CHUNK_COARSE_DIM ||
        local_cell.z >= CHUNK_COARSE_DIM) {
        return;
    }

    int chunk_slot = batch_chunk_slot[push.batch_offset + batch_index];
    vec4 chunk_data = batch_chunk_data[push.batch_offset + batch_index];
    int local_cell_index = local_cell.x + local_cell.y * CHUNK_COARSE_DIM +
        local_cell.z * CHUNK_COARSE_DIM * CHUNK_COARSE_DIM;
    int cell_index = chunk_slot * CHUNK_CELL_COUNT + local_cell_index;

    vec3 chunk_world_min = chunk_data.xyz;
    float chunk_cell_size = chunk_data.w;

    vec3 cell_min = chunk_world_min + vec3(local_cell) * chunk_cell_size;
    vec3 cell_center = cell_min + vec3(chunk_cell_size * 0.5);

    float half_diagonal = chunk_cell_size * 0.8660254;

    // This decides whether the cell gets a brick at all, and the only thing
    // the answer is compared against is half_diagonal -- so the evaluation
    // never needs to distinguish "far" from "very far", and a cull radius
    // covering that threshold plus the widest blend reach is a genuine
    // upper bound on anything that could change the outcome. A primitive
    // beyond it cannot pull the surface into this cell.
    //
    // It used to pass UNBOUNDED_BOUNDING_RADIUS, which meant fully
    // evaluating every primitive in the scene at all 4,096 cell centres of
    // every chunk baked -- rotation, domain repetition, deformation and the
    // parametric-expression VM included, for primitives metres away. That
    // is pure loss on a scene of any size, and this is one of the two loops
    // that made a chunk bake cost 100-300ms.
    // This chunk's candidate list, or (-1) meaning "no list, fold the whole
    // scene" -- see ChunkCandidateBuffer.
    ivec4 candidates = candidate_range[push.batch_offset + batch_index];
    int candidate_start = candidates.x;
    int candidate_count = candidates.y;

    int nearest_primitive;
    float center_dist =
        candidate_count >= 0
            ? scene_map_candidates(cell_center, candidate_start,
                                   candidate_count, nearest_primitive)
            : scene_map(cell_center, push.layer_count,
                        half_diagonal + push.max_smoothness, nearest_primitive);

    // Which pass owns this cell -- see ChunkCellAliasBuffer. Both passes
    // dispatch over the whole grid and each drops the cells that aren't
    // theirs, which costs one buffer read for the majority that exit here.
    int alias_src = cell_alias[(push.batch_offset + batch_index) * CHUNK_CELL_COUNT +
                               local_cell_index];
    if (push.pass == 0 ? (alias_src >= 0) : (alias_src < 0)) {
        return;
    }

    if (push.pass == 1) {
        // --- Alias: this cell's field is identical to alias_src's, which
        // pass 0 has already baked and made visible by the barrier between
        // the two dispatches. Claim a brick and copy, skipping every scene
        // evaluation. ---
        chunk_indirection[cell_index] = -1;
        // alias_src is a GLOBAL cell index (slot * CHUNK_CELL_COUNT + cell),
        // not a local one: the representative this cell copies from may live
        // in a different chunk of this batch, or in a chunk baked frames ago
        // that is still resident. See build_cell_alias_map() engine-side --
        // that widening is what turns deduplication from a within-chunk
        // trick into "bake this geometry once, ever".
        int src_brick = chunk_indirection[alias_src];
        if (src_brick < 0) {
            return; // representative had no surface, so neither has this one
        }
        atomicAdd(chunk_brick_demand[0], 1u);
        uint alias_free_before = atomicAdd(free_list_top, 0xFFFFFFFFu);
        if (alias_free_before == 0u || alias_free_before > uint(MAX_BRICKS)) {
            atomicAdd(free_list_top, 1u); // undo, same as the bake path
            return;
        }
        int brick_index = free_list[alias_free_before - 1u];
        chunk_indirection[cell_index] = brick_index;
        chunk_brick_primitive[brick_index] = chunk_brick_primitive[src_brick];
        int dst_base = brick_index * CHUNK_BRICK_VOXEL_COUNT;
        int src_base = src_brick * CHUNK_BRICK_VOXEL_COUNT;
        for (int i = 0; i < CHUNK_BRICK_VOXEL_COUNT; ++i) {
            chunk_bricks[dst_base + i] = chunk_bricks[src_base + i];
        }
        if (stats) {
            atomicAdd(chunk_brick_demand[6], uint(CHUNK_BRICK_VOXEL_COUNT));
        }
        float alias_voxel_size = chunk_cell_size / float(CHUNK_BRICK_DIM);
        generate_splat_points(brick_index, chunk_slot, cell_min,
                              chunk_cell_size, alias_voxel_size, stats);
        return;
    }

    if (stats) { atomicAdd(chunk_brick_demand[1], 1u); }

    chunk_indirection[cell_index] = -1;

    if (nearest_primitive >= 0 && abs(center_dist) <= half_diagonal) {
        atomicAdd(chunk_brick_demand[0], 1u);

        // Pop one index off free_list (see BrickFreeListBuffer's own
        // comment for exactly why this arithmetic is correct, including
        // the empty-stack case).
        uint free_count_before = atomicAdd(free_list_top, 0xFFFFFFFFu);
        if (free_count_before > 0u && free_count_before <= uint(MAX_BRICKS)) {
            int brick_index = free_list[free_count_before - 1u];
            chunk_indirection[cell_index] = brick_index;
            chunk_brick_primitive[brick_index] = nearest_primitive;

            // CHUNK_BRICK_DIM, not BRICK_DIM -- see its own comment in
            // Builtin.SdfFieldConfig.inc.glsl for why the chunked field's
            // brick resolution is a separate, finer constant.
            float voxel_size = chunk_cell_size / float(CHUNK_BRICK_DIM);
            float cull_radius = chunk_cell_size * CULL_RADIUS_CELLS + push.max_smoothness;
            // Local index range is -1..CHUNK_BRICK_DIM inclusive (the
            // apron), mapped to storage index range 0..CHUNK_BRICK_APRON_
            // DIM-1 via +1 -- identical scheme to the old voxelizer's brick
            // population.
            // Walked as sub-blocks rather than one flat sweep, so a
            // sub-block provably clear of the surface can be filled
            // arithmetically instead of evaluated.
            //
            // The flat sweep is CHUNK_BRICK_APRON_DIM^3 = 5,832 scene
            // evaluations per brick, and on architectural content most of
            // them answer "still nowhere near anything". A wall crossing a
            // brick puts the surface through a slab of it; the rest is
            // solid interior or open air, and both are exactly what a
            // distance test dismisses wholesale. One evaluation at a
            // sub-block's centre answers it for all of its voxels.
            //
            // THE FILL IS A CONSERVATIVE UNDER-ESTIMATE, not an
            // interpolation: an SDF changes by at most the distance
            // travelled, so d(centre) - |x - centre| is never larger than
            // the true distance at x. Reading low is the safe direction
            // everywhere this field is consumed -- a sphere-tracing step
            // shortens rather than overshooting a surface, and the splat
            // generator's own |d| <= spacing keep-test simply finds no
            // points, which is correct where there is no surface.
            //
            // SUB_BLOCK_DIM IS 3, NOT 6, AND THAT MATTERS MORE THAN IT
            // LOOKS. The threshold a sub-block must clear scales with its
            // own circumradius, so a 6^3 sub-block (radius 4.33 voxels,
            // 8.66 after the safety factor) can only be rejected if its
            // centre is nearly half a brick from the surface -- which is
            // almost never true of a brick that was allocated BECAUSE it
            // contains a surface. Measured with the counters above: at 6
            // the rejection barely fires on planar content, which is most
            // of an interior scene. At 3 the threshold is 3.46 voxels and
            // a plane through the brick leaves roughly two thirds of the
            // sub-blocks rejectable, for 216 probes instead of 27.
            //
            // SUB_BLOCK_SAFETY widens the threshold past the strict
            // circumradius because this engine's SDF is only approximately
            // distance-preserving: smooth unions, domain deformation and
            // several non-exact primitives can all report a distance that
            // shrinks faster than the step taken. The margin buys headroom
            // against that rather than trusting a Lipschitz bound the
            // scene never promised.
            const int SUB_BLOCK_DIM = 3; // CHUNK_BRICK_APRON_DIM / 6
            const int SUB_BLOCKS_PER_AXIS = CHUNK_BRICK_APRON_DIM / SUB_BLOCK_DIM;
            const float SUB_BLOCK_SAFETY = 2.0;
            // Half the distance between the first and last voxel CENTRE in
            // a sub-block, times sqrt(3) -- the farthest any of its voxels
            // sits from its centre.
            float sub_block_radius =
                float(SUB_BLOCK_DIM - 1) * 0.5 * voxel_size * 1.7320508;
            float sub_block_threshold = sub_block_radius * SUB_BLOCK_SAFETY;
            for (int sz = 0; sz < SUB_BLOCKS_PER_AXIS; ++sz) {
            for (int sy = 0; sy < SUB_BLOCKS_PER_AXIS; ++sy) {
            for (int sx = 0; sx < SUB_BLOCKS_PER_AXIS; ++sx) {
                // Storage indices [n*s, n*s+n-1] -> local indices shifted
                // by the apron's -1.
                ivec3 local_base = ivec3(sx, sy, sz) * SUB_BLOCK_DIM - ivec3(1);
                vec3 block_centre = cell_min +
                    (vec3(local_base) + float(SUB_BLOCK_DIM - 1) * 0.5 + 0.5) *
                        voxel_size;
                int unused_centre;
                float block_d =
                    candidate_count >= 0
                        ? scene_map_candidates(block_centre, candidate_start,
                                               candidate_count, unused_centre)
                        : scene_map(block_centre, push.layer_count,
                                    cull_radius, unused_centre);
                bool block_far = abs(block_d) > sub_block_threshold;
                float block_sign = block_d >= 0.0 ? 1.0 : -1.0;
                if (stats) {
                    atomicAdd(chunk_brick_demand[2], 1u);
                    atomicAdd(chunk_brick_demand[block_far ? 4 : 3],
                              uint(SUB_BLOCK_DIM * SUB_BLOCK_DIM * SUB_BLOCK_DIM));
                }

                for (int lz = 0; lz < SUB_BLOCK_DIM; ++lz) {
                for (int ly = 0; ly < SUB_BLOCK_DIM; ++ly) {
                for (int lx = 0; lx < SUB_BLOCK_DIM; ++lx) {
                    ivec3 local = local_base + ivec3(lx, ly, lz);
                    vec3 voxel_pos = cell_min + (vec3(local) + 0.5) * voxel_size;
                    float d;
                    if (block_far) {
                        d = block_sign * (abs(block_d) -
                                          distance(voxel_pos, block_centre));
                    } else {
                        int unused_nearest;
                        d = candidate_count >= 0
                                ? scene_map_candidates(voxel_pos, candidate_start,
                                                       candidate_count,
                                                       unused_nearest)
                                : scene_map(voxel_pos, push.layer_count,
                                            cull_radius, unused_nearest);
                    }
                    ivec3 store = local + ivec3(1);
                    int local_index = store.x + store.y * CHUNK_BRICK_APRON_DIM +
                        store.z * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM;
                    chunk_bricks[brick_index * CHUNK_BRICK_VOXEL_COUNT + local_index] = d;
                }
                }
                }
            }
            }
            }

            generate_splat_points(brick_index, chunk_slot, cell_min,
                                  chunk_cell_size, voxel_size, stats);
        } else {
            // Free list was already empty -- undo the wraparound decrement
            // so a later successful pop elsewhere doesn't read a bogus
            // (huge, wrapped) free_list_top forever. Leaves chunk_indirection
            // at -1 for this cell (no brick), same visible failure mode as
            // the old path's brick_index >= MAX_BRICKS case: a missing
            // patch of surface, not a crash -- chunk_brick_demand having
            // exceeded MAX_BRICKS is exactly what a caller checks
            // afterward to explain why.
            atomicAdd(free_list_top, 1u);
        }
    }
}
