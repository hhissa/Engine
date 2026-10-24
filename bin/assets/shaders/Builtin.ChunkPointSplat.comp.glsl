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

// The toroidal chunk-lookup table and the per-slot published flags -- the
// same two the marched field and the cull pass read. Needed here so
// finest_level_at() can tell "this level owns this space" apart from "this
// level actually HAS this space right now". See its own comment.
layout(binding = 6) readonly buffer ChunkTableBuffer {
    int chunk_table[];
};
// FLECKS. The splats are the surface's "flecks" (Dreams' term). A brick's
// looseness (0 = tight, 1 = loosest; authored per stamp, stored per brick)
// trades them for fewer, bigger ones: a loose fleck has a WORLD size of
// FLECK_TIGHT_VOXELS..FLECK_LOOSE_VOXELS voxels (so its size depends on looseness,
// not on viewing distance), and the brick keeps just enough points --
// (spacing / fleck size)^2 of them -- to cover the same surface. Loose flecks are round discs
// rather than this pass's usual square footprint, are never declined to the
// marcher at close range (they ARE the look), and carry their looseness in
// the visibility payload so the render pass draws them as flecks instead of
// snapping them back onto the smooth isosurface.
layout(binding = 8) readonly buffer ChunkBrickLoosenessBuffer {
    float chunk_brick_looseness[];
};
const float FLECK_TIGHT_VOXELS = 0.3;  // fleck size at the lowest looseness, in voxels
                                       // (under 1: smaller than the point spacing, so
                                       // the solid surface shows between flecks)
const float FLECK_LOOSE_VOXELS = 20.0; // fleck size at looseness 1, in voxels
// How many flecks cover each fleck-sized patch of surface: 1 just tiles it
// (gaps show wherever the random pick clumps); more overlap them into a
// fully covered, layered surface.
const float FLECK_DENSITY = 3.0;

// Per-material-slot fleck settings (VulkanRaymarchShader::set_material_
// flecks()): x = looseness, y = style + 1 (0 = never set: Circle), z = comb,
// w = ruffle. Looked up through the brick's material slot.
layout(binding = 9) readonly buffer FleckParamsBuffer {
    vec4 fleck_params[];
};
layout(binding = 10) readonly buffer ChunkBrickPrimitiveBuffer {
    int chunk_brick_primitive[];
};
const int FLECK_PARAM_SLOTS = 1000; // kMaxScenePrimitives engine-side

// EFFECTS MODE: per-material-slot animations (VulkanRaymarchShader::set_
// material_effects()), two vec4s per slot: (boil, flow, wave, evaporate),
// (throb, -, -, -), each 0..1. Animated by push.camera_right.w (seconds).
layout(binding = 11) readonly buffer FleckEffectsBuffer {
    vec4 fleck_effects[];
};
const float EFFECT_BOIL_RATE = 0.8;      // bubbles per second, per fleck
const float EFFECT_FLOW_SPEED = 0.6;     // fleck spacings per second
const float EFFECT_WAVE_RATE = 2.2;      // sways per second (radians / 2pi-ish)
const float EFFECT_WAVE_ANGLE = 0.75;    // radians of sway at full wave
const float EFFECT_EVAPORATE_RATE = 0.3; // puffs per second, per fleck
const float EFFECT_THROB_RATE = 1.2;     // beats per second

// Fleck shape (see fleck_covers()), in units of the fleck's own square.
const float FLECK_WOBBLE = 0.05;    // gentle wobble of the outline, so no
                                    // two flecks are quite the same shape
// Each fleck is a raised dab of paint, not a flat card: its surface rises
// by up to this many fleck radii (stored in each pixel's depth, so the
// render pass lights the real relief -- see fleck_paint_height()).
const float FLECK_DOME = 0.35;
// The dab's relief, as fractions of that height:
const float FLECK_BRISTLE = 0.14;   // grooves the brush's bristles left,
const float FLECK_BRISTLES = 5.0;   //   this many across the stroke
const float FLECK_LIP = 0.15;       // paint pushed up into a rim
const float FLECK_GRAIN = 0.04;     // fine unevenness

// Smooth 2D value noise in [0, 1].
float fleck_hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float fleck_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(fleck_hash(i), fleck_hash(i + vec2(1.0, 0.0)), f.x),
               mix(fleck_hash(i + vec2(0.0, 1.0)), fleck_hash(i + vec2(1.0, 1.0)), f.x),
               f.y);
}
const int FLECK_STYLE_CIRCLE = 1;

float fleck_segment_dist(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0));
}

// Whether point q of a fleck's own square [-1,1]^2 (u along the fleck's
// direction, v across it) is inside fleck `style`'s shape -- the 16 Dreams
// styles, in order. tools/modelling_software_v2/icons/make_fleck_icons.py
// draws the UI icons from the same shapes, so keep the two in step.
bool fleck_mask(int style, vec2 q) {
    float r = length(q);
    switch (style) {
    case 0: // Square
        return max(abs(q.x), abs(q.y)) <= 0.8;
    case 1: // Circle
        return r <= 1.0;
    case 2: // Triangle
        return q.y >= -0.48 && abs(q.x) <= (0.95 - q.y) * 0.5734;
    case 3: { // Hexagon
        vec2 a = abs(q);
        return max(a.x * 0.866 + a.y * 0.5, a.y) <= 0.85;
    }
    case 4: // Dots
        return length(q) <= 0.3 || length(q - vec2(0.6, 0.55)) <= 0.27 ||
               length(q - vec2(-0.6, 0.5)) <= 0.27 ||
               length(q - vec2(0.55, -0.6)) <= 0.27 ||
               length(q - vec2(-0.55, -0.6)) <= 0.27;
    case 5: // Scribble
        return abs(q.x) <= 0.95 && abs(q.y - 0.4 * sin(q.x * 7.0)) <= 0.2;
    case 6: // Veins
        return fleck_segment_dist(q, vec2(0.0, -0.95), vec2(0.0, 0.95)) <= 0.1 ||
               fleck_segment_dist(q, vec2(0.0, 0.1), vec2(0.7, 0.7)) <= 0.08 ||
               fleck_segment_dist(q, vec2(0.0, -0.3), vec2(-0.65, 0.25)) <= 0.08;
    case 7: { // Splat
        float a = atan(q.y, q.x);
        return r <= 0.66 + 0.18 * sin(5.0 * a + 0.7) + 0.12 * sin(9.0 * a + 2.1);
    }
    case 8: // Straight
        return abs(q.y) <= 0.22 && abs(q.x) <= 0.95;
    case 9: // Heavy
        return fleck_segment_dist(q, vec2(-0.55, 0.0), vec2(0.55, 0.0)) <= 0.45;
    case 10: // Hatching
        return max(abs(q.x), abs(q.y)) <= 0.9 && fract((q.y + 1.0) * 2.0) < 0.35;
    case 11: { // Cubist: an irregular four-sided shard
        vec2 v0 = vec2(-0.9, -0.3), v1 = vec2(0.2, -0.95);
        vec2 v2 = vec2(0.85, 0.4), v3 = vec2(-0.3, 0.85);
        float c0 = (v1.x - v0.x) * (q.y - v0.y) - (v1.y - v0.y) * (q.x - v0.x);
        float c1 = (v2.x - v1.x) * (q.y - v1.y) - (v2.y - v1.y) * (q.x - v1.x);
        float c2 = (v3.x - v2.x) * (q.y - v2.y) - (v3.y - v2.y) * (q.x - v2.x);
        float c3 = (v0.x - v3.x) * (q.y - v3.y) - (v0.y - v3.y) * (q.x - v3.x);
        return c0 >= 0.0 && c1 >= 0.0 && c2 >= 0.0 && c3 >= 0.0;
    }
    case 12: // Impressionist: a short dab
        return (q.x * q.x) / 0.9025 + (q.y * q.y) / 0.2025 <= 1.0;
    case 13: // Expressive: a curved, tapering stroke
        return abs(q.x) <= 0.95 &&
               abs(q.y - (0.6 * q.x * q.x - 0.3)) <= 0.3 * (1.0 - abs(q.x)) + 0.03;
    case 14: { // Baroque: a spiral
        float a = atan(q.y, q.x) + 3.14159265;
        for (int k = 0; k < 2; ++k) {
            float target = 0.2 + (a + 6.2831853 * float(k)) / 6.2831853 * 0.38;
            if (target < 0.97 && abs(r - target) < 0.1) {
                return true;
            }
        }
        return false;
    }
    case 15: // Streaky: broken streaks
        return abs(q.x) <= 0.98 &&
               ((abs(q.y - 0.1) <= 0.13 && fract(q.x * 2.5 + 0.3) < 0.72) ||
                (abs(q.y + 0.45) <= 0.08 && fract(q.x * 1.7) < 0.6));
    default:
        return r <= 1.0;
    }
}

// A fleck as a quick hand-made mark rather than a clean stamp: fleck_mask()'s
// shape, but each fleck its own size and stretch, its outline bent by a slow
// wobble and roughened by finer noise, and filled not solid but with a few
// wobbly scrawled lines running along it (u), gaps between -- so each one
// reads as a pen/brush scrawl, and neighbouring flecks of one style differ.
// `seed` is the fleck's own (fixed) random number.
// Paint height of a dab at shape-space point q (inside the shape): a soft
// mound, scored along the stroke (fleck x) by bristle grooves, with a lip
// of paint pushed up at the rim and a little grain. 0 at the edge, ~1 top.
float fleck_paint_height(vec2 q, vec2 offset) {
    float r = sqrt(dot(q, q) * 0.85);
    float mound = sqrt(max(1.0 - r * r, 0.0));
    // Grooves run along the stroke, wandering slightly, deepest mid-dab.
    float wander = 0.6 * fleck_noise(vec2(q.x * 1.3, 0.0) + offset + 4.1);
    float groove = 0.5 + 0.5 * sin((q.y + wander * 0.25) * FLECK_BRISTLES * 3.14159 +
                                   offset.y * 6.2831853);
    float bristle_depth = fleck_noise(vec2(q.x * 0.9, q.y * FLECK_BRISTLES) + offset);
    float bristles = groove * (0.5 + 0.5 * bristle_depth);
    float lip = smoothstep(0.5, 0.82, r) * (1.0 - smoothstep(0.82, 1.0, r));
    float grain = fleck_noise(q * 6.0 + offset + 3.3);
    return mound * (1.0 - FLECK_BRISTLE + FLECK_BRISTLE * bristles) +
           FLECK_LIP * lip + FLECK_GRAIN * grain * mound;
}

// Whether fleck-space point q is inside this fleck; height = its paint
// height there (see fleck_paint_height()).
bool fleck_covers(int style, vec2 q, uint seed, out float height) {
    height = 0.0;
    vec2 offset = vec2(float(seed & 1023u), float((seed >> 10) & 1023u)) * 0.173;
    float size = 0.85 + 0.35 * float((seed >> 20) & 63u) / 63.0;
    float stretch = 0.85 + 0.3 * float((seed >> 26) & 31u) / 31.0;
    q /= vec2(size * stretch, size / stretch);
    q += FLECK_WOBBLE * 2.0 *
         (vec2(fleck_noise(q * 1.7 + offset), fleck_noise(q * 1.7 + offset + 17.3)) - 0.5);
    if (!fleck_mask(style, q)) {
        return false;
    }
    height = fleck_paint_height(q, offset);
    return true;
}
// IMPASTO: each fleck is a card hinged at its base edge (fleck x = -1, the
// comb's upstream end) and swung up off the surface by up to this angle,
// so its tip really stands out from the object -- projected, depth-tested
// and (past the silhouette) drawn where it actually is. Every fleck pixel
// stores its exact ray distance to its card, so the render pass places
// and lights it from that alone (Builtin.RaymarchShader.comp.glsl).
const float FLECK_IMPASTO_ANGLE = 1.05; // radians at impasto 1 (~60 degrees)
const int FLECK_MAX_RADIUS_PX = 64;   // loose flecks' footprint cap
const float FLECK_DISC_RADIUS = 0.8;  // disc radius, in spacings (>0.707
                                      // so neighbours overlap)

layout(binding = 7) readonly buffer ChunkSlotPublishedBuffer {
    uint chunk_slot_published[];
};

// Total chunk slots across every clip level -- mirrors CHUNK_MAX_SLOTS in
// Builtin.ChunkClusterCull.comp.glsl and kMaxResidentChunks * kNumLevels
// engine-side.
const int SPLAT_CHUNK_MAX_SLOTS = NUM_CHUNK_LEVELS * 512;

// GLSL has no integer mod that handles negatives the way a toroidal table
// needs -- same helper, same reason, as floor_mod3() in Builtin.
// ChunkedFieldCommon.inc.glsl.
ivec3 splat_floor_mod3(ivec3 v, int m) {
    return ((v % m) + m) % m;
}

// Is level `level` actually resident AND published at p?
bool splat_level_resident_at(vec3 p, int level) {
    float level_world_size = chunk_level_world_size(level);
    ivec3 chunk_coord = ivec3(floor(p / level_world_size));
    ivec3 wrapped = splat_floor_mod3(chunk_coord, CHUNK_TABLE_DIM);
    int table_index = level * CHUNK_TABLE_DIM * CHUNK_TABLE_DIM * CHUNK_TABLE_DIM +
        wrapped.x + wrapped.y * CHUNK_TABLE_DIM +
        wrapped.z * CHUNK_TABLE_DIM * CHUNK_TABLE_DIM;
    int slot = chunk_table[table_index];
    return slot >= 0 && slot < SPLAT_CHUNK_MAX_SLOTS &&
        chunk_slot_published[slot] != 0u;
}

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
    vec4 camera_position; // xyz; w = this frame's TAA jitter x (pixels)
    vec4 camera_forward;  // xyz; w = this frame's TAA jitter y (pixels)
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
    int only_level;       // Diagnostic: when >= 0, only this clip level may
                         // splat, so an artefact can be attributed to one
                         // level by isolating each in turn. -1 = normal.
                         // See KENGINE_SPLAT_ONLY_LEVEL engine-side.
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
    // Diagnostic isolation -- claim every point for the chosen level, so
    // only clusters actually baked at that level match and splat.
    if (push.only_level >= 0) {
        return push.only_level;
    }
    for (int level = 0; level < NUM_CHUNK_LEVELS; ++level) {
        float level_world_size = chunk_level_world_size(level);
        ivec3 chunk_coord = ivec3(floor(p / level_world_size));
        ivec3 camera_chunk = ivec3(floor(camera_pos / level_world_size));
        ivec3 delta = chunk_coord - camera_chunk;
        ivec3 abs_delta = abs(delta);
        int chebyshev = max(abs_delta.x, max(abs_delta.y, abs_delta.z));
        if (chebyshev > stream_radius_chunks(level)) {
            continue;
        }
        // Being in a level's WINDOW is not the same as that level having
        // this space right now, and gating on the window alone is what made
        // chunks vanish while the camera moved. The window test is pure
        // camera arithmetic, so a fine chunk counts as "in window" the
        // instant the camera moves -- but it still has to bake, which takes
        // frames. During that gap the cull pass drops that chunk's own
        // clusters (their slot is unpublished, see Builtin.ChunkCluster
        // Cull.comp.glsl), AND every coarser level's points declined to
        // splat here because this test said the fine level owned the space.
        // Nothing splatted at all, so the surface simply disappeared until
        // the bake landed. An in-place rebake after an edit unpublishes the
        // slot exactly the same way.
        //
        // Skipping a non-resident level hands the space back to the coarser
        // one that does have it -- the same rule sample_clipmap_field() now
        // follows for the marched field, and the reason both must agree is
        // that they describe the same surface to the same pixels.
        if (!splat_level_resident_at(p, level)) {
            continue;
        }
        if (SPLAT_LEVEL_DITHER_BAND > 0.0 &&
            chebyshev == stream_radius_chunks(level) &&
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
            // Only hand the point to the coarser level if that level can
            // actually take it -- otherwise this reintroduces exactly the
            // hole the residency check above exists to close.
            if (dither < t && splat_level_resident_at(p, level + 1)) {
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

// FLECK QUEUE. A loose fleck's footprint can be up to FLECK_MAX_RADIUS_PX
// across, and a close-up loose brick keeps only a few percent of its points
// as flecks -- so drawn one fleck per thread, a warp would sit with one lane
// looping over thousands of pixels and the other 31 idle. Big flecks are
// queued in shared memory instead and the whole workgroup sweeps each one's
// pixels together. Small ones (and any past the cap) are still drawn by
// their own thread, where the per-fleck queue overhead isn't worth it.
const int FLECK_QUEUE_CAP = 64;
const int FLECK_QUEUE_MIN_RADIUS = 3;
shared uint s_fleck_count;
shared ivec3 s_fleck_center_radius[FLECK_QUEUE_CAP];   // pixel centre, radius
shared vec3 s_fleck_offset_fade[FLECK_QUEUE_CAP];      // sample offset, fx_fade
shared vec4 s_fleck_to_fleck[FLECK_QUEUE_CAP];         // mat2, column-major
shared vec4 s_fleck_card_centre[FLECK_QUEUE_CAP];      // xyz, w = fleck radius
shared vec4 s_fleck_card_normal[FLECK_QUEUE_CAP];      // xyz, w = point distance
shared uvec3 s_fleck_style_seed_payload[FLECK_QUEUE_CAP];

// One pixel of a queued loose fleck -- the shaped branch of main()'s own
// footprint loop, which must stay in step with this.
void splat_fleck_pixel(ivec2 q, vec2 fq, int style, uint seed, float fx_fade,
                       uint payload, vec3 card_centre, float fleck_radius,
                       vec3 card_normal, float point_dist) {
    float dome = 0.0;
    if (!fleck_covers(style, fq, seed, dome)) {
        return;
    }
    if (q.x < 0 || q.y < 0 || q.x >= push.width || q.y >= push.height) {
        return;
    }
    if (fx_fade > 0.0 &&
        splat_hash(uint(q.x) * 7919u + uint(q.y), uint(push.frame_index),
                   0x50F7u) < fx_fade * 0.7) {
        return;
    }
    vec2 jitter = vec2(push.camera_position.w, push.camera_forward.w);
    vec2 ray_uv = (vec2(q) + jitter -
                   0.5 * vec2(float(push.width), float(push.height))) /
                  float(push.height);
    vec3 ray = normalize(ray_uv.x * push.camera_right.xyz +
                         ray_uv.y * push.camera_up.xyz +
                         push.camera_forward.xyz);
    float denom = dot(ray, card_normal);
    float dist = abs(denom) > 1e-3
        ? dot(card_centre - push.camera_position.xyz, card_normal) / denom
        : point_dist;
    dist -= FLECK_DOME * fleck_radius * dome / max(abs(denom), 0.3);
    uint64_t pixel_entry =
        (uint64_t(floatBitsToUint(max(dist, 1e-3))) << 32) | uint64_t(payload);
    atomicMin(splat_visibility[q.y * push.width + q.x], pixel_entry);
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
    // FLECKS: this brick's settings -- the larger of its own (per-stamp)
    // looseness and its material slot's, plus the slot's style/comb/ruffle.
    int brick = int(cluster.meta.z);
    int fleck_slot = chunk_brick_primitive[brick];
    vec4 fp = fleck_slot >= 0 && fleck_slot < FLECK_PARAM_SLOTS
        ? fleck_params[fleck_slot] : vec4(0.0);
    float looseness = max(clamp(chunk_brick_looseness[brick], 0.0, 1.0),
                          clamp(fp.x, 0.0, 1.0));
    uint loose_code = uint(round(looseness * 15.0));
    // y = style + 1 + impasto / 2 (see set_material_flecks()).
    int fleck_style = fp.y > 0.5 ? int(floor(fp.y + 1e-4)) - 1 : FLECK_STYLE_CIRCLE;
    float fleck_impasto = fp.y > 0.5 ? clamp(fract(fp.y + 1e-4) * 2.0, 0.0, 1.0) : 0.0;
    float fleck_comb = clamp(fp.z, 0.0, 1.0);
    // w = ruffle level (0-3) + neatness / 2 (see set_material_flecks()).
    float fleck_ruffle = clamp(floor(fp.w + 1e-4), 0.0, 3.0) / 3.0;
    float fleck_neatness = clamp(fract(fp.w + 1e-4) * 2.0, 0.0, 1.0);
    // EFFECTS: this slot's animations. An effect needs flecks to move, so an
    // effected object is drawn as (small) flecks even at looseness 0.
    vec4 fx = fleck_slot >= 0 && fleck_slot < FLECK_PARAM_SLOTS
        ? fleck_effects[fleck_slot * 2] : vec4(0.0);
    float fx_throb = fleck_slot >= 0 && fleck_slot < FLECK_PARAM_SLOTS
        ? fleck_effects[fleck_slot * 2 + 1].x : 0.0;
    float fx_boil = fx.x, fx_flow = fx.y, fx_wave = fx.z, fx_evaporate = fx.w;
    bool has_effects = max(max(fx.x, fx.y), max(max(fx.z, fx.w), fx_throb)) > 0.0;
    if (has_effects) {
        loose_code = max(loose_code, 2u);
        looseness = max(looseness, 2.0 / 15.0);
    }
    float fx_time = push.camera_right.w;
    if (push.splat_mode == SPLAT_MODE_VISIBILITY && finest_px > SPLAT_MAX_PX &&
        loose_code == 0u) {
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

    if (gl_LocalInvocationID.x == 0u) {
        s_fleck_count = 0u;
    }
    barrier();

    // Each thread handles one point; a point that ends up drawing nothing
    // breaks out. Kept as a block rather than early returns because the
    // whole workgroup must reach the barrier below (see FLECK QUEUE).
    do {
        // Past here every thread handles exactly one point of this cluster.
        uint local_index = gl_LocalInvocationID.x;
        if (local_index >= cluster_points_used) {
            break; // partially filled page (the brick's last one)
        }
        // Which point of the owning BRICK this is -- the LOD prefix is expressed
        // in those terms, since a level's point set can span several clusters.
        uint brick_point_index = cluster.meta.x + local_index;
        if (brick_point_index >= cluster.lod_counts[level]) {
            break;
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
                break;
            }
            // Survivors widen to cover the area the thinned-out points left, but
            // NOT without limit: keep goes to zero with distance, so an uncapped
            // 1/sqrt(keep) grows a lone survivor into a blob far larger than the
            // feature it belongs to. At a silhouette those oversized splats hang
            // past the true edge and read as a row of beads or teeth along it --
            // visible along any straight edge seen against a distant background.
            spacing /= sqrt(max(keep, SPLAT_MIN_KEEP_FOR_WIDENING));
        }

        // FLECKS: a loose brick keeps 1 in grow^2 of its points and widens the
        // survivors by grow -- fewer, bigger flecks over the same surface. Its
        // own salt, so it thins independently of the screen-density roulette.
        // A fleck smaller than the spacing keeps every point and is just drawn
        // smaller (fleck_draw), leaving gaps the marched surface fills.
        float fleck_draw = spacing;
        if (loose_code > 0u) {
            float fleck_size = voxel_size *
                mix(FLECK_TIGHT_VOXELS, FLECK_LOOSE_VOXELS, float(loose_code - 1u) / 14.0);
            if (fleck_size > spacing) {
                float keep_loose = spacing / fleck_size;
                if (splat_hash(uint(cluster_index), local_index, 0x51ED27u) >
                    FLECK_DENSITY * keep_loose * keep_loose) {
                    break;
                }
                spacing = fleck_size;
            }
            fleck_draw = fleck_size;
        }

        uvec2 packed = cluster_points[uint(cluster_index) *
                                      uint(CHUNK_CLUSTER_POINTS) + local_index];
        vec3 frac = vec3(float(packed.x & 1023u), float((packed.x >> 10) & 1023u),
                         float((packed.x >> 20) & 1023u)) * (1.0 / 1023.0);
        vec3 p = cell_min + frac * cell_size;
        vec3 normal = unpack_point_normal(packed.y);

        // NEATNESS: pull each loose fleck onto a lattice in the surface -- rows
        // one fleck apart running across the comb direction, every other row
        // shifted half a fleck along (brickwork) -- so the flecks lie in neat,
        // staggered rows instead of scattered at random.
        if (loose_code > 0u && fleck_neatness > 0.0) {
            vec3 row_dir = vec3(0.0, 1.0, 0.0); // the comb direction (see below)
            vec3 down = row_dir - normal * dot(row_dir, normal);
            down = dot(down, down) < 1e-6 ? cross(normal, vec3(1.0, 0.0, 0.0)) : down;
            down = normalize(down);
            vec3 across = cross(normal, down);
            float row_v = dot(p, down) / spacing;
            float row = floor(row_v);
            float col_u = dot(p, across) / spacing + 0.5 * mod(row, 2.0);
            vec3 snapped = p + down * ((row + 0.5 - row_v) * spacing) +
                           across * ((round(col_u) - col_u) * spacing);
            p = mix(p, snapped, fleck_neatness);
        }

        // EFFECTS: move, swell, sway and fade this fleck over time. Each fleck
        // runs its own cycle (hashed phase) except throb, which beats together.
        float fx_size = 1.0;    // scale on the fleck's radius
        float fx_tilt = 0.0;    // extra swing about its base edge (radians)
        float fx_turn = 0.0;    // extra turn in the surface (radians)
        float fx_fade = 0.0;    // fraction of its pixels dropped (TAA softens it)
        if (has_effects) {
            uint fid = uint(cluster_index) * 977u + local_index;
            float phase_a = splat_hash(fid, 0u, 0xB0117u);
            float phase_b = splat_hash(fid, 1u, 0xB0117u);
            vec3 world_down = vec3(0.0, 1.0, 0.0); // scene +Y is down
            vec3 downhill = world_down - normal * dot(world_down, normal);
            downhill = dot(downhill, downhill) < 1e-6 ? cross(normal, vec3(1.0, 0.0, 0.0))
                                                     : normalize(downhill);
            // Boil: each fleck swells up off the surface like a bubble rising,
            // then bursts (vanishes) and starts again.
            if (fx_boil > 0.0) {
                float t = fract(fx_time * EFFECT_BOIL_RATE * (0.7 + 0.6 * phase_b) + phase_a);
                float swell = sin(3.14159 * t);
                p += normal * (fx_boil * swell * swell * spacing * 0.7);
                fx_size *= mix(1.0, 0.55 + 0.75 * swell, fx_boil);
                fx_fade = max(fx_fade, fx_boil * smoothstep(0.85, 1.0, t));
            }
            // Flow: flecks slide downhill across the surface, lined up with the
            // current, shrinking away at each end of their run so the loop
            // back to the start never shows.
            if (fx_flow > 0.0) {
                float t = fract(fx_time * EFFECT_FLOW_SPEED + phase_a);
                p += downhill * ((t - 0.5) * 2.0 * spacing * fx_flow);
                fx_size *= mix(1.0, sin(3.14159 * t), fx_flow * 0.8);
                fleck_comb = max(fleck_comb, fx_flow);
            }
            // Wave: flecks sway about their base like grass, in a gust that
            // travels across the object (phase by position), with a flutter.
            if (fx_wave > 0.0) {
                float gust = fx_time * EFFECT_WAVE_RATE * 3.14159 +
                             dot(p, vec3(1.0, 0.35, 0.6)) * 9.0;
                fx_tilt += fx_wave * EFFECT_WAVE_ANGLE * (0.55 + 0.45 * sin(gust));
                fx_turn += fx_wave * 0.35 * sin(gust * 1.7 + phase_a * 6.2831853);
            }
            // Evaporate: flecks drift up and away like steam -- lifting off,
            // spreading, softening and thinning out until they're gone.
            if (fx_evaporate > 0.0) {
                float t = fract(fx_time * EFFECT_EVAPORATE_RATE * (0.75 + 0.5 * phase_b) +
                                phase_a);
                vec3 rise = normalize(normal + vec3(0.0, -1.2, 0.0));
                p += rise * (fx_evaporate * t * t * spacing * 4.0);
                fx_size *= 1.0 + fx_evaporate * t * 0.9;
                fx_fade = max(fx_fade, fx_evaporate * smoothstep(0.1, 1.0, t));
            }
            // Throb: the whole object beats -- flecks swell and push out on a
            // sharp heartbeat pulse, then relax.
            if (fx_throb > 0.0) {
                float beat = fract(fx_time * EFFECT_THROB_RATE);
                float pulse = exp(-beat * 9.0) + 0.6 * exp(-max(beat - 0.22, 0.0) * 12.0) *
                                                     step(0.22, beat);
                p += normal * (fx_throb * pulse * spacing * 0.5);
                fx_size *= 1.0 + fx_throb * pulse * 0.45;
            }
            // Whole flecks vanish at random as they fade; the rest lose pixels.
            if (splat_hash(fid, uint(push.frame_index), 0xFADEu) < fx_fade * fx_fade) {
                break;
            }
        }

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
            break;
        }

        // Clip-level gating, dithered inside the transition band -- see this
        // shader's own notes.
        // Frame-independent for the same reason as the roulette above -- a
        // per-frame dither makes the whole transition band between two clip
        // levels boil. Different salt so the two decisions stay decorrelated.
        float level_dither = splat_hash(uint(cluster_index), local_index, 7777u);
        if (level_of_cell_size(cell_size) !=
            finest_level_at(p, push.camera_position.xyz, level_dither)) {
            break;
        }

        float z = dot(v, push.camera_forward.xyz);
        if (z < 0.05) {
            break; // behind (or grazing) the camera plane -- nothing to splat
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
        // Bits 28-29: looseness (FLECKS; 0 = an ordinary splat, 1-3 = loose),
        // bits 30-31: ruffle (0-3) -- what the render pass needs to draw it.
        uint loose_bits = loose_code == 0u ? 0u : uint(clamp(round(looseness * 3.0), 1.0, 3.0));
        uint ruffle_bits = loose_bits == 0u ? 0u : uint(round(fleck_ruffle * 3.0));
        float point_dist = length(v);
        uint payload = id | (spacing_code << 25) | (loose_bits << 28) | (ruffle_bits << 30);
        uint64_t entry = (uint64_t(floatBitsToUint(point_dist)) << 32) | uint64_t(payload);

        // Footprint: cover this point's own share of the surface (half the
        // projected spacing, plus one pixel of slop) so neighbouring points'
        // footprints overlap into gap-free coverage.
        float footprint_px = spacing * inv_z * float(push.height);
        int radius = clamp(int(footprint_px * 0.5) + 1, 1, 8);
        // A loose fleck is drawn as its style's shape, lying on the surface: a
        // frame (t, b) in the surface plane, turned at random per fleck or --
        // with comb -- toward one shared direction (down the surface, like
        // combed hair), projected to the screen so it foreshortens properly.
        // Each pixel is mapped back into the fleck's own square and kept only
        // if fleck_mask() says it's inside the shape.
        bool shaped = loose_code > 0u;
        mat2 to_fleck = mat2(1.0);
        float fleck_radius = 0.0; // world radius, for the dome
        vec3 card_centre = p;     // the fleck card's centre and plane normal,
        vec3 card_normal = normal; // for each pixel's exact distance
        if (shaped) {
            vec3 comb_dir = vec3(0.0, 1.0, 0.0); // world +Y: down on screen
            vec3 tc = comb_dir - normal * dot(comb_dir, normal);
            if (dot(tc, tc) < 1e-6) {
                tc = cross(normal, vec3(1.0, 0.0, 0.0));
            }
            tc = normalize(tc);
            vec3 bc = cross(normal, tc);
            float turn = splat_hash(uint(cluster_index), local_index, 0xC0FFEEu) *
                         6.2831853;
            vec3 t = mix(tc * cos(turn) + bc * sin(turn), tc, fleck_comb);
            t = dot(t, t) < 1e-6 ? tc : normalize(t);
            if (fx_turn != 0.0) { // wave's flutter
                t = normalize(t * cos(fx_turn) + cross(normal, t) * sin(fx_turn));
            }
            vec3 b = cross(normal, t);
            float fleck_r = fleck_draw * FLECK_DISC_RADIUS;
            fleck_r *= fx_size;
            fleck_radius = fleck_r;
            float swing = fleck_impasto * FLECK_IMPASTO_ANGLE + fx_tilt;
            if (swing > 0.0) {
                // Swing the card about its base edge (centre - r*t, along b):
                // t tilts up toward the normal, the centre rises with it.
                float angle = min(swing, 1.45);
                vec3 t_raised = t * cos(angle) + normal * sin(angle);
                card_centre = p + (t_raised - t) * fleck_r;
                card_normal = normalize(cross(t_raised, b));
                t = t_raised;
                vec3 vc = card_centre - push.camera_position.xyz;
                float zc = dot(vc, push.camera_forward.xyz);
                if (zc < 0.05) {
                    break;
                }
                inv_z = 1.0 / zc;
                uv = vec2(dot(vc, push.camera_right.xyz), dot(vc, push.camera_up.xyz)) * inv_z;
                pixel = uv * float(push.height) +
                    0.5 * vec2(float(push.width), float(push.height));
                center = ivec2(round(pixel));
            }
            // Screen-space (pixel) image of a small world offset w at the card.
            vec2 st = (vec2(dot(t, push.camera_right.xyz), dot(t, push.camera_up.xyz)) -
                       uv * dot(t, push.camera_forward.xyz)) * inv_z *
                      float(push.height) * fleck_r;
            vec2 sb = (vec2(dot(b, push.camera_right.xyz), dot(b, push.camera_up.xyz)) -
                       uv * dot(b, push.camera_forward.xyz)) * inv_z *
                      float(push.height) * fleck_r;
            vec2 extent = abs(st) + abs(sb);
            float reach = max(extent.x, extent.y);
            if (reach > float(FLECK_MAX_RADIUS_PX)) {
                // Shrink rather than clip, so a close-up fleck keeps its shape.
                float shrink = float(FLECK_MAX_RADIUS_PX) / reach;
                st *= shrink;
                sb *= shrink;
                reach = float(FLECK_MAX_RADIUS_PX);
            }
            float det = st.x * sb.y - st.y * sb.x;
            if (abs(det) < 0.25) {
                shaped = false; // seen edge-on: under a pixel, draw it as one
                radius = 0;
                card_centre = p;
                card_normal = normal;
            } else {
                radius = max(int(ceil(reach)), 1);
                // Inverse of [st sb]: pixel offset -> fleck coordinates.
                to_fleck = mat2(sb.y, -st.y, -sb.x, st.x) / det;
            }
        }

        // Each covered pixel's true sample point (its centre plus this frame's
        // TAA jitter), relative to the fleck's exact projected centre.
        vec2 jitter = vec2(push.camera_position.w, push.camera_forward.w);
        vec2 sample_offset = vec2(center) + jitter - pixel;
        uint fleck_seed = id * 2654435761u + 0x9E3779B9u;
        fleck_seed ^= fleck_seed >> 15;
        fleck_seed *= 2246822519u;

        // A big loose fleck goes to the workgroup's queue instead of being
        // drawn here by this one thread -- see FLECK QUEUE.
        if (shaped && radius >= FLECK_QUEUE_MIN_RADIUS) {
            uint job = atomicAdd(s_fleck_count, 1u);
            if (job < uint(FLECK_QUEUE_CAP)) {
                s_fleck_center_radius[job] = ivec3(center, radius);
                s_fleck_offset_fade[job] = vec3(sample_offset, fx_fade);
                s_fleck_to_fleck[job] = vec4(to_fleck[0], to_fleck[1]);
                s_fleck_card_centre[job] = vec4(card_centre, fleck_radius);
                s_fleck_card_normal[job] = vec4(card_normal, point_dist);
                s_fleck_style_seed_payload[job] = uvec3(uint(fleck_style), fleck_seed, payload);
                break;
            }
        }

        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                vec2 fq = to_fleck * (vec2(dx, dy) + sample_offset);
                float dome = 0.0;
                if (shaped && !fleck_covers(fleck_style, fq, fleck_seed, dome)) {
                    continue;
                }
                ivec2 q = center + ivec2(dx, dy);
                if (q.x < 0 || q.y < 0 || q.x >= push.width || q.y >= push.height) {
                    continue;
                }
                // A fading fleck (boil's burst, evaporate) drops a changing share
                // of its pixels each frame: TAA blends that into a soft, see-
                // through dab rather than a hard one.
                if (fx_fade > 0.0 &&
                    splat_hash(uint(q.x) * 7919u + uint(q.y), uint(push.frame_index),
                               0x50F7u) < fx_fade * 0.7) {
                    continue;
                }
                uint64_t pixel_entry = entry;
                if (shaped) {
                    // This pixel's ray (same construction as the render pass)
                    // against the fleck's card: its exact distance.
                    vec2 ray_uv = (vec2(q) + jitter -
                                   0.5 * vec2(float(push.width), float(push.height))) /
                                  float(push.height);
                    vec3 ray = normalize(ray_uv.x * push.camera_right.xyz +
                                         ray_uv.y * push.camera_up.xyz +
                                         push.camera_forward.xyz);
                    float denom = dot(ray, card_normal);
                    float dist = abs(denom) > 1e-3
                        ? dot(card_centre - push.camera_position.xyz, card_normal) / denom
                        : point_dist;
                    // Raised by the paint along the card's normal: that much
                    // nearer along this ray.
                    dist -= FLECK_DOME * fleck_radius * dome /
                            max(abs(denom), 0.3);
                    pixel_entry = (uint64_t(floatBitsToUint(max(dist, 1e-3))) << 32) |
                                  uint64_t(payload);
                }
                atomicMin(splat_visibility[q.y * push.width + q.x], pixel_entry);
            }
        }
    } while (false);

    // FLECK QUEUE: the whole workgroup draws the queued flecks together,
    // one fleck at a time, each thread taking every 256th pixel of it.
    barrier();
    uint fleck_jobs = min(s_fleck_count, uint(FLECK_QUEUE_CAP));
    for (uint job = 0u; job < fleck_jobs; ++job) {
        ivec3 cr = s_fleck_center_radius[job];
        int side = 2 * cr.z + 1;
        vec3 offset_fade = s_fleck_offset_fade[job];
        vec4 tf = s_fleck_to_fleck[job];
        mat2 to_fleck_q = mat2(tf.xy, tf.zw);
        vec4 cc = s_fleck_card_centre[job];
        vec4 cn = s_fleck_card_normal[job];
        uvec3 ssp = s_fleck_style_seed_payload[job];
        for (int t = int(gl_LocalInvocationID.x); t < side * side;
             t += CHUNK_CLUSTER_POINTS) {
            ivec2 d = ivec2(t % side, t / side) - ivec2(cr.z);
            splat_fleck_pixel(cr.xy + d,
                              to_fleck_q * (vec2(d) + offset_fade.xy),
                              int(ssp.x), ssp.y, offset_fade.z, ssp.z,
                              cc.xyz, cc.w, cn.xyz, cn.w);
        }
    }
}
