#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_shader_atomic_int64 : require

// Point-cloud splatting against the chunked field -- the Dreams technique
// (Alex Evans, "Learning From Failure", SIGGRAPH 2015): the SDF stays the
// authoring and spatial structure, but primary visibility comes from
// splatting a point cloud sampled onto its isosurface rather than from
// marching a ray per pixel.
//
// ONE WORKGROUP PER VISIBLE CLUSTER, one thread per point slot. A cluster is
// a fixed-capacity page of CHUNK_CLUSTER_POINTS surface points (see Builtin.
// SdfFieldConfig.inc.glsl) that a brick claimed at bake time; a brick owns
// as many as its surface needed. That makes the unit of work here a
// constant-cost one, which is exactly what makes it schedulable: this
// dispatch is INDIRECT, over the list Builtin.ChunkClusterCull.comp.glsl
// compacted this frame, so its cost tracks what is on screen rather than how
// big the pool is.
//
// This one dispatch serves BOTH consumers, controlled by the caller's
// splat_mode (see VulkanRaymarchShader::set_splat_mode()) -- what it writes
// is identical either way, only Builtin.RaymarchShader.comp.glsl's reading
// of it differs:
//
//   Prime mode:      the raymarcher starts each ray just short of its
//                    pixel's nearest splat instead of at the camera,
//                    skipping the empty-space march. The image is exactly
//                    what an unprimed march produces; only cost changes.
//   Visibility mode: the raymarcher shades the winning splat directly --
//                    no primary march at all for a covered pixel. This is
//                    the actual Dreams-style renderer.
//
// VISIBILITY BUFFER. One uint64 per pixel: the high 32 bits hold the
// point's distance from the camera as raw float bits (for finite floats
// >= 0 that bit pattern orders identically to the float itself), the low
// 32 bits hold which point won, plus the spacing its splat was drawn at.
// A single atomicMin therefore resolves nearest-splat-wins AND carries the
// payload needed to shade the winner -- no depth pre-pass and no second
// buffer. Cleared to ~0 ("no splat here") every frame; a real payload can
// never collide with that sentinel since the largest possible id
// (MAX_CLUSTERS * CHUNK_CLUSTER_POINTS, 2^25) is far below 0xFFFFFFFF.
//
// STOCHASTIC LOD. Two things here are deliberately random per point -- but
// NOT per frame. Both hash the point's own identity only, so the pattern is
// fixed to the surface and travels with it.
//
// They were per-frame originally, on the Dreams reasoning that TAA
// (Builtin.TaaResolve.comp.glsl) would average successive subsets back into
// full density. It does not, because a pattern that decorrelates completely
// between frames is precisely what a neighbourhood-clamping resolve throws
// away: the history never accumulated, and the surface visibly boiled --
// static geometry under a stipple that swam across it. Fixing the seed
// costs the temporal refinement and buys a stable image, which on this
// resolve is the better half of the trade.
//
//   1. Russian roulette thinning. A cluster whose points project finer than
//      one splat per pixel keeps only a fraction of them and widens the
//      survivors' footprints to match. Cost tracks the SCREEN, not the
//      geometry. With a per-point seed a given point crosses its own keep
//      threshold once, as the camera approaches, instead of flickering
//      across it every frame.
//   2. Clip-level dithering. Where two clip levels overlap (the outermost
//      ring of a level's streaming window) a point picks its level by a
//      threshold weighted by how far into the transition band it sits, so
//      the change dissolves across the band rather than switching a whole
//      level at once.
//
// CLIP-LEVEL GATING. Every clip level is resident at once and their windows
// nest (level L's window sits entirely inside L+1's -- see
// sample_clipmap_field(), Builtin.ChunkedFieldCommon.inc.glsl), so the same
// surface carries points at several resolutions simultaneously. Splatting
// all of them would let a coarse level's points win over the fine level's
// on the same surface (they disagree by up to a coarse voxel), reading as
// speckled z-fighting. So a point is splatted only by the level that
// sample_clipmap_field() would itself pick at that position, recomputed
// here with the identical Chebyshev test.
//
// DECLINED CLUSTERS STILL OCCLUDE. Visibility mode refuses to splat a
// cluster whose points are too coarse for how big it is on screen
// (SPLAT_MAX_PX), intending those pixels to march instead. But "no splat
// from THIS cluster" is not "no splat on those pixels": splatting has no
// occlusion, so geometry BEHIND the declined cluster splats the very same
// pixels, and Builtin.RaymarchShader.comp.glsl -- which trusts the nearest
// splat it finds -- shades that instead. A close-up surface in front of a
// distant one would read as see-through. So a declined cluster writes a
// conservative NEAR BOUND (see SplatTileBoundBuffer) into every screen tile
// its box projects onto, and the render pass refuses any splat farther than
// its pixel's bound. Only declined clusters write bounds, so a scene whose
// clusters all splat pays nothing and loses no splats.

#include "Builtin.SdfFieldConfig.inc.glsl"

// Must match kMaxChunkClusters engine-side -- same constant/comment as
// Builtin.ChunkVoxelize.comp.glsl's.
const int MAX_CLUSTERS = 131072;

// X stride the engine's dispatch packs cluster indices with (cluster_index =
// WorkGroupID.y * this + WorkGroupID.x) -- purely to stay under Vulkan's
// per-dimension workgroup-count limit (65535), which MAX_CLUSTERS exceeds in
// one dimension. Must match the dispatch shape in VulkanRaymarchShader::
// render_to() exactly.
const int SPLAT_DISPATCH_STRIDE_X = 1024;

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
// local_size_x must equal CHUNK_CLUSTER_POINTS -- GLSL wants a literal here,
// so this is a manual sync point with Builtin.SdfFieldConfig.inc.glsl; there
// is no static_assert in GLSL, so the guard is the engine-side static_assert
// on kChunkClusterPoints instead.

layout(binding = 0) readonly buffer ChunkClusterPointBuffer {
    uvec2 cluster_points[];
};

layout(binding = 1) readonly buffer ChunkClusterBuffer {
    ChunkCluster chunk_clusters[];
};

// See this shader's VISIBILITY BUFFER note above. render_width_ x
// render_height_ entries (the RENDER resolution, not the swapchain's),
// cleared to ~0 by a vkCmdFillBuffer before every dispatch.
layout(binding = 2) buffer SplatVisibilityBuffer {
    uint64_t splat_visibility[];
};

// Conservative per-tile near bound, in SPLAT_TILE_SIZE-pixel tiles covering
// the render target (ceil(width/TILE) * ceil(height/TILE) entries, row
// major). Each entry is a float distance as raw bits, atomicMin'd -- the
// same "positive floats compare correctly as uints" trick the visibility
// buffer's depth half uses. ~0 means "nothing declined to splat over this
// tile", which is both the cleared state and the common case. See this
// shader's DECLINED CLUSTERS STILL OCCLUDE note.
layout(binding = 3) buffer SplatTileBoundBuffer {
    uint splat_tile_bound[];
};

// This frame's surviving clusters, densely packed by Builtin.
// ChunkClusterCull.comp.glsl, and its four-uint argument block (see that
// shader) -- [3] is the count, which bounds-checks the last, partially
// filled row of the indirect dispatch.
layout(binding = 4) readonly buffer VisibleClusterBuffer {
    uint visible_clusters[];
};
layout(binding = 5) readonly buffer ClusterCullArgsBuffer {
    uint cull_args[];
};

// Tile edge in pixels -- must match kSplatTileSize engine-side (it sizes the
// buffer) and Builtin.RaymarchShader.comp.glsl's copy. A coarser tile costs
// fewer atomics per declined cluster but rejects more good splats around
// that cluster's silhouette; 16 keeps both small.
const int SPLAT_TILE_SIZE = 16;

// Same camera basis Builtin.RaymarchShader.comp.glsl's main() builds its
// rays from -- this pass inverts that exact mapping (uv = (pixel - 0.5 *
// size) / size.y, dir = uv.x * right + uv.y * up + forward), so a point's
// splat lands on the pixel whose ray actually passes through it.
layout(push_constant) uniform PushConstants {
    vec4 camera_position; // xyz + unused pad
    vec4 camera_forward;  // xyz + unused pad
    vec4 camera_right;    // xyz + unused pad
    vec4 camera_up;       // xyz + unused pad
    int width;            // render-target size in pixels -- see
    int height;           // SplatVisibilityBuffer's comment above.
    int splat_mode;       // SPLAT_MODE_* -- only Visibility applies the
                         // too-sparse-to-splat rule below; see it.
    int frame_index;      // Cycles every frame. No longer feeds the
                         // stochastic decisions above -- see STOCHASTIC LOD
                         // for why those are seeded per point instead. Kept
                         // because the shadow pass shares this push layout
                         // and still uses it.
} push;

// Must match SPLAT_MODE_* in Builtin.RaymarchShader.comp.glsl / SplatMode
// engine-side.
const int SPLAT_MODE_PRIME = 1;
const int SPLAT_MODE_VISIBILITY = 2;

// The projected point spacing, in pixels, this pass aims each cluster at --
// i.e. how much screen one splat should cover. Dreams targeted "1 splat to 1
// screen pixel, or just under"; 1.0 here is that same target, and it is what
// the Russian roulette thinning aims at.
const float SPLAT_TARGET_PX = 1.0;

// How far past that target the FINEST available level may stretch before
// Visibility mode gives up on a cluster entirely and lets its pixels march
// instead (Prime mode never gives up -- a conservative starting distance
// doesn't care how coarse the point that produced it was).
//
// This is the rule that keeps close-up surfaces sharp, and it exists because
// a point cloud built from voxels bottoms out at one point per voxel: close
// enough to the camera, even that projects to many pixels, and no available
// level can describe the surface per-pixel any more. Rather than show a
// degraded approximation there, those clusters aren't splatted -- their
// pixels find no splat (but do find a near bound, see this shader's header),
// fall back to the raymarcher, and look exactly like they always did.
//
// THIS IS THE MAIN QUALITY/PERFORMANCE DIAL. Lower it to hand more of the
// screen back to the raymarcher -- sharper, slower. Raise it to let splats
// cover more -- faster, degrading into faceting on curved surfaces rather
// than flat colour blocks, since refine_splat_hit() (Builtin.RaymarchShader.
// comp.glsl) gives every pixel in a footprint its own position and UV.
const float SPLAT_MAX_PX = 4.0;

// How much of a level's outermost chunk ring is a transition band in which
// a point picks its clip level at random (weighted by depth into the band)
// rather than deterministically -- the splat analogue of LOD_BLEND_FRACTION
// (Builtin.ChunkedFieldCommon.inc.glsl).
//
// DISABLED (0.0), and not for performance. Dithering makes each level's
// coverage of the band stochastic: a fine-level point drops out with
// probability t and the coarse level's own, independently hashed points fill
// in only on average. The two are uncorrelated, so in any single frame the
// band contains gaps -- and a gap in splat coverage is not a pixel that
// falls back to marching, it is a pixel that takes whatever splatted BEHIND
// the surface. That reads as the geometry at every clip-level boundary going
// see-through, which is exactly the failure this renderer keeps having to
// design around.
//
// Re-enabling it needs a coverage-PRESERVING formulation: one where a point
// dropped by the fine level is replaced by a specific coarse point rather
// than by an independent coin flip -- or where the transition is a blend of
// two complete sets rather than a random choice between them.
const float SPLAT_LEVEL_DITHER_BAND = 0.0;

// Floor on the keep probability used to widen surviving splats. 1/sqrt(k)
// at k = 0.04 is a 5x widening, which is already generous for covering
// thinned-out neighbours; past that a survivor stops representing a
// neighbourhood and starts being a blob with a silhouette of its own.
const float SPLAT_MIN_KEEP_FOR_WIDENING = 0.04;

// Cheap per-point hash for the stochastic decisions -- decorrelated across
// points, clusters and frames, which is all TAA needs to average them out.
float splat_hash(uint a, uint b, uint c) {
    uint h = a * 0x9E3779B9u ^ b * 0x85EBCA6Bu ^ c * 0xC2B2AE35u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return float(h & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

// Which clip level a cluster belongs to, recovered from its own cell size:
// level L's cell is COARSE_CELL_SIZE * 2^L wide by construction
// (chunk_level_world_size(L) / CHUNK_COARSE_DIM -- see Builtin.
// SdfFieldConfig.inc.glsl), so the ratio is an exact power of two and this
// log2 is exact.
int level_of_cell_size(float cell_size) {
    return int(round(log2(cell_size / COARSE_CELL_SIZE)));
}

// The level sample_clipmap_field() (Builtin.ChunkedFieldCommon.inc.glsl)
// would pick at p: the finest whose streaming window contains it. MUST stay
// identical to that function's own loop -- see this shader's CLIP-LEVEL
// GATING note. `dither` in [0,1) picks stochastically inside the transition
// band at the outer ring of a level's window, mirroring what LOD_BLEND_
// FRACTION does continuously for the marched field. Returns
// NUM_CHUNK_LEVELS if p is outside every level's window.
int finest_level_at(vec3 p, vec3 camera_pos, float dither) {
    for (int level = 0; level < NUM_CHUNK_LEVELS; ++level) {
        float level_world_size = chunk_level_world_size(level);
        ivec3 chunk_coord = ivec3(floor(p / level_world_size));
        ivec3 camera_chunk = ivec3(floor(camera_pos / level_world_size));
        ivec3 delta = chunk_coord - camera_chunk;
        ivec3 abs_delta = abs(delta);
        int chebyshev = max(abs_delta.x, max(abs_delta.y, abs_delta.z));
        if (chebyshev > STREAM_RADIUS_CHUNKS) {
            continue;
        }
        if (SPLAT_LEVEL_DITHER_BAND > 0.0 &&
            chebyshev == STREAM_RADIUS_CHUNKS &&
            level + 1 < NUM_CHUNK_LEVELS) {
            // How far toward this chunk's away-from-camera face p sits,
            // along whichever axes are actually at the ring's limit -- the
            // identical construction sample_clipmap_field() blends with.
            vec3 chunk_center = (vec3(chunk_coord) + 0.5) * level_world_size;
            vec3 local = (p - chunk_center) / (level_world_size * 0.5);
            float outward = -1.0;
            if (abs_delta.x == chebyshev) {
                outward = max(outward, sign(float(delta.x)) * local.x);
            }
            if (abs_delta.y == chebyshev) {
                outward = max(outward, sign(float(delta.y)) * local.y);
            }
            if (abs_delta.z == chebyshev) {
                outward = max(outward, sign(float(delta.z)) * local.z);
            }
            float t = clamp((outward - (1.0 - SPLAT_LEVEL_DITHER_BAND)) /
                                SPLAT_LEVEL_DITHER_BAND,
                            0.0, 1.0);
            if (dither < t) {
                return level + 1;
            }
        }
        return level;
    }
    return NUM_CHUNK_LEVELS;
}

// Stamps a cluster's conservative near distance into every screen tile its
// bounding box can possibly cover -- the whole of the DECLINED CLUSTERS
// STILL OCCLUDE scheme above. The value is the distance from the camera to
// the box itself (not to any point in it), so it is a true lower bound on
// how near anything this cluster contains can be: a splat farther than that
// MIGHT be hidden by it, and the render pass marches rather than trusting
// the splat.
//
// The whole workgroup runs this together -- every thread computes the same
// (uniform) tile rectangle, then they split the tiles between them, since a
// cluster close enough to be declined can cover a large part of the screen.
void write_tile_bounds(vec3 box_min, vec3 box_max) {
    vec3 cam = push.camera_position.xyz;
    // Distance from the camera to the box (0 if inside it) -- the standard
    // clamped point-AABB distance.
    vec3 outside = max(max(box_min - cam, cam - box_max), vec3(0.0));
    uint bound_bits = floatBitsToUint(length(outside));

    int tiles_x = (push.width + SPLAT_TILE_SIZE - 1) / SPLAT_TILE_SIZE;
    int tiles_y = (push.height + SPLAT_TILE_SIZE - 1) / SPLAT_TILE_SIZE;

    // Project the 8 corners to get the covering pixel rectangle. A corner at
    // or behind the camera plane has no finite projection, and a box
    // straddling that plane can cover any part of the screen -- so that case
    // falls back to the whole target rather than to a rectangle built from
    // garbage. It only happens for a box the camera is essentially inside,
    // where the bound is ~0 anyway.
    vec2 lo = vec2(1e30);
    vec2 hi = vec2(-1e30);
    bool straddles = false;
    for (int i = 0; i < 8; ++i) {
        vec3 corner = vec3((i & 1) == 0 ? box_min.x : box_max.x,
                           (i & 2) == 0 ? box_min.y : box_max.y,
                           (i & 4) == 0 ? box_min.z : box_max.z);
        vec3 v = corner - cam;
        float z = dot(v, push.camera_forward.xyz);
        if (z < 0.05) {
            straddles = true;
            break;
        }
        vec2 uv = vec2(dot(v, push.camera_right.xyz),
                       dot(v, push.camera_up.xyz)) / z;
        vec2 pixel = uv * float(push.height) +
            0.5 * vec2(float(push.width), float(push.height));
        lo = min(lo, pixel);
        hi = max(hi, pixel);
    }

    ivec2 tile_lo = ivec2(0);
    ivec2 tile_hi = ivec2(tiles_x - 1, tiles_y - 1);
    if (!straddles) {
        tile_lo = clamp(ivec2(floor(lo)) / SPLAT_TILE_SIZE, ivec2(0), tile_hi);
        tile_hi = clamp(ivec2(ceil(hi)) / SPLAT_TILE_SIZE, tile_lo, tile_hi);
    }

    int span_x = tile_hi.x - tile_lo.x + 1;
    int total = span_x * (tile_hi.y - tile_lo.y + 1);
    for (int t = int(gl_LocalInvocationID.x); t < total;
         t += CHUNK_CLUSTER_POINTS) {
        int tx = tile_lo.x + (t % span_x);
        int ty = tile_lo.y + (t / span_x);
        atomicMin(splat_tile_bound[ty * tiles_x + tx], bound_bits);
    }
}

void main() {
    // The dispatch is rounded up to whole rows of SPLAT_DISPATCH_STRIDE_X
    // workgroups, so the last row overshoots the real count.
    uint visible_index = uint(gl_WorkGroupID.y) * uint(SPLAT_DISPATCH_STRIDE_X) +
        uint(gl_WorkGroupID.x);
    if (visible_index >= cull_args[3]) {
        return;
    }
    int cluster_index = int(visible_clusters[visible_index]);
    if (cluster_index >= MAX_CLUSTERS) {
        return;
    }

    ChunkCluster cluster = chunk_clusters[cluster_index];
    uint cluster_points_used = cluster.meta.y;
    if (cluster_points_used == 0u) {
        return; // page is on the free list
    }

    vec3 cell_min = cluster.bbox_min_cell.xyz;
    float cell_size = cluster.bbox_min_cell.w;
    float voxel_size = cell_size / float(CHUNK_BRICK_DIM);
    vec3 cell_center = cell_min + vec3(cell_size * 0.5);

    // Distance along the view axis, not euclidean: this is what the
    // perspective divide below actually scales by. Uniform across the
    // workgroup -- it is a property of the CLUSTER, not of any one point.
    float cluster_z = max(dot(cell_center - push.camera_position.xyz,
                              push.camera_forward.xyz),
                          0.05);
    float px_per_world = float(push.height) / cluster_z;

    // Deepest level the owning brick actually HAS its own point set for. A
    // level whose cumulative count merely repeats the previous level's added
    // nothing: either it found no new near-surface samples, or -- the common
    // case -- it didn't fit the brick's cluster budget and the bake rolled it
    // back whole. Either way the points on hand are the coarser level's, so
    // that is the level whose SPACING describes them, and sizing footprints
    // to a finer spacing than the points really have leaves gaps between
    // them. A gap is not a missing splat that falls back to marching: the
    // renderer trusts the nearest splat it finds, so a gap shows whatever is
    // BEHIND this surface.
    int deepest = CHUNK_POINT_LOD_COUNT - 1;
    while (deepest > 0 &&
           cluster.lod_counts[deepest] == cluster.lod_counts[deepest - 1]) {
        --deepest;
    }

    // Coarsest level whose projected spacing still meets the target; levels
    // run coarse (largest stride) to fine, so the first that fits is the
    // coarsest that does. Falls through to the finest level this brick HAS
    // when none fit -- i.e. when it is close enough that even one point per
    // voxel is stretched.
    int level = deepest;
    for (int L = 0; L < CHUNK_POINT_LOD_COUNT; ++L) {
        if (voxel_size * float(chunk_point_lod_stride(L)) * px_per_world <=
            SPLAT_TARGET_PX) {
            level = min(L, deepest);
            break;
        }
    }
    // A coarse level can be empty (nothing within its own keep threshold)
    // while finer ones aren't; stepping up to the first level that has points
    // keeps such a brick splatted instead of silently dropping it.
    while (level < deepest && cluster.lod_counts[level] == 0u) {
        ++level;
    }

    // Too close for even the finest level this brick has to resolve the
    // screen -- see SPLAT_MAX_PX, and this shader's header for why declining
    // still has to leave a mark.
    float finest_px = voxel_size * float(chunk_point_lod_stride(deepest)) *
        px_per_world;
    if (push.splat_mode == SPLAT_MODE_VISIBILITY && finest_px > SPLAT_MAX_PX) {
        // Only the clip level actually RESPONSIBLE for this region can
        // occlude anything: a coarser level's copy of the same surface is
        // never what sample_clipmap_field() sees there, so letting it post a
        // near bound would reject the fine level's perfectly good splats --
        // and since coarse levels are declined out to a much greater
        // distance, that alone would switch splatting off across most of the
        // view.
        if (level_of_cell_size(cell_size) ==
            finest_level_at(cell_center, push.camera_position.xyz, 0.5)) {
            write_tile_bounds(cell_min, cell_min + vec3(cell_size));
        }
        return;
    }

    // Past here every thread handles exactly one point of this cluster.
    uint local_index = gl_LocalInvocationID.x;
    if (local_index >= cluster_points_used) {
        return; // partially filled page (the brick's last one)
    }
    // Which point of the owning BRICK this is -- the LOD prefix is expressed
    // in those terms, since a level's point set can span several clusters.
    uint brick_point_index = cluster.meta.x + local_index;
    if (brick_point_index >= cluster.lod_counts[level]) {
        return;
    }

    float spacing = voxel_size * float(chunk_point_lod_stride(level));

    // Russian roulette thinning (see this shader's STOCHASTIC LOD note). If
    // this level's points project finer than one per pixel, keep a fraction
    // and widen the survivors to cover the same surface. Squared because
    // point density on a surface goes as 1/spacing^2.
    float projected_px = spacing * px_per_world;
    if (projected_px < SPLAT_TARGET_PX) {
        float keep = projected_px / SPLAT_TARGET_PX;
        keep = keep * keep;
        // SEEDED BY THE POINT, NOT BY THE FRAME. This used to mix
        // push.frame_index in, so every frame re-rolled which points
        // survived -- the surface was re-stippled from scratch 60 times a
        // second. The theory was that TAA would average the subsets back
        // into full density; in practice a pattern that decorrelates
        // completely between frames is exactly what TAA's neighbourhood
        // clamp rejects, so the history never accumulated and the result
        // boiled. That is the "painterly, moving splats" look: the geometry
        // is static, the stipple is not.
        //
        // Hashing only the point's own identity fixes the pattern TO THE
        // SURFACE. It still varies per point, so the thinning is still
        // stochastic rather than a visible lattice, but a given point makes
        // the same decision every frame and the dither moves with the
        // geometry instead of independently of it. Points now fade in
        // monotonically as the camera approaches (keep rises with
        // projected_px) rather than flickering across the threshold.
        if (splat_hash(uint(cluster_index), local_index, 0x9E3779B9u) > keep) {
            return;
        }
        // Survivors widen to cover the area the thinned-out points left, but
        // NOT without limit: keep goes to zero with distance, so an uncapped
        // 1/sqrt(keep) grows a lone survivor into a blob far larger than the
        // feature it belongs to. At a silhouette those oversized splats hang
        // past the true edge and read as a row of beads or teeth along it --
        // visible along any straight edge seen against a distant background.
        spacing /= sqrt(max(keep, SPLAT_MIN_KEEP_FOR_WIDENING));
    }

    uvec2 packed = cluster_points[uint(cluster_index) *
                                  uint(CHUNK_CLUSTER_POINTS) + local_index];
    vec3 frac = vec3(float(packed.x & 1023u), float((packed.x >> 10) & 1023u),
                     float((packed.x >> 20) & 1023u)) * (1.0 / 1023.0);
    vec3 p = cell_min + frac * cell_size;
    vec3 normal = unpack_point_normal(packed.y);

    vec3 v = p - push.camera_position.xyz;

    // Backfacing points describe surface the camera cannot see, so dropping
    // them is free coverage. The threshold is deliberately NOT zero: normals
    // are stored octahedrally in 16 bits and points sit on a surface the
    // field only approximates, so a point near a silhouette can read as
    // barely-backfacing when it is actually visible. Rejecting exactly at
    // zero thins those silhouettes into hairline gaps -- and a gap shows
    // whatever is behind the surface. Keeping the grazing band costs a few
    // percent more splats and removes that failure entirely.
    if (dot(normal, normalize(v)) > 0.25) {
        return;
    }

    // Clip-level gating, dithered inside the transition band -- see this
    // shader's own notes.
    // Frame-independent for the same reason as the roulette above -- a
    // per-frame dither makes the whole transition band between two clip
    // levels boil. Different salt so the two decisions stay decorrelated.
    float level_dither = splat_hash(uint(cluster_index), local_index, 7777u);
    if (level_of_cell_size(cell_size) !=
        finest_level_at(p, push.camera_position.xyz, level_dither)) {
        return;
    }

    float z = dot(v, push.camera_forward.xyz);
    if (z < 0.05) {
        return; // behind (or grazing) the camera plane -- nothing to splat
    }

    float inv_z = 1.0 / z;
    vec2 uv = vec2(dot(v, push.camera_right.xyz), dot(v, push.camera_up.xyz)) *
        inv_z;
    // Inverse of the render shader's uv construction -- pixel CENTERS sit at
    // integer coordinates there, hence the plain round below.
    vec2 pixel = uv * float(push.height) +
        0.5 * vec2(float(push.width), float(push.height));
    ivec2 center = ivec2(round(pixel));

    // Exact distance packed above the point's own id, with the SPACING it
    // was drawn at (as a power-of-two multiple of the cluster's voxel size)
    // in the id's top bits. Carrying that saves the render pass from
    // recomputing this whole selection to learn how big the winning splat
    // was -- which it needs both for the disc it intersects and for Prime
    // mode's back-off margin. The id itself needs 25 bits (MAX_CLUSTERS *
    // CHUNK_CLUSTER_POINTS), so the code sits at bits 25-27.
    uint id = uint(cluster_index) * uint(CHUNK_CLUSTER_POINTS) + local_index;
    uint spacing_code = uint(clamp(int(ceil(log2(max(spacing / voxel_size, 1.0)))),
                                   0, 7));
    uint64_t entry = (uint64_t(floatBitsToUint(length(v))) << 32) |
        uint64_t(id | (spacing_code << 25));

    // Footprint: cover this point's own share of the surface (half the
    // projected spacing, plus one pixel of slop) so neighbouring points'
    // footprints overlap into gap-free coverage.
    float footprint_px = spacing * inv_z * float(push.height);
    int radius = clamp(int(footprint_px * 0.5) + 1, 1, 8);

    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            ivec2 q = center + ivec2(dx, dy);
            if (q.x < 0 || q.y < 0 || q.x >= push.width || q.y >= push.height) {
                continue;
            }
            atomicMin(splat_visibility[q.y * push.width + q.x], entry);
        }
    }
}
