#include "vulkan_raymarch_shader.h"
#include "../../../core/logger.h"
#include "../../../resources/expression.h"
#include "../../../resources/sdf_scene.h"
#include "../../../systems/geometry_system.h"
#include "../../../systems/texture_system.h"
#include "../vulkan_commandbuffer.h"
#include "../vulkan_image.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <glm/gtc/quaternion.hpp>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
constexpr std::string_view BUILTIN_SHADER_NAME_VOXELIZE =
    "Builtin.RaymarchVoxelize";
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_VOXELIZE =
    "Builtin.ChunkVoxelize";
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_DEBUG_QUERY =
    "Builtin.ChunkedFieldDebugQuery";
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_EVICT =
    "Builtin.ChunkEvict";
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_PROBE_BAKE =
    "Builtin.ChunkProbeBake";
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_POINT_SPLAT =
    "Builtin.ChunkPointSplat";
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_CLUSTER_CULL =
    "Builtin.ChunkClusterCull";
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_SHADOW_SPLAT =
    "Builtin.ChunkShadowSplat";
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_VOXEL_CASCADE =
    "Builtin.ChunkVoxelCascade";
constexpr std::string_view BUILTIN_SHADER_NAME_STOCHASTIC_AO =
    "Builtin.StochasticAo";
constexpr std::string_view BUILTIN_SHADER_NAME_PROBE_BAKE =
    "Builtin.ProbeBake";
constexpr std::string_view BUILTIN_SHADER_NAME_RAYMARCH =
    "Builtin.RaymarchShader";
constexpr std::string_view BUILTIN_SHADER_NAME_DEFERRED_SHADE =
    "Builtin.DeferredShade";
constexpr std::string_view BUILTIN_SHADER_NAME_BLOOM_BLUR_H =
    "Builtin.BloomBlurH";
constexpr std::string_view BUILTIN_SHADER_NAME_POST_COMPOSITE =
    "Builtin.PostComposite";
constexpr std::string_view BUILTIN_SHADER_NAME_TAA_RESOLVE =
    "Builtin.TaaResolve";

// Matches the `push_constant` block in Builtin.RaymarchShader.comp.glsl.
// std430 (push constants use the same rules as std430, not std140) still
// aligns vec3 to 16 bytes, so both fields are declared as vec4 to make the
// padding explicit instead of implicit. Recorded directly into the command
// buffer each frame (vkCmdPushConstants) rather than a UBO: there's no
// separate buffer memory a still-in-flight previous frame could be reading
// while this frame's value is written, so no per-swapchain-image buffering
// is needed the way a UBO would require.
struct PushConstants {
  f32 camera_position[4]; // xyz + unused pad
  f32 camera_forward[4];  // xyz + unused pad
  f32 camera_right[4];    // xyz + unused pad
  f32 camera_up[4];       // xyz + unused pad
  i32 light_count;   // How many of light_buffer_'s entries to sum -- see
                     // rebuild_static_scene().
  f32 ambient;        // Scene-wide ambient factor -- see
                     // GeometrySystem::ambient().
  i32 selected_primitive_index; // scene_textures/scene_diffuse_colours index
                               // to outline, or -1 for none -- see
                               // VulkanRaymarchShader::set_selected_primitive().
  i32 flags; // kRenderFlag* bits below. Four independent booleans packed
            // into one scalar: this block sits close to Vulkan's guaranteed
            // 128-byte push-constant limit, and four whole ints for four
            // bits is the first thing that should give.
  i32 layer_count; // How many layer_buffer_ entries the render pass's
                  // per-pixel material re-evaluation folds (see scene_map()
                  // in Builtin.SdfSceneCommon.inc.glsl) -- same value the
                  // voxelize pass baked with, set by rebuild_static_scene().
  i32 volumetric_start; // First index (into primitives[]/scene_textures/
                       // scene_diffuse_colours) of the tail range
                       // rebuild_static_scene() appended GeometrySystem's
                       // registered volumetrics at -- see
                       // accumulate_volumetrics() in Builtin.RaymarchShader.
                       // comp.glsl. Meaningless if volumetric_count is 0.
  i32 volumetric_count; // How many contiguous entries starting there are
                       // volumetrics, not opaque primitives.
  f32 time; // Seconds since this shader was constructed -- drives
           // accumulate_volumetrics()'s scrolling texture animation (a
           // static shaft would look like a fixed decal rather than a
           // drifting light shaft). Accumulated in render_to(), never reset.
  f32 gi_cascade_center_x;
  f32 gi_cascade_center_y;
  f32 gi_cascade_center_z; // Phase 5: gi_cascade_center_'s current value --
                          // see sample_gi_cascade() in Builtin.
                          // RaymarchShader.comp.glsl.
  i32 splat_mode; // SplatMode, as its underlying i32 -- 0 unless render_to()
                 // actually recorded the splat prepass this frame (see
                 // splat_visibility_buffer_'s header comment), so the shader
                 // never reads stale splat data.
  i32 frame_index; // Seeds the shading pass's own dithering (the imperfect
                  // shadow maps' rotated taps), so it differs every frame
                  // and TAA can average it.
  f32 taa_jitter_x; // This frame's sub-pixel sample offset (see
  f32 taa_jitter_y; // kTaaJitterCount below). Zero while TAA is off. Last
                   // in the block, and two scalars rather than a vec2, to
                   // keep it at exactly Vulkan's guaranteed 128 bytes --
                   // see Builtin.RaymarchShader.comp.glsl's matching note.
};
// PushConstants::flags bits -- must match RENDER_FLAG_* in Builtin.
// RaymarchShader.comp.glsl and Builtin.DeferredShade.comp.glsl.
constexpr i32 kRenderFlagGrid = 1;
constexpr i32 kRenderFlagSkybox = 2;
constexpr i32 kRenderFlagChunkedField = 4;
constexpr i32 kRenderFlagIsm = 8;

// 4 vec4s + 15 scalars = 124 bytes -- within Vulkan's guaranteed minimum
// push constant size of 128 bytes, so no device limit query is
// needed here.

// Fallback when GeometrySystem has no registered lights at all (e.g. an
// .sdf file with no "light" blocks, or nothing loaded yet) -- without this,
// a scene authored before lights existed (or one that just doesn't bother
// with them) would render pitch black instead of with the same single
// hardcoded directional light this engine always used to have. Matches
// that old hardcoded look exactly (including the diffuse term's old
// implicit *0.85 scale, now folded into intensity).
const glm::vec3 kDefaultLightDirection(0.6f, 0.7f, -0.6f);
const glm::vec3 kDefaultLightColour(1.0f, 1.0f, 1.0f);
constexpr f32 kDefaultLightIntensity = 0.85f;

// Matches Builtin.RaymarchVoxelize.comp.glsl's push constant block.
struct VoxelizePushConstants {
  i32 layer_count;
  f32 max_smoothness; // See voxelize()'s own comment.
};

// Matches Builtin.ProbeBake.comp.glsl's push constant block.
struct ProbeBakePushConstants {
  i32 light_count;
  f32 ambient;
};

// Matches Builtin.ChunkVoxelize.comp.glsl's push constant block. (See
// vulkan_raymarch_shader.h for GpuChunkBatchEntry, used below and by
// voxelize_chunk_batch() -- defined there, not here, since the header's
// own member function declarations need it too.)
struct ChunkVoxelizePushConstants {
  i32 layer_count;
  f32 max_smoothness;
  i32 batch_count;
  i32 batch_offset; // Where this ring slot's region starts -- see the batch
                   // buffers' creation comment.
  i32 pass;         // 0 = bake representatives, 1 = copy aliases. See
                   // ChunkCellAliasBuffer in the shader.
  i32 collect_stats; // See ChunkBrickDemandBuffer in the shader.
};

// How many u32s chunk_brick_demand_buffer_ holds: [0] is the brick demand
// the overflow check reads, [1..5] are the bake-cost counters (see the
// shader's own comment). Kept as one buffer rather than two because the
// counters share its "written by every invocation, read once by the CPU"
// access pattern exactly.
constexpr u32 kChunkBakeStatCount = 6;

// Matches Builtin.ChunkedFieldDebugQuery.comp.glsl's push constant block.
struct ChunkDebugQueryPushConstants {
  f32 query_x;
  f32 query_y;
  f32 query_z;
  i32 use_clipmap;
  f32 camera_x;
  f32 camera_y;
  f32 camera_z;
};

// Matches Builtin.ChunkedFieldDebugQuery.comp.glsl's DebugOutputBuffer.
struct ChunkDebugQueryOutput {
  f32 dist;
  f32 skip_dist;
  i32 material_index;
};

// Matches Builtin.ChunkEvict.comp.glsl's push constant block.
struct ChunkEvictPushConstants {
  i32 batch_count;
  i32 batch_offset; // Where this ring slot's region starts -- see the batch
                   // buffers' creation comment.
};

// Matches Builtin.ChunkProbeBake.comp.glsl's push constant block.
struct ChunkProbeBakePushConstants {
  i32 light_count;
  f32 ambient;
  f32 cascade_center_x;
  f32 cascade_center_y;
  f32 cascade_center_z;
  f32 camera_x;
  f32 camera_y;
  f32 camera_z;
};

// Matches Builtin.ChunkPointSplat.comp.glsl's push constant block -- the
// same camera basis PushConstants carries (vec4-padded for the same std430
// reason), plus the render-target size the splat's projected pixel
// coordinates are relative to.
struct ChunkPointSplatPushConstants {
  f32 camera_position[4];
  f32 camera_forward[4];
  f32 camera_right[4];
  f32 camera_up[4];
  i32 width;
  i32 height;
  i32 splat_mode; // SplatMode as its underlying i32 -- the splat pass needs
                 // it to decide whether to apply the too-sparse-to-splat
                 // rule (Visibility only); see SPLAT_MAX_PX in the shader.
  i32 frame_index; // Seed for the pass's stochastic decisions (Russian
                  // roulette thinning, clip-level dithering). Only has to
                  // differ between frames -- TAA is what turns that noise
                  // back into a smooth image.
};

// Matches Builtin.ChunkVoxelCascade.comp.glsl's push constant block: per
// cascade, xyz = min corner in render space and w = voxel size.
struct ChunkVoxelCascadePushConstants {
  f32 cascade_origin[4][4];
};

// Matches Builtin.StochasticAo.comp.glsl's push constant block.
struct StochasticAoPushConstants {
  f32 camera_position[4];
  f32 camera_forward[4];
  f32 camera_right[4];
  f32 camera_up[4];
  f32 cascade_origin[4][4];
  f32 jitter_x;
  f32 jitter_y;
  i32 frame_index;
  i32 pad;
};

// Matches Builtin.ChunkShadowSplat.comp.glsl's push constant block.
struct ChunkShadowSplatPushConstants {
  i32 frame_index;
  i32 pad0;
  i32 pad1;
  i32 pad2;
};

// Matches Builtin.ChunkClusterCull.comp.glsl's push constant block.
struct ChunkClusterCullPushConstants {
  f32 camera_position[4];
  f32 camera_forward[4];
  f32 camera_right[4];
  f32 camera_up[4];
  f32 half_extent_x; // View half-extents at unit forward distance, in the
  f32 half_extent_y; // same uv units the render pass builds rays in.
  i32 cluster_count;
  i32 light_count;    // How many lights to pair clusters against.
  i32 ism_count;      // How many of them can have a shadow map (kIsmCount).
  i32 shadow_enabled; // Zero skips shadow pairing entirely.
  i32 pad0;
  i32 pad1;
};

// Matches Builtin.BloomBlurH.comp.glsl's push constant block.
struct BloomBlurHPushConstants {
  i32 full_width;
  i32 full_height;
  f32 bloom_threshold;
};

// Matches Builtin.PostComposite.comp.glsl's push constant block.
struct PostCompositePushConstants {
  i32 full_width;
  i32 full_height;
  f32 bloom_intensity;
  f32 vignette_strength;
  f32 vignette_radius;
  i32 pixelation_enabled;
  i32 pixelation_block_size;
};

// GI probe grid dimensions. Must match PROBE_DIM in both
// Builtin.ProbeBake.comp.glsl and Builtin.RaymarchShader.comp.glsl exactly.
// Probes sit at the corners of a (kProbeDim-1)^3 cell grid spanning the
// full [-BOUNDS, BOUNDS] cube inclusive (2-unit spacing at BOUNDS=16) --
// coarse relative to the voxel field's own 0.25-unit cells, deliberately:
// GI only needs to vary smoothly across room-scale distances, not resolve
// fine surface detail, and probe count is the dominant cost of
// bake_probes() (kProbeDim^3 probes x their own gather-sample count x
// kProbeBounceCount bounces).
constexpr u32 kProbeDim = 16;
constexpr u32 kProbeCount = kProbeDim * kProbeDim * kProbeDim;
// How many light bounces bake_probes() simulates -- Inigo Quilez's
// "simplegi" article (the technique this is adapted from) mentions using
// up to 3; raised past that here for noticeably more colour bleeding into
// corners/indirectly-lit areas before the bounces converge. Each
// additional bounce is a full extra kProbeCount x PROBE_GATHER_SAMPLES
// gather-ray pass, so this is the other major bake-cost lever alongside
// kProbeDim -- still a one-time cost per rebake(), not per frame, so
// raising it is cheap relative to render_to()'s own per-frame budget.
constexpr u32 kProbeBounceCount = 6;
// bake_probes()'s ping-pong loop starts bounce 0 reading probe_buffer_a_
// (zeroed) and writing probe_buffer_b_, then alternates -- so bounce i
// writes b_ if i is even, a_ if i is odd. After kProbeBounceCount
// dispatches (indices 0..kProbeBounceCount-1), the *last* write is at
// index kProbeBounceCount-1, which lands in b_ exactly when that index is
// even, i.e. when kProbeBounceCount is odd. Resolved once here so
// render_set_'s construction-time ProbeBuffer binding and bake_probes()'s
// runtime loop can never disagree about which buffer ends up "final".
constexpr bool kProbeFinalInBufferB = (kProbeBounceCount % 2) == 1;

// Sparse voxel field dimensions. Must match COARSE_DIM/BRICK_DIM in
// assets/shaders/Builtin.SdfFieldConfig.inc.glsl exactly -- that file is the
// single shared source of truth on the GLSL side (included by every shader
// that bakes or samples the field); C++ can't #include a GLSL file, so this
// stays a documented manual sync point rather than a compiled one.
// kCoarseDim is scaled up from 16 in lockstep with BOUNDS (2.0 -> 16.0, 8x,
// in that file) so COARSE_CELL_SIZE -- and therefore voxel resolution --
// stays exactly what it was before: only the world volume actually covered
// grows, not how coarsely it's sampled.
constexpr u32 kCoarseDim = 128;
constexpr u32 kBrickDim = 8;
// Each brick stores a 1-voxel apron on every side (evaluated directly from
// the SDF, not copied from a neighbor) so trilinear sampling has continuous
// data to blend into right at brick boundaries — without this, adjacent
// bricks' data disagreed at the seam and produced visible facets.
constexpr u32 kBrickApronDim = kBrickDim + 2;
constexpr u32 kBrickVoxelCount = kBrickApronDim * kBrickApronDim * kBrickApronDim;
// Brick pool capacity. A brick is only allocated where the surface band
// crosses a coarse cell (a ~2-cell-thick shell around every surface, since
// half_diagonal in the voxelize shader slightly exceeds half a cell), so
// demand scales with total scene *surface area*: roughly area / 0.0625 x 2
// bricks. Reference points at the current 0.25-unit cell size: a full
// ground plane costs 32768 (2 x 128^2 cell-layers); games/SH's room.sdf
// loaded at .scale(5.0) -- a hollow shell ~19x12x25 whose inner+outer
// faces total ~4000 units^2 -- measured 140637 together with its other
// scenes. 262144 gives that class of scene ~1.9x headroom, at the price of
// a 1GB device-local pool (4KB per brick: 10^3 apron voxels x f32) --
// deliberately budgeted for the desktop GPUs this engine actually targets;
// if that ever needs shrinking, halve the voxel to f16
// (GL_EXT_shader_16bit_storage) or tighten the allocation band before
// shrinking the count. The original value, 2048, was sized for the old
// 16^3 grid and never scaled with the 8x kCoarseDim bump above; overflow
// drops cells in nondeterministic atomicAdd order and renders as random
// missing/stray chunks of surface. voxelize() reads the demand counter
// back after every bake and KWARNs whenever demand exceeds this.
constexpr u32 kMaxBricks = 262144;

// Chunked field (Phase 3a). Must match CHUNK_COARSE_DIM/CHUNK_TABLE_DIM in
// Builtin.SdfFieldConfig.inc.glsl exactly -- same manual-sync-point caveat
// as kCoarseDim/kBrickDim above. Deliberately independent constants from
// kCoarseDim/kMaxBricks above -- see that file's comment on why this field
// shares no GPU buffer with the fixed-cube one.
constexpr u32 kChunkCoarseDim = 16;
constexpr u32 kChunkCellCount = kChunkCoarseDim * kChunkCoarseDim * kChunkCoarseDim;
constexpr u32 kChunkTableDim = 16;
// How many chunks' worth of indirection sub-blocks chunk_indirection_buffer_
// reserves room for -- PER LEVEL (see kNumLevels below); the buffer's total
// size is kNumLevels * kMaxResidentChunks. Each level's ChunkStreamingManager
// instance (chunk_streaming_levels_) owns exactly this many slots, offset by
// its own level*kMaxResidentChunks (see ChunkStreamingManager's slot_offset
// constructor parameter).
constexpr u32 kMaxResidentChunks = 64;
// This field's own, separate, much smaller brick pool, SHARED across every
// level (a brick is just a fixed-size voxel-data blob, level-agnostic --
// what a level determines is how much world space one brick's voxels cover,
// not the blob's own byte size). Must match MAX_BRICKS in Builtin.
// ChunkVoxelize.comp.glsl/Builtin.ChunkEvict.comp.glsl exactly.
// 5 levels (sizes 4/8/16/32/64) puts the coarsest level's own streaming
// window radius (~1.5-2x its chunk size, see kStreamRadiusChunks) comfortably
// past MAX_DIST in Builtin.RaymarchShader.comp.glsl (128.0) -- with only 3
// levels (sizes 4/8/16) the coarsest window topped out around 24-32 units,
// well short of MAX_DIST, so anything the raymarcher tried to reach beyond
// that silently read as empty space (sample_clipmap_field() has no level
// past the coarsest to fall through to) even though the ray itself kept
// marching all the way to MAX_DIST looking for a hit.
constexpr u32 kNumLevels = 5;
// Per-level share of the shared pool. A level's own worst case is
// kMaxResidentChunks (64) resident chunks x CHUNK_CELL_COUNT (16^3 = 4096)
// cells = 262144 cells -- the ORIGINAL 4096 budgeted bricks for barely 1.6%
// of that as "near-surface." kMaxBricks above budgets its own 128^3 = same
// 262144-cell grid a proven-working 262144 bricks (100%, i.e. no sparsity
// assumption at all -- see that constant's comment). Reported bug: bricks
// failing to appear, specifically as the streamed window changed (i.e.
// exactly when demand across every level is highest at once) -- matches
// this engine's own prior kMaxBricks incident exactly (see its comment's
// "The original value, 2048...rendered as random missing/stray chunks of
// surface" history): Builtin.ChunkVoxelize.comp.glsl's free-list pop
// silently leaves chunk_indirection at -1 once the shared pool is empty
// ("a missing patch of surface, not a crash", per its own comment), and
// architectural detail that isn't sparse relative to a coarse cell -- e.g.
// a domain-repeated lattice of thin walls, whose near-surface shell can
// span a large fraction of a chunk's cells at once -- routinely exceeded
// the old 1.6% budget. Raised 4x (to 16384, ~6.25%) rather than all the way
// to kMaxBricks' own 100% ratio: this pool already pays per-voxel for
// CHUNK_BRICK_DIM=16's finer resolution (kChunkBrickApronDim below), so
// matching kMaxBricks' ratio verbatim would cost several extra GB of
// device-local VRAM on top of kMaxBricks' own ~1GB -- 4x is enough headroom
// for the reported case while staying within this engine's "desktop GPU
// target" budget (see kMaxBricks' own comment); raise further only if a
// scene demonstrably still overflows this.
constexpr u32 kMaxChunkBricksPerLevel = 16384;
constexpr u32 kMaxChunkBricks = kMaxChunkBricksPerLevel * kNumLevels;
// The chunked field's own brick resolution -- deliberately NOT kBrickDim
// above, even though a chunk's cell layout otherwise mirrors the fixed-cube
// field's. A clip level's own cell (chunk_level_world_size(level)/
// kChunkCoarseDim) doubles every level, so its voxel_size (cell_size/
// kChunkBrickDim) doubles right along with it -- at the old kBrickDim=8,
// level 4 of 5 (cell_size=4.0) gave voxel_size=0.5, too coarse to resolve a
// primitive thinner than that (routine for architectural detail -- thin
// walls, panels, a repeated ceiling's own ridges): the surface crossing
// between adjacent voxel samples can vanish entirely rather than just
// looking blocky, reading as the geometry losing hits specifically once it
// falls under a coarser level's responsibility -- a real, reported
// symptom, not a hypothetical one. Doubled to 16 (voxel_size=4.0/16=0.25 at
// that same level 4, matching the fixed-cube field's own uniform
// resolution) so every level -- especially the coarser ones this problem
// is specific to -- stays fine enough for ordinary architectural-scale
// detail. Kept as its own constant (not raising kBrickDim itself) so
// sdf_editor's synchronous-authoring fallback and every game keep the
// fixed-cube field's existing ~1GB brick pool exactly as it was -- only
// chunk_brick_pool_buffer_ below pays for this (roughly 455MB at this
// value: kMaxChunkBricks * kChunkBrickVoxelCount * sizeof(f32)). Must
// match CHUNK_BRICK_DIM in Builtin.SdfFieldConfig.inc.glsl exactly.
constexpr u32 kChunkBrickDim = 16;
constexpr u32 kChunkBrickApronDim = kChunkBrickDim + 2;
constexpr u32 kChunkBrickVoxelCount =
    kChunkBrickApronDim * kChunkBrickApronDim * kChunkBrickApronDim;
// Splatting: per-brick surface-point budget -- must match CHUNK_POINTS_PER_
// BRICK in Builtin.SdfFieldConfig.inc.glsl AND Builtin.ChunkPointSplat.
// comp.glsl's local_size_x (one thread per point slot) exactly; the
// How many points one cluster page holds -- the unit of splat scheduling
// (see CHUNK_CLUSTER_POINTS in Builtin.SdfFieldConfig.inc.glsl for why
// clusters exist at all). Must equal Builtin.ChunkPointSplat.comp.glsl's
// local_size_x, which GLSL requires as a literal, hence the static_assert.
constexpr u32 kChunkClusterPoints = 256;
static_assert(kChunkClusterPoints == 256,
              "Builtin.ChunkPointSplat.comp.glsl's local_size_x is a literal "
              "256 -- change it together with this.");

// How many pages one brick may claim, i.e. its point budget in clusters.
// 4 x 256 = 1024 points covers a brick's finest LOD level even where two
// surface sheets cross it; a brick wanting more drops to a coarser level
// rather than truncating.
constexpr u32 kChunkMaxClustersPerBrick = 4;

// The shared cluster pool's size. At 256 points x 8 bytes a page this is
// ~268MB of point data plus ~8MB of records -- the dominant cost of the
// splat representation, and the number to raise if bricks start dropping to
// coarser levels because the pool ran dry. Must match MAX_CLUSTERS in
// Builtin.ChunkVoxelize/ChunkEvict/ChunkPointSplat.comp.glsl.
constexpr u32 kMaxChunkClusters = 131072;

// How many levels the per-brick progressive LOD pyramid has -- must match
// CHUNK_POINT_LOD_COUNT in Builtin.SdfFieldConfig.inc.glsl. The cumulative
// counts live in every cluster record as a uvec4, so more than 4 levels
// means widening that field first.
constexpr u32 kChunkPointLodCount = 4;
static_assert(kChunkPointLodCount <= 4,
              "ChunkCluster::lod_counts is a uvec4 in the shaders; more than "
              "4 LOD levels needs a wider record there first.");

// X stride the splat dispatch packs cluster indices with (see SPLAT_
// DISPATCH_STRIDE_X in Builtin.ChunkPointSplat.comp.glsl -- must match
// exactly): kMaxChunkClusters exceeds Vulkan's guaranteed 65535
// per-dimension workgroup limit, so the dispatch is folded into 2D.
constexpr u32 kSplatDispatchStrideX = 1024;
// Edge, in render-target pixels, of the splat pass's conservative near-
// bound tiles (splat_tile_bound_buffer_) -- must match SPLAT_TILE_SIZE in
// Builtin.ChunkPointSplat.comp.glsl AND Builtin.RaymarchShader.comp.glsl
// exactly; it decides both the buffer's size here and how the two shaders
// index it.
constexpr u32 kSplatTileSize = 16;

// Imperfect shadow maps (step 5) -- must match ISM_RESOLUTION/ISM_COUNT/
// ISM_ATLAS_DIM in Builtin.ChunkShadowSplat.comp.glsl and Builtin.
// DeferredShade.comp.glsl. 64 maps of 128x128 is Dreams' own budget, and
// the whole atlas is 4MB.
constexpr u32 kIsmResolution = 128;
constexpr u32 kIsmCount = 64;
constexpr u32 kIsmAtlasDim = 8;
static_assert(kIsmAtlasDim * kIsmAtlasDim == kIsmCount,
              "the shadow atlas is a square grid of tiles");

// Cap on how many (cluster, light) shadow-casting pairs one frame can carry
// -- must match MAX_SHADOW_PAIRS in Builtin.ChunkClusterCull.comp.glsl.
// Overflowing it drops casters for that frame rather than corrupting
// anything.
constexpr u32 kMaxShadowPairs = 262144;

// The binary voxel cascades the ambient-occlusion pass traces against (step
// 6) -- must match VOXEL_CASCADE_DIM/VOXEL_CASCADE_COUNT in Builtin.
// ChunkVoxelCascade.comp.glsl and Builtin.StochasticAo.comp.glsl. Four
// cascades of 64^3 BITS is 128KB total, which is why it is rebuilt from
// scratch every frame rather than maintained incrementally.
constexpr u32 kVoxelCascadeDim = 64;
constexpr u32 kVoxelCascadeCount = 4;
constexpr u32 kVoxelCascadeWords =
    kVoxelCascadeDim * kVoxelCascadeDim * kVoxelCascadeDim / 32;
// World-space edge of the finest cascade. Each coarser one doubles it, so
// cascade 3 spans 128 units -- past that the AO ray has already run out of
// its own distance budget.
constexpr f32 kVoxelCascadeFinestExtent = 16.0f;

// Bits of PushConstants::flags -- declared here rather than with the struct
// because the AO flag needs kVoxelCascade* above to make sense.
constexpr i32 kRenderFlagAo = 16;
// Must match Builtin.SdfFieldConfig.inc.glsl's COARSE_CELL_SIZE = (2.0 *
// BOUNDS) / COARSE_DIM = 32.0 / 128.0 exactly -- the fine-voxel resolution
// is shared between the fixed-cube and chunked fields (see that file's
// comment), but only the chunked field's own C++ code (voxelize_chunk()/
// debug_verify_chunked_field()) needs it as a value here; the fixed-cube
// field never needed a C++-side copy since BOUNDS/COARSE_DIM only ever
// mattered GLSL-side there.
constexpr f32 kCoarseCellSize = 0.25f;
constexpr f32 kChunkWorldSize = static_cast<f32>(kChunkCoarseDim) * kCoarseCellSize;

// Phase 4: level L's chunk is 2^L times level 0's world size -- must match
// Builtin.SdfFieldConfig.inc.glsl's chunk_level_world_size() exactly (same
// doubling formula as chunk_world_size() in chunk_types.h, just resolved
// with this file's own kChunkWorldSize as the level-0 base). Same chunk
// *count* stays resident per level (kMaxResidentChunks, kStreamRadiusChunks
// below), so total world coverage grows exponentially with level while the
// per-level GPU memory cost stays flat -- the actual point of a clipmap.
f32 chunk_level_world_size(u32 level) {
  return kChunkWorldSize * static_cast<f32>(1u << level);
}

// A primitive whose bounding DIAMETER is smaller than this fraction of a
// level's own coarse-cell size (kCoarseCellSize * 2^level -- same doubling
// as chunk_level_world_size(), since kChunkCoarseDim stays fixed per level)
// wouldn't meaningfully change what that level's much coarser voxels
// resolve to: it's comfortably sub-cell there, not just smaller than a
// whole chunk. 0.5 (not, say, 1.0) is deliberately conservative -- a
// primitive right at a coarse cell's own scale can still visibly shift
// what that cell bakes to, so only a primitive well under it gets skipped.
// Used by update_streaming()'s surgical dirty-rebake path to skip queueing
// a level's touched chunks for a primitive too small to matter there (see
// kMaxForcedRebakesPerFrame's own comment for why that queue's size
// matters) -- level 0 is never skipped regardless of this, since it's
// always what's visible up close.
constexpr f32 kLevelSkipRelativeSize = 0.5f;

bool primitive_matters_at_level(f32 bounding_radius, u32 level) {
  if (level == 0) {
    return true;
  }
  f32 cell_size = kCoarseCellSize * static_cast<f32>(1u << level);
  return (2.0f * bounding_radius) >= cell_size * kLevelSkipRelativeSize;
}

// Streaming policy, shared by every level (Phase 3b proved the load/evict/
// ring-delay mechanics at a single level; Phase 4 just runs kNumLevels
// independent instances of the same policy -- see chunk_streaming_levels_).
// Chebyshev radius (in chunks) update_streaming() keeps loaded around the
// camera, per level -- (2*1+1)^3 = 27 candidate chunks, safely under
// kMaxResidentChunks (64) with headroom so the window doesn't thrash
// against the slot cap right at its own edge.
constexpr i32 kStreamRadiusChunks = 1;
// Loads and evictions are budgeted separately per level (each dispatch is
// cheap relative to a full rebake, but still real GPU work) -- this is the
// "no vkDeviceWaitIdle stall" lever the plan calls for: spread over N
// frames instead of one, at N x the latency to fully catch up after a big
// camera jump. Applied per level, so total per-frame chunk work scales
// with kNumLevels -- still far cheaper than one full rebake either way.
constexpr u32 kMaxChunkBakesPerFrame = 2;
// Separate, larger per-level budget for pending_forced_evictions_ (a
// dirty-scene-edit-triggered in-place rebake -- see update_streaming()'s
// own comment) rather than sharing kMaxChunkBakesPerFrame with ordinary
// camera-driven boundary streaming. Boundary streaming is continuous,
// every-frame background work that has to stay cheap forever; a forced
// rebake is a one-shot burst tied to something the user just did and is
// actively watching -- capping it at the SAME tiny per-frame allowance as
// background streaming meant a single edit's touched chunks (e.g. ~32
// across every level for one ordinary primitive in a small scene) could
// take several frames to fully resolve, each one still slightly stale.
// Set well above a typical edit's
// per-level chunk count so ordinary edits clear in one frame, but still
// well under kMaxResidentChunks (64) so the worst case -- a brute-force
// full-scene sweep from an unbounded primitive (see geometry_bounding_
// radius()) -- spreads over a handful of frames instead of one single,
// much larger stall.
//
// MEASURED, and lowered because of it. GPU timestamps put a single bake
// submission at 646-2130ms MEAN with an 8.2 SECOND peak on a large scene:
// one chunk bake is ~4096 cells, each near-surface one filling an 18^3
// brick with a full scene_map() evaluation per voxel plus the splat point
// pyramid, so batching 16 of them into one submission asks for over a
// billion primitive evaluations at once. The async queue saturates for
// seconds, the ring's fence eventually blocks the CPU, and that is the
// stutter felt when crossing a chunk boundary or changing LOD level.
//
// 4 keeps an ordinary edit resolving within a couple of frames while
// cutting the worst-case submission to a quarter. It is a mitigation, not
// the fix: the per-chunk cost itself is the real problem (see the
// per-voxel scene_map() call in Builtin.ChunkVoxelize.comp.glsl -- it
// evaluates every primitive that survives a cull radius, with no per-chunk
// candidate list to shorten that loop the way Dreams' evaluator does).
constexpr u32 kMaxForcedRebakesPerFrame = 4;
// Phase 6: capacity of chunk_voxelize_batch_buffer_/chunk_evict_batch_
// buffer_ -- see their own comment. update_streaming() now collects every
// chunk a frame needs (re)baked/evicted, across every level, into ONE
// flat batch per dispatch type instead of one dispatch per chunk. Worst
// case per type is (kMaxChunkBakesPerFrame + kMaxForcedRebakesPerFrame)
// per level * kNumLevels = (2+16)*5 = 90; 96 leaves a little headroom
// without being a meaningfully bigger allocation (each entry is a handful
// of floats/ints).
constexpr u32 kMaxChunkBatchSize = 96;

// Hard cap on how many chunks ONE async submission may bake, across every
// level combined -- separate from, and much tighter than, the per-level
// budgets above.
//
// MEASURED. The per-level budgets add up to (2 + 4) * 5 = 30 chunks in a
// single dispatch, and at ~30ms per chunk that is a ~900ms dispatch. Which
// would be fine if "async compute" meant what it sounds like -- but
// async_compute_queue is queue index 1 of the GRAPHICS family (see
// vulkan_device.cpp), and two queues of one family share the hardware
// engine rather than running side by side. So a 900ms bake does not
// overlap the frame, it starves it: wall-clock frames were still spiking
// past 1.4 SECONDS after the graphics queue stopped waiting on the bake at
// all, purely from GPU contention.
//
// Not waiting on the bake is what makes THIS fixable at all, though: a
// short submission is no longer a correctness constraint (nothing blocks
// on it), only a scheduling one, so the batch can be sliced as finely as
// the GPU wants without adding latency anywhere that matters. Whatever a
// frame declines is simply re-offered next frame, nearest-first.
//
// 4 keeps a submission near 100ms even at the coarse levels' per-chunk
// cost, and the ring is kAsyncRingDepth deep so the CPU can keep several
// in flight without ever blocking.
//
// Now the CEILING on an adaptive figure rather than the figure itself --
// see kSubmissionTargetMs.
constexpr u32 kMaxChunksPerSubmission = 8;

// How much GPU time one async submission should aim to occupy.
//
// A fixed chunk count is the wrong control variable: the same 2 chunks are
// 10ms of work in an empty region and 170ms across a wall at the coarsest
// clip level, and it is the milliseconds that starve the frame, not the
// count. Targeting time instead makes the streamer self-tuning -- it
// catches up fast where chunks are cheap and backs off exactly where they
// are expensive, on whatever GPU it happens to be running on.
//
// This started at 40ms, on the reasoning that nothing waits on the bake so
// it only has to avoid "monopolising the GPU long enough to be felt" --
// but 40ms IS long enough to be felt. async_compute_queue is a second
// queue in the SAME family on this hardware (see its declaration comment),
// sharing the GPU's execution units with graphics, so a submission sized
// to this figure starves the frame for up to ~2-3 frames' worth of time
// every time it lands -- continuously, while the camera is crossing chunk
// boundaries. That was the residual "moving between chunks still hitches"
// after the bake went async: the CPU no longer waited, but the GPU still
// did. ~12ms keeps each submission under a 60Hz frame (the driver can
// interleave graphics between submissions, just not preempt within one),
// trading total streaming throughput (kAsyncRingDepth keeps two in flight,
// so the streamer still makes steady progress) for never occupying the GPU
// longer than roughly a frame at a time.
constexpr f64 kSubmissionTargetMs = 12.0;

// How far AHEAD of the camera the streaming window is centred, expressed in
// FRAMES of its current per-frame step.
//
// Frames rather than seconds because update_streaming() is handed a camera
// position and nothing else -- no delta time -- and measuring the step per
// call makes this self-contained. It also behaves the right way when frame
// times stretch: a slow frame means both a longer step AND a longer real
// wait for the bake, so a lead measured in frames grows exactly when it
// needs to. kMaxStreamLeadChunks caps it either way.
//
// Chunks are otherwise requested at the moment they are needed: the window
// is centred on the camera, so a chunk starts baking as the camera crosses
// into range of it and is empty until the bake lands. Biasing the centre
// down the direction of travel starts that bake earlier, and -- because
// ChunkStreamingManager::update() also sorts to_load by distance from the
// position it is given -- reorders the queue so the chunks about to be
// entered are baked first.
constexpr f32 kStreamLeadFrames = 45.0f;

// Ceiling on that bias, in chunks of whichever level is being planned.
//
// The lead must never push the camera's OWN chunk out of the window, or the
// streamer would race ahead and leave the viewer standing in unbaked space.
// A bias under one chunk width can move the centre chunk index by at most
// one, which a kStreamRadiusChunks=1 window still covers, so 0.75 leaves
// margin without needing a wider (and much more expensive) window.
constexpr f32 kMaxStreamLeadChunks = 0.75f;

// Smoothing on the camera velocity the bias is computed from. Raw
// frame-to-frame velocity is far too noisy to steer streaming with -- a
// single long frame (which is exactly what happens while chunks are being
// baked) would otherwise swing the window wildly.
constexpr f32 kStreamVelocitySmoothing = 0.1f;

// How many candidate entries one chunk's list can name before the bake
// gives up on listing them and evaluates the whole scene for that chunk
// instead (see chunk_candidate_buffer_). A scene dense enough to exceed it
// in one chunk simply bakes that chunk at the old cost.
//
// MEASURED, and raised because of it. At 64 -- "a chunk overlaps a handful
// of things", sized before entries could name individual repetition
// instances -- 30 of 224 chunks of DiegosOffice still took the fallback,
// and they are the expensive ones: a coarse clip level's chunk is 64 world
// units wide, so a 10-unit repetition cell puts dozens of copies inside it
// and the list fills up long before the scene's own 25 primitives have
// been walked. Every one of those chunks then folds the entire scene at
// every voxel, which is exactly what the list exists to avoid.
//
// 256 clears that with room to spare and costs almost nothing: the two
// candidate buffers together are 20 bytes per entry, so the whole ring is
// under 2MB.
constexpr u32 kMaxCandidatesPerChunk = 256;


// Cap on how many repetition instances of a SINGLE primitive one chunk's
// list will name individually before falling back to the primitive's own
// repeat_*() fold for it (see PrimitiveBound).
//
// Resolving instances is a large win when a chunk contains one or two
// copies -- the common case at the fine clip levels, where a chunk is
// smaller than a repetition cell. It stops being one when a chunk spans
// many cells, which is routine at coarse levels: enumerating 49 copies
// costs 49 list entries and 49 evaluations per sample, where repeat_
// limited()'s neighbour check costs 8 regardless of how many copies exist.
// 8 is the break-even point, and keeping the number small also keeps one
// repeated primitive from crowding everything else out of the list.
constexpr i64 kMaxResolvedInstancesPerPrimitive = 8;
// How many update_streaming() tick()s must pass after a load/evict before
// that chunk's slot is trusted safe to reuse -- see ChunkStreamingManager's
// header comment for exactly what hazard this guards against (two frames'
// command buffers aren't completion-ordered relative to each other without
// an explicit fence/barrier). Deliberately a fixed conservative constant
// rather than wired to the real swapchain's max_frames_in_flight (typically
// 1-2 for double/triple buffering): comfortably covers that in practice,
// and avoids a new dependency from chunk_streaming_ (constructed before
// context_->swapchain necessarily reflects its final image count) on
// swapchain internals for a value where erring slightly high costs nothing
// but a frame or two of extra latency, not correctness.
constexpr u32 kFramesInFlightDelay = 3;

// Phase 5: single camera-centered GI cascade for the chunked field (see
// Builtin.ChunkProbeBake.comp.glsl) -- must match PROBE_DIM/
// GI_CASCADE_CELL_SIZE there exactly. Deliberately just one cascade for
// now (the plan's stated ideal is one per voxel clip level), matching this
// whole effort's established "prove the mechanics with a smaller number
// first" pattern (see kNumLevels=3, not 5) -- adding more is mechanical
// repetition of this same one once it's verified, not a new design.
constexpr u32 kGiProbeDim = 16;
constexpr f32 kGiCascadeCellSize = 2.0f;
// Recenter once the camera drifts more than half a cascade cell from the
// cascade's current center -- classic terrain-clipmap texture behavior
// (discrete whole-cell steps, not continuous tracking): keeps the probe
// grid's own texel positions stable between recenters (each recenter
// snaps to the nearest whole GI_CASCADE_CELL_SIZE multiple), which is what
// avoids every single probe needing to move -- see update_gi_cascade().
constexpr f32 kGiCascadeRecenterThreshold = kGiCascadeCellSize * 0.5f;

// Cap on simultaneously baked static primitives. Must match
// MAX_SCENE_PRIMITIVES in Builtin.RaymarchShader.comp.glsl exactly (that's
// the one place it's compile-time-fixed, for the sampler array; everywhere
// else the count is just how many of this capacity are actually in use).
constexpr u32 kMaxScenePrimitives = 1000;

// Cap on simultaneously registered layers (see GeometrySystem::SceneLayer).
// Only bounds layer_buffer_'s allocated size -- there's no shader-side
// fixed-size array depending on this the way scene_textures depends on
// kMaxScenePrimitives, since layers are read from a plain storage buffer
// (LayerBuffer's `Layer layers[]` is unsized in both
// Builtin.RaymarchVoxelize.comp.glsl and Builtin.RaymarchShader.comp.glsl --
// raising this needs no shader recompile). Index 0 is the always-present
// default layer GeometrySystem::load_scene() never hands out (see its own
// comment), so this must exceed kMaxScenePrimitives by at least 1 for tools
// that put one primitive per layer -- e.g. the SDF editor's "Add Primitive"
// (see SdfEditorWindow::on_add_clicked()), which otherwise silently stops
// rendering anything past the 15th primitive even though
// kMaxScenePrimitives allows far more.
constexpr u32 kMaxLayers = 1001;

// Cap on simultaneously registered lights. Only bounds light_buffer_'s
// allocated size -- lights are read from a plain storage buffer (see
// GpuLight below), not a fixed-size shader array, so this could grow freely
// if a scene ever legitimately needed more.
constexpr u32 kMaxLights = 128;

// Matches the `Primitive` struct in Builtin.RaymarchVoxelize.comp.glsl.
struct GpuPrimitive {
  f32 position_type[4]; // xyz = world position, w = PrimitiveType as float.
  // xyz = Geometry::params, w = Geometry::extra_param -- per-type meaning,
  // see SdfPrimitiveDef::params' comment (sdf_scene.h) and primitive_sdf()
  // in Builtin.RaymarchVoxelize.comp.glsl for exactly what each type reads.
  f32 params[4];
  // Unit quaternion (x,y,z,w) rotating the primitive's local space into
  // world space -- computed from Geometry::rotation's Euler angles (see
  // rebuild_static_scene() below). The voxelize shader applies its inverse
  // (conjugate) to a sample point before evaluating every type but Plane
  // (always the horizontal y=height plane, which never rotates); a sphere
  // reads it too but is unaffected (rotation-invariant).
  f32 rotation[4];
  // x = Geometry::param_expr_scale (the accumulated uniform scale
  // resolve_params() applies to formula-driven slots as s*f(p/s) -- see
  // that field's comment); yzw unused padding, kept as a full vec4 so the
  // std430 layout stays a whole number of vec4s on both sides.
  f32 expr_scale[4];
  // Domain repetition (see https://iquilezles.org/articles/sdfrepetition/
  // and Builtin.SdfSceneCommon.inc.glsl's Primitive struct comment): x =
  // Geometry::repetition_mode as a float (0 = None, the zero-init default --
  // left untouched for a volumetric, which never repeats, see
  // rebuild_static_scene() below); yzw = Geometry::repetition_cell.xyz.
  f32 repeat_mode_cell[4];
  // xyz = Geometry::repetition_count.xyz. w = a conservative world-space
  // bounding-sphere radius around position_type.xyz (or
  // kUnboundedBoundingRadius -- see that constant's comment, geometry_
  // system.h), computed by geometry_bounding_radius() and used by
  // scene_map()'s cull_radius pre-check (Builtin.SdfSceneCommon.inc.glsl)
  // to skip evaluating a primitive that plainly can't matter at a given
  // sample point. Was unused padding before that field existed; repurposed
  // rather than growing every GpuPrimitive by another vec4.
  f32 repeat_count[4];
  // Domain deformation -- see Geometry::twist/bend/displace_amplitude/
  // displace_frequency's comment. x = twist, y = bend, z =
  // displace_amplitude, w = displace_frequency.
  f32 deform[4];
};

// Matches the `Layer` struct in Builtin.RaymarchVoxelize.comp.glsl.
struct GpuLayer {
  f32 op_smoothness[4]; // x=LayerOperation as float, y=smoothness.
  i32 range[4];           // x=primitive_start, y=primitive_count.
};

// Matches the `Light` struct in Builtin.RaymarchShader.comp.glsl.
struct GpuLight {
  f32 vector_type[4]; // xyz = Light::vector (direction or position -- see
                     // LightType's comment), w = LightType as float.
  f32 colour_intensity[4]; // rgb = Light::colour, a = Light::intensity.
  // x = the index (into primitives[]/scene_textures/scene_diffuse_colours)
  // of the emissive primitive this Point light was synthesized from (see
  // rebuild_static_scene()'s emissive-primitive loop), or -1 for a light
  // with no associated primitive (every authored SdfLightDef light, plus
  // the hardcoded fallback). shadow_march() (Builtin.BakedFieldCommon.inc.
  // glsl) needs this: a synthesized light sits at its own primitive's
  // position, so a shadow ray toward it would otherwise always find that
  // same primitive's own shell in the way first (any point outside a
  // convex shape has to cross its surface to reach its interior/center) --
  // this tells the shadow march which primitive to treat as see-through
  // for that one light's rays, rather than a real occluder. yzw unused
  // padding, kept as a full vec4 so the std430 layout stays a whole number
  // of vec4s on both sides, matching every other GPU-side struct here.
  f32 source_primitive[4];
};

// A single compiled "parametric attribute" expression -- matches the
// `ParamExpr` struct in Builtin.RaymarchVoxelize.comp.glsl exactly (plain
// C arrays, not std::array, so the memory layout is the flat, tightly
// packed one std430 also produces for an all-scalar-array struct like this
// -- see rebuild_static_scene()'s load_data() call, a raw memcpy). 4
// consecutive entries per primitive (params.x/y/z/extra_param, in that
// order) in param_expr_buffer_; instruction_count == 0 means "no formula
// for this slot, use the plain constant instead" (see
// Geometry::param_expressions).
struct GpuParamExpr {
  i32 op[kMaxExprInstructions]{};
  f32 operand[kMaxExprInstructions]{};
  i32 instruction_count = 0;
};

// Mirrors ChunkCluster in Builtin.SdfFieldConfig.inc.glsl -- used here only
// to size chunk_cluster_buffer_ and to zero records at reset, never built
// field by field (the voxelize pass is the only writer).
struct GpuChunkCluster {
  f32 bbox_min_cell[4];
  f32 normal_cone[4];
  u32 lod_counts[4];
  u32 meta[4];
};
static_assert(sizeof(GpuChunkCluster) == 64,
              "ChunkCluster's std430 array stride is 64 bytes (four 16-byte "
              "vectors); the engine-side mirror has to match exactly.");

// Matches the push_constant block in Builtin.TaaResolve.comp.glsl.
struct TaaResolvePushConstants {
  f32 camera_position[4];
  f32 camera_forward[4];
  f32 camera_right[4];
  f32 camera_up[4];
  f32 prev_camera_position[4];
  f32 prev_camera_forward[4];
  f32 prev_camera_right[4];
  f32 prev_camera_up[4];
  f32 jitter_x;
  f32 jitter_y;
  f32 blend;
  i32 pad;
};

// How many frames the sub-pixel jitter sequence runs before repeating. A
// Halton (2,3) sequence of this length spreads samples evenly inside the
// pixel; 8 is the usual compromise -- long enough that edges resolve
// smoothly, short enough that a camera which stops moving settles quickly
// rather than continuing to shimmer through a long cycle.
constexpr u32 kTaaJitterCount = 8;

// The radical inverse of `index` in `base` -- the standard Halton
// construction. Written out rather than pulled from a library since it is
// four lines and used in exactly one place.
f32 halton(u32 index, u32 base) {
  f32 result = 0.0f;
  f32 fraction = 1.0f / static_cast<f32>(base);
  u32 i = index;
  while (i > 0) {
    result += static_cast<f32>(i % base) * fraction;
    i /= base;
    fraction /= static_cast<f32>(base);
  }
  return result;
}

// The blend weight given to the NEW sample each frame once history exists.
// 0.1 keeps ten frames' worth of history in flight, which is what makes
// one-sample-per-pixel stochastic effects usable; lower ghosts more on
// motion, higher anti-aliases less.
constexpr f32 kTaaBlendFactor = 0.1f;

// Wrap for noise_frame_ (see its declaration). Long enough that the repeat
// is invisible next to TAA's ~20-frame effective window, short enough that
// the shader-side hashes (which fold the seed through fract() at float
// precision) never see a value large enough to degrade.
constexpr u32 kNoiseFrameWrap = 1024;

} // namespace

VulkanRaymarchShader::VulkanRaymarchShader(VulkanContext &context)
    : context_(&context),
      voxelize_stage_(context, BUILTIN_SHADER_NAME_VOXELIZE, "comp",
                     VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_voxelize_stage_(context, BUILTIN_SHADER_NAME_CHUNK_VOXELIZE, "comp",
                           VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_debug_query_stage_(context, BUILTIN_SHADER_NAME_CHUNK_DEBUG_QUERY,
                              "comp", VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_evict_stage_(context, BUILTIN_SHADER_NAME_CHUNK_EVICT, "comp",
                        VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_probe_bake_stage_(context, BUILTIN_SHADER_NAME_CHUNK_PROBE_BAKE,
                             "comp", VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_cluster_cull_stage_(context, BUILTIN_SHADER_NAME_CHUNK_CLUSTER_CULL,
                               "comp", VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_shadow_splat_stage_(context, BUILTIN_SHADER_NAME_CHUNK_SHADOW_SPLAT,
                               "comp", VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_voxel_cascade_stage_(context, BUILTIN_SHADER_NAME_CHUNK_VOXEL_CASCADE,
                                "comp", VK_SHADER_STAGE_COMPUTE_BIT),
      stochastic_ao_stage_(context, BUILTIN_SHADER_NAME_STOCHASTIC_AO, "comp",
                          VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_point_splat_stage_(context, BUILTIN_SHADER_NAME_CHUNK_POINT_SPLAT,
                              "comp", VK_SHADER_STAGE_COMPUTE_BIT),
      probe_bake_stage_(context, BUILTIN_SHADER_NAME_PROBE_BAKE, "comp",
                       VK_SHADER_STAGE_COMPUTE_BIT),
      render_stage_(context, BUILTIN_SHADER_NAME_RAYMARCH, "comp",
                   VK_SHADER_STAGE_COMPUTE_BIT),
      deferred_shade_stage_(context, BUILTIN_SHADER_NAME_DEFERRED_SHADE, "comp",
                           VK_SHADER_STAGE_COMPUTE_BIT),
      bloom_blur_h_stage_(context, BUILTIN_SHADER_NAME_BLOOM_BLUR_H, "comp",
                        VK_SHADER_STAGE_COMPUTE_BIT),
      post_composite_stage_(context, BUILTIN_SHADER_NAME_POST_COMPOSITE, "comp",
                           VK_SHADER_STAGE_COMPUTE_BIT),
      taa_resolve_stage_(context, BUILTIN_SHADER_NAME_TAA_RESOLVE, "comp",
                        VK_SHADER_STAGE_COMPUTE_BIT) {
  if (!voxelize_stage_.is_valid() || !chunk_voxelize_stage_.is_valid() ||
      !chunk_debug_query_stage_.is_valid() || !chunk_evict_stage_.is_valid() ||
      !chunk_probe_bake_stage_.is_valid() ||
      !chunk_cluster_cull_stage_.is_valid() ||
      !chunk_shadow_splat_stage_.is_valid() ||
      !chunk_voxel_cascade_stage_.is_valid() ||
      !stochastic_ao_stage_.is_valid() ||
      !chunk_point_splat_stage_.is_valid() ||
      !probe_bake_stage_.is_valid() ||
      !render_stage_.is_valid() || !deferred_shade_stage_.is_valid() ||
      !bloom_blur_h_stage_.is_valid() ||
      !post_composite_stage_.is_valid() || !taa_resolve_stage_.is_valid()) {
    KERROR("Unable to create shader module(s) for the raymarch field "
          "pipeline.");
    return;
  }

  // Output/bloom-scratch/post-process images, sized off render_scale_ (1.0
  // unless set_render_scale() has been called) -- see
  // recreate_render_target_images().
  base_width_ = context_->framebuffer_width;
  base_height_ = context_->framebuffer_height;
  recreate_render_target_images();

  // Sparse voxel field storage.
  const u64 indirection_size =
      static_cast<u64>(kCoarseDim) * kCoarseDim * kCoarseDim * sizeof(i32);
  const u64 brick_pool_size =
      static_cast<u64>(kMaxBricks) * kBrickVoxelCount * sizeof(f32);

  indirection_buffer_.emplace(*context_, indirection_size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  // ALLOCATED LAZILY -- this starts as a single brick's worth of memory,
  // not the full ~1GB pool, and ensure_fixed_brick_pool() grows it the
  // first time the fixed-cube field is actually baked.
  //
  // The two fields' brick pools are ~1.05GB (this one) and ~1.91GB (the
  // chunked one), which is essentially a 4GB card's entire budget between
  // them -- and nothing can use both at once: the editor runs purely on the
  // chunked field, a game purely on the fixed one. Holding both reserved
  // from construction is what forced the splat point pool into host memory,
  // where every bake wrote it across PCIe and every frame's splat pass read
  // it back. Deferring this one buys the room to keep the points in VRAM,
  // and costs a game nothing but a one-time reallocation on its first bake.
  const u64 fixed_pool_initial_size =
      fixed_brick_pool_ready_ ? brick_pool_size
                              : static_cast<u64>(kBrickVoxelCount) * sizeof(f32);
  brick_pool_buffer_.emplace(*context_, fixed_pool_initial_size,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  // Host-visible, unlike the two above: voxelize() reads the counter back
  // after every bake to detect brick-pool overflow (demand > kMaxBricks,
  // which the shader can only handle by silently dropping cells). A single
  // u32 the GPU only touches via atomicAdd -- not worth a staging-copy
  // round trip.
  brick_counter_buffer_.emplace(*context_, sizeof(u32),
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // Chunked field (Phase 3a) -- see its own member declarations' comments.
  // Own separate buffers throughout, none shared with the fixed-cube field
  // above.
  // Phase 4: NUM_CHUNK_LEVELS-partitioned -- level L's table/indirection
  // sub-range starts at L*kChunkTableDim^3 / L*kMaxResidentChunks
  // respectively (see voxelize_chunk()/evict_chunk()'s table-index math
  // and chunk_streaming_levels_'s per-level slot_offset).
  const u64 chunk_table_size = static_cast<u64>(kNumLevels) * kChunkTableDim *
      kChunkTableDim * kChunkTableDim * sizeof(i32);
  const u64 chunk_indirection_size = static_cast<u64>(kNumLevels) *
      kMaxResidentChunks * kChunkCellCount * sizeof(i32);
  const u64 chunk_brick_pool_size =
      static_cast<u64>(kMaxChunkBricks) * kChunkBrickVoxelCount * sizeof(f32);
  const u64 chunk_brick_primitive_size =
      static_cast<u64>(kMaxChunkBricks) * sizeof(i32);

  chunk_table_buffer_.emplace(*context_, chunk_table_size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  // Host-visible for the same reason chunk_table_buffer_ above is: the CPU
  // rewrites individual entries as chunks are published and retired, every
  // frame that streams. Tiny (kNumLevels * kMaxResidentChunks u32s).
  chunk_slot_published_buffer_.emplace(
      *context_,
      static_cast<u64>(kNumLevels) * kMaxResidentChunks * sizeof(u32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  chunk_indirection_buffer_.emplace(*context_, chunk_indirection_size,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_brick_pool_buffer_.emplace(*context_, chunk_brick_pool_size,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_brick_primitive_buffer_.emplace(*context_, chunk_brick_primitive_size,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_brick_demand_buffer_.emplace(*context_,
                                    sizeof(u32) * kChunkBakeStatCount,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  // DEVICE-LOCAL, and the stack pointer especially. Every cell that decides
  // it needs a brick does an atomicAdd on chunk_brick_free_list_top_buffer_
  // and then reads one entry out of the list -- across a whole bake that is
  // hundreds of thousands of contended atomics on a SINGLE dword. In host
  // memory each of those crossed PCIe, which turned the allocator itself
  // into a serialization point in the middle of the hottest loop the engine
  // has. Nothing on the CPU reads either buffer; reset_chunked_field()
  // seeds them once through a staging copy (see upload_to_device_local()).
  chunk_brick_free_list_buffer_.emplace(
      *context_, static_cast<u64>(kMaxChunkBricks) * sizeof(i32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_brick_free_list_top_buffer_.emplace(
      *context_, sizeof(u32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_debug_query_output_buffer_.emplace(
      *context_, sizeof(ChunkDebugQueryOutput),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // Phase 6: see chunk_voxelize_batch_slot_buffer_/chunk_voxelize_batch_
  // data_buffer_/chunk_evict_batch_buffer_'s own comment. Host-visible --
  // update_streaming() writes these directly from the CPU every call that
  // has anything to record, same as chunk_table_buffer_ above.
  // kAsyncRingDepth regions, not one: a single shared region is what forced
  // ensure_async_cmd() to wait on EVERY ring slot's fence before it could
  // memcpy this frame's entries, since the previous submission might still
  // be reading them. With one region per slot, a submission can only ever
  // collide with its own slot's previous use -- a full ring lap ago -- so
  // waiting on that one fence is enough, and the CPU stops blocking on the
  // GPU's most recent bake every frame the camera moves.
  chunk_voxelize_batch_slot_buffer_.emplace(
      *context_,
      static_cast<u64>(kMaxChunkBatchSize) * kAsyncRingDepth * sizeof(i32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  chunk_voxelize_batch_data_buffer_.emplace(
      *context_,
      static_cast<u64>(kMaxChunkBatchSize) * kAsyncRingDepth * sizeof(glm::vec4),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  chunk_evict_batch_buffer_.emplace(
      *context_,
      static_cast<u64>(kMaxChunkBatchSize) * kAsyncRingDepth * sizeof(i32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // The splat point clusters -- DEVICE-LOCAL, which it could not be while
  // the fixed-cube field held ~1GB from construction (see brick_pool_
  // buffer_'s creation comment for what changed).
  //
  // This is the buffer the whole splat pipeline is built on: the bake
  // writes every point into it, and every frame the splat pass reads back
  // the points of every visible cluster. In host memory all of that crossed
  // PCIe -- measured at ~10ms per frame in the streaming prepass even on
  // frames with no bake at all, plus a large share of a bake that ran
  // 100-300ms per chunk. Nothing on the CPU ever touches it, so host
  // visibility bought nothing.
  chunk_cluster_point_buffer_.emplace(
      *context_,
      static_cast<u64>(kMaxChunkClusters) * kChunkClusterPoints * sizeof(u32) * 2,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  // ALL DEVICE-LOCAL, for exactly the reason the point buffer above already
  // is -- these were simply left behind when it moved.
  //
  // chunk_cluster_buffer_ is the worst of them: kMaxChunkClusters records
  // of 64 bytes is 8MB, and Builtin.ChunkClusterCull.comp.glsl reads EVERY
  // one of them every frame (it sweeps the whole pool, since meta.y == 0 is
  // what marks a page free), then the splat pass reads the survivors again.
  // In host memory that is the whole pool pulled across PCIe once per
  // frame, on top of the ~10ms the point buffer's own comment above already
  // measured for the same mistake. The other three are written by the bake
  // -- one cluster record and one owner entry per page claimed, plus the
  // free-list atomics -- and read by the splat/evict passes. The CPU only
  // ever seeds them, once, in reset_chunked_field().
  chunk_cluster_buffer_.emplace(
      *context_, static_cast<u64>(kMaxChunkClusters) * sizeof(GpuChunkCluster),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_cluster_free_list_buffer_.emplace(
      *context_, static_cast<u64>(kMaxChunkClusters) * sizeof(i32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_cluster_free_top_buffer_.emplace(
      *context_, sizeof(u32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_brick_cluster_buffer_.emplace(
      *context_,
      static_cast<u64>(kMaxChunkBricks) * kChunkMaxClustersPerBrick * sizeof(i32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  // Per-chunk candidate lists -- see chunk_candidate_buffer_'s own comment.
  // Host-visible: the CPU builds them fresh for every batch, exactly like
  // the batch entries themselves, and they are tiny.
  chunk_candidate_buffer_.emplace(
      *context_,
      static_cast<u64>(kMaxChunkBatchSize) * kMaxCandidatesPerChunk *
          kAsyncRingDepth * sizeof(i32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  chunk_candidate_range_buffer_.emplace(
      *context_,
      static_cast<u64>(kMaxChunkBatchSize) * kAsyncRingDepth * sizeof(i32) * 4,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  chunk_candidate_offset_buffer_.emplace(
      *context_,
      static_cast<u64>(kMaxChunkBatchSize) * kMaxCandidatesPerChunk *
          kAsyncRingDepth * sizeof(glm::vec4),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  // Host-visible like the rest of the per-batch inputs: the CPU rewrites it
  // for every submission and nothing on the GPU writes it back.
  chunk_cell_alias_buffer_.emplace(
      *context_,
      static_cast<u64>(kMaxChunkBatchSize) * kChunkCellCount * kAsyncRingDepth *
          sizeof(i32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // Step 3's cull output -- see chunk_visible_cluster_buffer_'s own comment.
  // Device-local: written and read entirely on the GPU, every frame.
  chunk_visible_cluster_buffer_.emplace(
      *context_, static_cast<u64>(kMaxChunkClusters) * sizeof(u32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_cull_args_buffer_.emplace(
      *context_, sizeof(u32) * 4,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  // Step 5's shadow work list and atlas -- see their member comments. All
  // device-local: written and read entirely on the GPU, every frame.
  chunk_shadow_pair_buffer_.emplace(
      *context_, static_cast<u64>(kMaxShadowPairs) * sizeof(u32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_shadow_args_buffer_.emplace(
      *context_, sizeof(u32) * 4,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_resident_cluster_buffer_.emplace(
      *context_, static_cast<u64>(kMaxChunkClusters) * sizeof(u32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_resident_args_buffer_.emplace(
      *context_, sizeof(u32) * 4,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  voxel_cascade_buffer_.emplace(
      *context_,
      static_cast<u64>(kVoxelCascadeCount) * kVoxelCascadeWords * sizeof(u32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  shadow_atlas_buffer_.emplace(
      *context_,
      static_cast<u64>(kIsmCount) * kIsmResolution * kIsmResolution * sizeof(u32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  // Phase 5: the chunked field's GI cascade -- same size/usage pattern as
  // probe_buffer_a_/probe_buffer_b_ above (device-local, buffer a gets
  // TRANSFER_DST since bake_gi_cascade_bounce() zeroes it via
  // vkCmdFillBuffer once, on the first bake ever -- see
  // gi_probes_initialized_'s comment).
  const u64 chunk_gi_probe_buffer_size =
      static_cast<u64>(kGiProbeDim) * kGiProbeDim * kGiProbeDim * sizeof(f32) * 4;
  chunk_gi_probe_buffer_a_.emplace(*context_, chunk_gi_probe_buffer_size,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_gi_probe_buffer_b_.emplace(*context_, chunk_gi_probe_buffer_size,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  if (!chunk_table_buffer_->is_valid() || !chunk_indirection_buffer_->is_valid() ||
      !chunk_brick_pool_buffer_->is_valid() ||
      !chunk_brick_primitive_buffer_->is_valid() ||
      !chunk_brick_demand_buffer_->is_valid() ||
      !chunk_brick_free_list_buffer_->is_valid() ||
      !chunk_brick_free_list_top_buffer_->is_valid() ||
      !chunk_debug_query_output_buffer_->is_valid() ||
      !chunk_voxelize_batch_slot_buffer_->is_valid() ||
      !chunk_voxelize_batch_data_buffer_->is_valid() ||
      !chunk_evict_batch_buffer_->is_valid() ||
      !chunk_cluster_point_buffer_->is_valid() ||
      !chunk_cluster_buffer_->is_valid() ||
      !chunk_cluster_free_list_buffer_->is_valid() ||
      !chunk_cluster_free_top_buffer_->is_valid() ||
      !chunk_brick_cluster_buffer_->is_valid() ||
      !chunk_slot_published_buffer_->is_valid() ||
      !chunk_candidate_buffer_->is_valid() ||
      !chunk_candidate_range_buffer_->is_valid() ||
      !chunk_candidate_offset_buffer_->is_valid() ||
      !chunk_cell_alias_buffer_->is_valid() ||
      !chunk_visible_cluster_buffer_->is_valid() ||
      !chunk_cull_args_buffer_->is_valid() ||
      !chunk_shadow_pair_buffer_->is_valid() ||
      !chunk_shadow_args_buffer_->is_valid() ||
      !shadow_atlas_buffer_->is_valid() ||
      !chunk_resident_cluster_buffer_->is_valid() ||
      !chunk_resident_args_buffer_->is_valid() ||
      !voxel_cascade_buffer_->is_valid() ||
      !chunk_gi_probe_buffer_a_->is_valid() || !chunk_gi_probe_buffer_b_->is_valid()) {
    KERROR("Failed to create chunked-field buffers.");
    return;
  }

  // GeometrySystem's registered scene: primitive_buffer_/
  // primitive_colour_buffer_ are written directly from the CPU (host
  // visible) whenever rebuild_static_scene() runs, since that's cheap and
  // infrequent -- unlike indirection/brick_pool/brick_counter above, which
  // only the GPU ever writes.
  const u64 primitive_buffer_size =
      static_cast<u64>(kMaxScenePrimitives) * sizeof(GpuPrimitive);
  const u64 primitive_colour_size =
      static_cast<u64>(kMaxScenePrimitives) * sizeof(f32) * 4;
  const u64 layer_buffer_size = static_cast<u64>(kMaxLayers) * sizeof(GpuLayer);
  const u64 brick_primitive_size = static_cast<u64>(kMaxBricks) * sizeof(i32);

  primitive_buffer_.emplace(
      *context_, primitive_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  primitive_colour_buffer_.emplace(
      *context_, primitive_colour_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  layer_buffer_.emplace(
      *context_, layer_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  brick_primitive_buffer_.emplace(*context_, brick_primitive_size,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  const u64 light_buffer_size = static_cast<u64>(kMaxLights) * sizeof(GpuLight);
  light_buffer_.emplace(
      *context_, light_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // GI probe grid, double-buffered -- see bake_probes()/probe_buffer_a_'s
  // header comment for the ping-pong scheme. Device-local and
  // TRANSFER_DST: probe_buffer_a_ is zeroed via vkCmdFillBuffer at the
  // start of every bake_probes() call (the "previous bounce" input to
  // bounce 0, which must start at zero -- see the shader's file header
  // comment), the same idiom brick_counter_buffer_ already uses.
  const u64 probe_buffer_size = static_cast<u64>(kProbeCount) * sizeof(f32) * 4;
  probe_buffer_a_.emplace(*context_, probe_buffer_size,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  probe_buffer_b_.emplace(*context_, probe_buffer_size,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  // 4 entries per primitive (params.x/y/z/extra_param) -- see GpuParamExpr.
  const u64 param_expr_buffer_size =
      static_cast<u64>(kMaxScenePrimitives) * 4 * sizeof(GpuParamExpr);
  param_expr_buffer_.emplace(
      *context_, param_expr_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // One float (0.0/1.0) per primitive -- see Material::pixelation_exempt.
  const u64 pixelation_exempt_buffer_size =
      static_cast<u64>(kMaxScenePrimitives) * sizeof(f32);
  pixelation_exempt_buffer_.emplace(
      *context_, pixelation_exempt_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // One vec4 (xyz=texture_offset, w=texture_rotation) per primitive -- see
  // Material::texture_offset/texture_rotation.
  const u64 tex_transform_buffer_size =
      static_cast<u64>(kMaxScenePrimitives) * sizeof(f32) * 4;
  tex_transform_buffer_.emplace(
      *context_, tex_transform_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  if (!indirection_buffer_->is_valid() || !brick_pool_buffer_->is_valid() ||
      !brick_counter_buffer_->is_valid() || !primitive_buffer_->is_valid() ||
      !primitive_colour_buffer_->is_valid() || !layer_buffer_->is_valid() ||
      !brick_primitive_buffer_->is_valid() || !light_buffer_->is_valid() ||
      !param_expr_buffer_->is_valid() || !probe_buffer_a_->is_valid() ||
      !probe_buffer_b_->is_valid() || !pixelation_exempt_buffer_->is_valid() ||
      !tex_transform_buffer_->is_valid()) {
    KERROR("Failed to create sparse voxel field buffers.");
    return;
  }

  // Descriptor set layout for pass 1 (voxelize): writes indirection, brick
  // pool, and the allocation counter; reads the registered static
  // primitives, layers, and parametric-attribute expressions, and writes
  // which primitive wins at each allocated brick.
  VkDescriptorSetLayoutBinding voxelize_bindings[7]{};
  for (u32 i = 0; i < 7; ++i) {
    voxelize_bindings[i].binding = i;
    voxelize_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    voxelize_bindings[i].descriptorCount = 1;
    voxelize_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }

  VkDescriptorSetLayoutCreateInfo voxelize_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  voxelize_layout_info.bindingCount = 7;
  voxelize_layout_info.pBindings = voxelize_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &voxelize_layout_info,
                                       context_->allocator,
                                       &voxelize_set_layout_));

  // Chunked field (Phase 3a) -- its own descriptor set layouts/pool/sets,
  // entirely separate from every layout/pool below (see chunk_
  // descriptor_pool_'s own comment). chunk_voxelize_set_: {0=chunk_
  // indirection, 1=chunk_brick_pool, 2=chunk_brick_demand, 3=brick_free_
  // list, 4=brick_free_list_top, 5=primitive_buffer (shared, read-only --
  // see voxelize_chunk()'s own comment), 6=chunk_brick_primitive,
  // 7=layer_buffer (shared), 8=param_expr_buffer (shared), 9=chunk_
  // voxelize_batch_slot, 10=chunk_voxelize_batch_data (Phase 6 -- see
  // their own comment), 11=chunk_cluster_point, 12=chunk_cluster,
  // 13=chunk_cluster_free_list, 14=chunk_cluster_free_top,
  // 15=chunk_brick_cluster (the splat point clusters this pass allocates and
  // fills -- see their own comment), 16=chunk_candidate, 17=chunk_candidate_
  // range, 18=chunk_candidate_offset (the per-chunk primitive lists and
  // their resolved repetition instances -- see their own comment),
  // 19=chunk_cell_alias} -- must match Builtin.ChunkVoxelize.comp.glsl's
  // bindings exactly.
  VkDescriptorSetLayoutBinding chunk_voxelize_bindings[20]{};
  for (u32 i = 0; i < 20; ++i) {
    chunk_voxelize_bindings[i].binding = i;
    chunk_voxelize_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_voxelize_bindings[i].descriptorCount = 1;
    chunk_voxelize_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_voxelize_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_voxelize_layout_info.bindingCount = 20;
  chunk_voxelize_layout_info.pBindings = chunk_voxelize_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_voxelize_layout_info,
                                       context_->allocator,
                                       &chunk_voxelize_set_layout_));

  // chunk_debug_query_set_: {0=chunk_table, 1=chunk_indirection,
  // 2=chunk_brick_pool, 3=chunk_brick_primitive, 4=debug_output} -- must
  // match Builtin.ChunkedFieldDebugQuery.comp.glsl's bindings exactly.
  VkDescriptorSetLayoutBinding chunk_debug_query_bindings[5]{};
  for (u32 i = 0; i < 5; ++i) {
    chunk_debug_query_bindings[i].binding = i;
    chunk_debug_query_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_debug_query_bindings[i].descriptorCount = 1;
    chunk_debug_query_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_debug_query_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_debug_query_layout_info.bindingCount = 5;
  chunk_debug_query_layout_info.pBindings = chunk_debug_query_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_debug_query_layout_info,
                                       context_->allocator,
                                       &chunk_debug_query_set_layout_));

  // chunk_evict_set_: {0=chunk_indirection, 1=brick_free_list,
  // 2=brick_free_list_top, 3=chunk_evict_batch (Phase 6 -- see its own
  // comment), 4=chunk_brick_cluster, 5=chunk_cluster, 6=chunk_cluster_free_
  // list, 7=chunk_cluster_free_top (returning a freed brick's splat
  // priming)} -- must match Builtin.ChunkEvict.comp.glsl's bindings
  // exactly.
  VkDescriptorSetLayoutBinding chunk_evict_bindings[8]{};
  for (u32 i = 0; i < 8; ++i) {
    chunk_evict_bindings[i].binding = i;
    chunk_evict_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_evict_bindings[i].descriptorCount = 1;
    chunk_evict_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_evict_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_evict_layout_info.bindingCount = 8;
  chunk_evict_layout_info.pBindings = chunk_evict_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_evict_layout_info,
                                       context_->allocator,
                                       &chunk_evict_set_layout_));

  // chunk_probe_bake_set_(_odd_): {0=chunk_table, 1=chunk_indirection,
  // 2=chunk_brick_pool, 3=chunk_brick_primitive, 4=primitive_colour_buffer
  // (shared, read-only), 5=light_buffer (shared, read-only), 6=Prev
  // ProbeBuffer, 7=CurrProbeBuffer} -- must match Builtin.ChunkProbeBake.
  // comp.glsl's bindings exactly. Same ping-pong-pair-of-fixed-sets scheme
  // as probe_bake_set_/probe_bake_set_odd_ (see their own comment).
  VkDescriptorSetLayoutBinding chunk_probe_bake_bindings[8]{};
  for (u32 i = 0; i < 8; ++i) {
    chunk_probe_bake_bindings[i].binding = i;
    chunk_probe_bake_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_probe_bake_bindings[i].descriptorCount = 1;
    chunk_probe_bake_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_probe_bake_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_probe_bake_layout_info.bindingCount = 8;
  chunk_probe_bake_layout_info.pBindings = chunk_probe_bake_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_probe_bake_layout_info,
                                       context_->allocator,
                                       &chunk_probe_bake_set_layout_));

  // chunk_point_splat_set_: {0=chunk_cluster_point, 1=chunk_cluster,
  // 2=splat_visibility, 3=splat_tile_bound, 4=visible_clusters,
  // 5=cull_args} -- must match Builtin.
  // ChunkPointSplat.comp.glsl's bindings exactly. Binding 3 is re-pointed
  // by rebind_render_target_descriptors() whenever splat_visibility_buffer_ is
  // recreated (resize/render-scale change).
  VkDescriptorSetLayoutBinding chunk_point_splat_bindings[6]{};
  for (u32 i = 0; i < 6; ++i) {
    chunk_point_splat_bindings[i].binding = i;
    chunk_point_splat_bindings[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_point_splat_bindings[i].descriptorCount = 1;
    chunk_point_splat_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_point_splat_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_point_splat_layout_info.bindingCount = 6;
  chunk_point_splat_layout_info.pBindings = chunk_point_splat_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_point_splat_layout_info,
                                       context_->allocator,
                                       &chunk_point_splat_set_layout_));

  // chunk_cluster_cull_set_: {0=chunk_cluster, 1=visible_clusters,
  // 2=cull_args, 3=light_buffer, 4=shadow_pairs, 5=shadow_args,
  // 6=resident_clusters, 7=resident_args, 8=chunk_slot_published} -- must
  // match Builtin.ChunkClusterCull.comp.glsl.
  VkDescriptorSetLayoutBinding chunk_cluster_cull_bindings[9]{};
  for (u32 i = 0; i < 9; ++i) {
    chunk_cluster_cull_bindings[i].binding = i;
    chunk_cluster_cull_bindings[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_cluster_cull_bindings[i].descriptorCount = 1;
    chunk_cluster_cull_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_cluster_cull_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_cluster_cull_layout_info.bindingCount = 9;
  chunk_cluster_cull_layout_info.pBindings = chunk_cluster_cull_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_cluster_cull_layout_info,
                                       context_->allocator,
                                       &chunk_cluster_cull_set_layout_));

  // chunk_shadow_splat_set_ -- see the writes below for the binding list.
  VkDescriptorSetLayoutBinding chunk_shadow_splat_bindings[7]{};
  for (u32 i = 0; i < 7; ++i) {
    chunk_shadow_splat_bindings[i].binding = i;
    chunk_shadow_splat_bindings[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_shadow_splat_bindings[i].descriptorCount = 1;
    chunk_shadow_splat_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_shadow_splat_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_shadow_splat_layout_info.bindingCount = 7;
  chunk_shadow_splat_layout_info.pBindings = chunk_shadow_splat_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_shadow_splat_layout_info,
                                       context_->allocator,
                                       &chunk_shadow_splat_set_layout_));

  // chunk_voxel_cascade_set_ -- see its writes below.
  VkDescriptorSetLayoutBinding chunk_voxel_cascade_bindings[5]{};
  for (u32 i = 0; i < 5; ++i) {
    chunk_voxel_cascade_bindings[i].binding = i;
    chunk_voxel_cascade_bindings[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_voxel_cascade_bindings[i].descriptorCount = 1;
    chunk_voxel_cascade_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_voxel_cascade_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_voxel_cascade_layout_info.bindingCount = 5;
  chunk_voxel_cascade_layout_info.pBindings = chunk_voxel_cascade_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_voxel_cascade_layout_info,
                                       context_->allocator,
                                       &chunk_voxel_cascade_set_layout_));

  VkDescriptorPoolSize chunk_pool_size{};
  chunk_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  // chunk_voxelize_set_ + chunk_debug_query_set_ + chunk_evict_set_ +
  // chunk_probe_bake_set_/_odd_ (x2, like probe_bake_set_/_odd_ above) +
  // chunk_point_splat_set_.
  chunk_pool_size.descriptorCount = 20 + 5 + 8 + 8 * 2 + 6 + 9 + 7 + 5;
  VkDescriptorPoolCreateInfo chunk_pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  chunk_pool_info.poolSizeCount = 1;
  chunk_pool_info.pPoolSizes = &chunk_pool_size;
  chunk_pool_info.maxSets = 9;
  VK_CHECK(vkCreateDescriptorPool(context_->device.logical_device,
                                  &chunk_pool_info, context_->allocator,
                                  &chunk_descriptor_pool_));

  VkDescriptorSetLayout chunk_layouts[9] = {
      chunk_voxelize_set_layout_,      chunk_debug_query_set_layout_,
      chunk_evict_set_layout_,         chunk_probe_bake_set_layout_,
      chunk_probe_bake_set_layout_,    chunk_point_splat_set_layout_,
      chunk_cluster_cull_set_layout_,  chunk_shadow_splat_set_layout_,
      chunk_voxel_cascade_set_layout_};
  VkDescriptorSet chunk_sets[9];
  VkDescriptorSetAllocateInfo chunk_alloc_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  chunk_alloc_info.descriptorPool = chunk_descriptor_pool_;
  chunk_alloc_info.descriptorSetCount = 9;
  chunk_alloc_info.pSetLayouts = chunk_layouts;
  VK_CHECK(vkAllocateDescriptorSets(context_->device.logical_device,
                                    &chunk_alloc_info, chunk_sets));
  chunk_voxelize_set_ = chunk_sets[0];
  chunk_debug_query_set_ = chunk_sets[1];
  chunk_evict_set_ = chunk_sets[2];
  chunk_probe_bake_set_ = chunk_sets[3];
  chunk_probe_bake_set_odd_ = chunk_sets[4];
  chunk_point_splat_set_ = chunk_sets[5];
  chunk_cluster_cull_set_ = chunk_sets[6];
  chunk_shadow_splat_set_ = chunk_sets[7];
  chunk_voxel_cascade_set_ = chunk_sets[8];

  VkDescriptorBufferInfo chunk_voxelize_buffer_infos[20] = {
      {chunk_indirection_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_pool_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_demand_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_free_list_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_free_list_top_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {layer_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {param_expr_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_voxelize_batch_slot_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_voxelize_batch_data_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_point_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_free_list_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_free_top_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_candidate_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_candidate_range_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_candidate_offset_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cell_alias_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_voxelize_writes[20]{};
  for (u32 i = 0; i < 20; ++i) {
    chunk_voxelize_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_voxelize_writes[i].dstSet = chunk_voxelize_set_;
    chunk_voxelize_writes[i].dstBinding = i;
    chunk_voxelize_writes[i].descriptorCount = 1;
    chunk_voxelize_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_voxelize_writes[i].pBufferInfo = &chunk_voxelize_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 20,
                        chunk_voxelize_writes, 0, nullptr);

  VkDescriptorBufferInfo chunk_debug_query_buffer_infos[5] = {
      {chunk_table_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_indirection_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_pool_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_debug_query_output_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_debug_query_writes[5]{};
  for (u32 i = 0; i < 5; ++i) {
    chunk_debug_query_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_debug_query_writes[i].dstSet = chunk_debug_query_set_;
    chunk_debug_query_writes[i].dstBinding = i;
    chunk_debug_query_writes[i].descriptorCount = 1;
    chunk_debug_query_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_debug_query_writes[i].pBufferInfo = &chunk_debug_query_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 5,
                        chunk_debug_query_writes, 0, nullptr);

  VkDescriptorBufferInfo chunk_evict_buffer_infos[8] = {
      {chunk_indirection_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_free_list_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_free_list_top_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_evict_batch_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_free_list_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_free_top_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_evict_writes[8]{};
  for (u32 i = 0; i < 8; ++i) {
    chunk_evict_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_evict_writes[i].dstSet = chunk_evict_set_;
    chunk_evict_writes[i].dstBinding = i;
    chunk_evict_writes[i].descriptorCount = 1;
    chunk_evict_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_evict_writes[i].pBufferInfo = &chunk_evict_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 8, chunk_evict_writes,
                        0, nullptr);

  // chunk_point_splat_set_ -- see its layout comment above. Binding 3
  // (splat_visibility_buffer_) gets an initial write here and is re-pointed by
  // rebind_render_target_descriptors() on every recreate.
  VkDescriptorBufferInfo chunk_point_splat_buffer_infos[6] = {
      {chunk_cluster_point_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {splat_visibility_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {splat_tile_bound_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_visible_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cull_args_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_point_splat_writes[6]{};
  for (u32 i = 0; i < 6; ++i) {
    chunk_point_splat_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_point_splat_writes[i].dstSet = chunk_point_splat_set_;
    chunk_point_splat_writes[i].dstBinding = i;
    chunk_point_splat_writes[i].descriptorCount = 1;
    chunk_point_splat_writes[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_point_splat_writes[i].pBufferInfo = &chunk_point_splat_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 6,
                        chunk_point_splat_writes, 0, nullptr);

  VkDescriptorBufferInfo chunk_cluster_cull_buffer_infos[9] = {
      {chunk_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_visible_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cull_args_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {light_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_shadow_pair_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_shadow_args_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_resident_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_resident_args_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_slot_published_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_cluster_cull_writes[9]{};
  for (u32 i = 0; i < 9; ++i) {
    chunk_cluster_cull_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_cluster_cull_writes[i].dstSet = chunk_cluster_cull_set_;
    chunk_cluster_cull_writes[i].dstBinding = i;
    chunk_cluster_cull_writes[i].descriptorCount = 1;
    chunk_cluster_cull_writes[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_cluster_cull_writes[i].pBufferInfo =
        &chunk_cluster_cull_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 9,
                        chunk_cluster_cull_writes, 0, nullptr);

  // chunk_voxel_cascade_set_: {0=chunk_cluster_point, 1=chunk_cluster,
  // 2=resident_clusters, 3=resident_args, 4=voxel_cascades} -- must match
  // Builtin.ChunkVoxelCascade.comp.glsl.
  VkDescriptorBufferInfo chunk_voxel_cascade_buffer_infos[5] = {
      {chunk_cluster_point_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_resident_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_resident_args_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {voxel_cascade_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_voxel_cascade_writes[5]{};
  for (u32 i = 0; i < 5; ++i) {
    chunk_voxel_cascade_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_voxel_cascade_writes[i].dstSet = chunk_voxel_cascade_set_;
    chunk_voxel_cascade_writes[i].dstBinding = i;
    chunk_voxel_cascade_writes[i].descriptorCount = 1;
    chunk_voxel_cascade_writes[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_voxel_cascade_writes[i].pBufferInfo =
        &chunk_voxel_cascade_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 5,
                        chunk_voxel_cascade_writes, 0, nullptr);

  // chunk_shadow_splat_set_: {0=chunk_cluster_point, 1=chunk_cluster,
  // 2=light_buffer, 3=shadow_atlas, 4=shadow_pairs, 5=shadow_args,
  // 6=chunk_brick_primitive} -- must match Builtin.ChunkShadowSplat.comp.glsl.
  VkDescriptorBufferInfo chunk_shadow_splat_buffer_infos[7] = {
      {chunk_cluster_point_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {light_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {shadow_atlas_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_shadow_pair_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_shadow_args_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_shadow_splat_writes[7]{};
  for (u32 i = 0; i < 7; ++i) {
    chunk_shadow_splat_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_shadow_splat_writes[i].dstSet = chunk_shadow_splat_set_;
    chunk_shadow_splat_writes[i].dstBinding = i;
    chunk_shadow_splat_writes[i].descriptorCount = 1;
    chunk_shadow_splat_writes[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_shadow_splat_writes[i].pBufferInfo =
        &chunk_shadow_splat_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 7,
                        chunk_shadow_splat_writes, 0, nullptr);

  // chunk_probe_bake_set_ is bounce parity "write_to_b" (Prev=a_/Curr=b_);
  // chunk_probe_bake_set_odd_ is the reverse -- same scheme as
  // write_probe_bake_set() below, just for the chunked field's own probe
  // buffers/bindings.
  struct ChunkProbeBakeBufferBinding {
    u32 binding;
    VkBuffer buffer;
  };
  auto write_chunk_probe_bake_set = [&](VkDescriptorSet set, VkBuffer prev_buffer,
                                        VkBuffer curr_buffer) {
    ChunkProbeBakeBufferBinding bindings[8] = {
        {0, chunk_table_buffer_->handle()},
        {1, chunk_indirection_buffer_->handle()},
        {2, chunk_brick_pool_buffer_->handle()},
        {3, chunk_brick_primitive_buffer_->handle()},
        {4, primitive_colour_buffer_->handle()},
        {5, light_buffer_->handle()},
        {6, prev_buffer},
        {7, curr_buffer},
    };
    VkDescriptorBufferInfo buffer_infos[8];
    VkWriteDescriptorSet writes[8]{};
    for (u32 i = 0; i < 8; ++i) {
      buffer_infos[i] = {bindings[i].buffer, 0, VK_WHOLE_SIZE};
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = bindings[i].binding;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffer_infos[i];
    }
    vkUpdateDescriptorSets(context_->device.logical_device, 8, writes, 0,
                          nullptr);
  };
  write_chunk_probe_bake_set(chunk_probe_bake_set_, chunk_gi_probe_buffer_a_->handle(),
                             chunk_gi_probe_buffer_b_->handle());
  write_chunk_probe_bake_set(chunk_probe_bake_set_odd_,
                             chunk_gi_probe_buffer_b_->handle(),
                             chunk_gi_probe_buffer_a_->handle());

  // Descriptor set layout for pass 2 (render): the output image, read-only
  // access to indirection/brick_pool/brick_primitive, the static
  // primitives' diffuse tints, their textures (a small fixed-size array --
  // see scene_textures in the shader for why), and the registered lights.
  // The camera state that used to be a UBO binding here is now a push
  // constant instead (see PushConstants above) — no separate binding
  // needed, and no per-swapchain-image duplication either. Binding 3 used
  // to be an intentional gap (previously the now-removed dynamic
  // primitive's own texture) -- now filled by light_buffer_ instead of
  // staying unused.
  // 0 = out_image, 1 = indirection, 2 = brick pool, 3 = lights, 4 = brick
  // materials, 5 = primitive colours (+texture scale in .a), 6 = the
  // texture array, 7/8/9 = primitives/layers/param exprs -- the same
  // analytic-scene buffers the voxelize set binds at 3/5/6, re-bound here
  // for the render pass's per-pixel material re-evaluation (see the
  // Builtin.SdfSceneCommon.inc.glsl include in
  // Builtin.RaymarchShader.comp.glsl) -- 10 = the baked GI probe grid
  // (see ProbeBuffer in the same shader), read-only here just like every
  // other baked-field binding -- 11 = per-primitive pixelation-exempt
  // flags (see PixelationExemptBuffer in the same shader), written into
  // out_image's alpha channel for the post-process pass to read -- and
  // 12 = the optional skybox texture (see set_skybox()), a single combined
  // image sampler rather than a fixed-size array like binding 6 since
  // there's only ever one at a time -- and 13 = each primitive's effective
  // texture offset/rotation (see tex_transform_buffer_'s comment), read by
  // the same triplanar sampling that reads binding 5's texture_scale -- and
  // 14 = each primitive's bump map texture (see Material::bump_texture),
  // a second fixed-size array exactly mirroring binding 6's scene_textures
  // but sampled purely for luminance by sample_scene_heights(), never for
  // colour.
  // 15/16/17/18 (Phase 4): read-only access to the chunked/streamed field
  // (chunk_table_buffer_/chunk_indirection_buffer_/chunk_brick_pool_
  // buffer_/chunk_brick_primitive_buffer_) -- always bound (Vulkan
  // requires every declared binding point at a valid buffer regardless of
  // whether the shader's current push-constant-driven branch actually
  // reads it -- same reasoning as binding 6/14's always-bound-but-maybe-
  // unused texture slots), but Builtin.RaymarchShader.comp.glsl only
  // actually samples through these when chunked_field_enabled_ is true
  // (see set_chunked_field_enabled()) -- off by default, so nothing
  // currently working changes unless a caller explicitly opts in. 19 =
  // Phase 5's chunked-field GI cascade (see sample_gi_cascade()), same
  // always-bound-but-conditionally-sampled reasoning. 20/21/22 = the splat
  // visibility buffer and the point pool/meta needed to decode a winning
  // splat back into a world position (see splat_visibility_buffer_'s own
  // comment and read_splat() in the shader), only read while
  // push.splat_mode is nonzero -- same always-bound reasoning again. 23 =
  // the splat pass's per-tile near bound, which decides whether a splat
  // can be trusted at all (see splat_tile_bound_buffer_'s own comment).
  VkDescriptorSetLayoutBinding render_bindings[28]{};
  for (u32 i = 0; i < 28; ++i) {
    render_bindings[i].binding = i;
    render_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    render_bindings[i].descriptorCount = 1;
    render_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  render_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  render_bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  render_bindings[6].descriptorCount = kMaxScenePrimitives;
  render_bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  render_bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  render_bindings[14].descriptorCount = kMaxScenePrimitives;
  // 24 = depth_image_, the per-pixel hit distance TAA reprojects with (and
  // the AO pass traces against) -- a storage image like binding 0, not a
  // buffer.
  render_bindings[24].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  // 25 = the G-buffer's normal/material half (see normal_material_image_).
  render_bindings[25].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  // 27 = the ambient-occlusion estimate the shading pass multiplies its
  // indirect term by (26 is the shadow atlas, a buffer).
  render_bindings[27].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

  VkDescriptorSetLayoutCreateInfo render_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  render_layout_info.bindingCount = 28;
  render_layout_info.pBindings = render_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &render_layout_info, context_->allocator,
                                       &render_set_layout_));

  // Descriptor set layout for pass 2 (probe bake): read-only access to the
  // baked field (indirection/brick_pool/brick_primitive) and the static
  // scene's diffuse colours/lights (for a gather ray's hit shading), plus
  // one read and one write binding for the previous/current bounce's
  // probe grid -- see bake_probes(), which rewrites bindings 5/6 to
  // alternate probe_buffer_a_/probe_buffer_b_ before every bounce's
  // dispatch (everything else here is static, set once below).
  VkDescriptorSetLayoutBinding probe_bake_bindings[7]{};
  for (u32 i = 0; i < 7; ++i) {
    probe_bake_bindings[i].binding = i;
    probe_bake_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    probe_bake_bindings[i].descriptorCount = 1;
    probe_bake_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }

  VkDescriptorSetLayoutCreateInfo probe_bake_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  probe_bake_layout_info.bindingCount = 7;
  probe_bake_layout_info.pBindings = probe_bake_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &probe_bake_layout_info,
                                       context_->allocator,
                                       &probe_bake_set_layout_));

  // Descriptor set layout for pass 4a (bloom blur, horizontal): reads
  // output_image_, writes bloom_temp_image_ -- see Builtin.BloomBlurH.
  // comp.glsl.
  VkDescriptorSetLayoutBinding bloom_blur_h_bindings[2]{};
  bloom_blur_h_bindings[0].binding = 0;
  bloom_blur_h_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bloom_blur_h_bindings[0].descriptorCount = 1;
  bloom_blur_h_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bloom_blur_h_bindings[1] = bloom_blur_h_bindings[0];
  bloom_blur_h_bindings[1].binding = 1;

  VkDescriptorSetLayoutCreateInfo bloom_blur_h_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  bloom_blur_h_layout_info.bindingCount = 2;
  bloom_blur_h_layout_info.pBindings = bloom_blur_h_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &bloom_blur_h_layout_info,
                                       context_->allocator,
                                       &bloom_blur_h_set_layout_));

  // Descriptor set layout for pass 4b (post-composite): reads
  // output_image_ and bloom_temp_image_, writes post_process_image_ -- see
  // Builtin.PostComposite.comp.glsl.
  VkDescriptorSetLayoutBinding post_composite_bindings[3]{};
  for (u32 i = 0; i < 3; ++i) {
    post_composite_bindings[i].binding = i;
    post_composite_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    post_composite_bindings[i].descriptorCount = 1;
    post_composite_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }

  VkDescriptorSetLayoutCreateInfo post_composite_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  post_composite_layout_info.bindingCount = 3;
  post_composite_layout_info.pBindings = post_composite_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &post_composite_layout_info,
                                       context_->allocator,
                                       &post_composite_set_layout_));

  // Descriptor set layout for the TAA resolve pass: 0 = this frame's
  // colour, 1 = its depth, 2 = history in, 3 = history out, 4 = resolved
  // output -- see Builtin.TaaResolve.comp.glsl. Two sets are allocated from
  // it, one per ping-pong parity.
  VkDescriptorSetLayoutBinding taa_resolve_bindings[5]{};
  for (u32 i = 0; i < 5; ++i) {
    taa_resolve_bindings[i].binding = i;
    taa_resolve_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    taa_resolve_bindings[i].descriptorCount = 1;
    taa_resolve_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo taa_resolve_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  taa_resolve_layout_info.bindingCount = 5;
  taa_resolve_layout_info.pBindings = taa_resolve_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &taa_resolve_layout_info,
                                       context_->allocator,
                                       &taa_resolve_set_layout_));

  // stochastic_ao_set_: {0=gbuffer_depth, 1=gbuffer_normal_material,
  // 2=ao_image (write), 3=voxel_cascades} -- must match Builtin.
  // StochasticAo.comp.glsl. The three images are render targets, so this set
  // is re-pointed by rebind_render_target_descriptors() like every other set
  // that references one.
  VkDescriptorSetLayoutBinding stochastic_ao_bindings[4]{};
  for (u32 i = 0; i < 4; ++i) {
    stochastic_ao_bindings[i].binding = i;
    stochastic_ao_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    stochastic_ao_bindings[i].descriptorCount = 1;
    stochastic_ao_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  stochastic_ao_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  VkDescriptorSetLayoutCreateInfo stochastic_ao_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  stochastic_ao_layout_info.bindingCount = 4;
  stochastic_ao_layout_info.pBindings = stochastic_ao_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &stochastic_ao_layout_info,
                                       context_->allocator,
                                       &stochastic_ao_set_layout_));

  // One pool backing all nine sets.
  VkDescriptorPoolSize pool_sizes[3]{};
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  // voxelize's 7 + render's 20 (11 + Phase 4's 4 chunked-field bindings +
  // Phase 5's 1 GI cascade probe binding + splatting's 4: visibility
  // buffer, point pool, point meta, per-tile near bound) + probe bake's
  // 7 x2 (probe_bake_set_ and probe_bake_set_odd_ -- see their
  // declaration comment).
  // ...plus the AO pass's one storage buffer (the voxel cascades).
  pool_sizes[0].descriptorCount = 43;
  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  // render's 2 (output + depth) + bloom blur h's 2 + post composite's 3 +
  // TAA resolve's 5 x2 sets.
  // render's 3 (colour + the G-buffer's two halves) + bloom blur h's 2 +
  // post composite's 3 + TAA resolve's 5 x2 sets.
  // render's 4 (colour, the G-buffer's two halves, the AO estimate) + bloom
  // blur h's 2 + post composite's 3 + TAA resolve's 5 x2 sets + the AO
  // pass's own 3 images.
  pool_sizes[1].descriptorCount = 22;
  pool_sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  // scene_textures + scene_bump_textures + skybox_texture.
  pool_sizes[2].descriptorCount = kMaxScenePrimitives * 2 + 1;

  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = pool_sizes;
  pool_info.maxSets = 9;
  VK_CHECK(vkCreateDescriptorPool(context_->device.logical_device, &pool_info,
                                  context_->allocator, &descriptor_pool_));

  VkDescriptorSetLayout layouts[9] = {
      voxelize_set_layout_,       render_set_layout_,
      probe_bake_set_layout_,     bloom_blur_h_set_layout_,
      post_composite_set_layout_, probe_bake_set_layout_,
      taa_resolve_set_layout_,    taa_resolve_set_layout_,
      stochastic_ao_set_layout_};
  VkDescriptorSet sets[9];
  VkDescriptorSetAllocateInfo alloc_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc_info.descriptorPool = descriptor_pool_;
  alloc_info.descriptorSetCount = 9;
  alloc_info.pSetLayouts = layouts;
  VK_CHECK(vkAllocateDescriptorSets(context_->device.logical_device,
                                    &alloc_info, sets));
  voxelize_set_ = sets[0];
  render_set_ = sets[1];
  probe_bake_set_ = sets[2];
  bloom_blur_h_set_ = sets[3];
  post_composite_set_ = sets[4];
  probe_bake_set_odd_ = sets[5];
  taa_resolve_sets_[0] = sets[6];
  taa_resolve_sets_[1] = sets[7];
  stochastic_ao_set_ = sets[8];

  // Populate voxelize_set_: {indirection, brick_pool, brick_counter,
  // primitive_buffer, brick_primitive_buffer, layer_buffer, param_expr_buffer}.
  VkDescriptorBufferInfo voxelize_buffer_infos[7] = {
      {indirection_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {brick_pool_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {brick_counter_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {brick_primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {layer_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {param_expr_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };

  VkWriteDescriptorSet voxelize_writes[7]{};
  for (u32 i = 0; i < 7; ++i) {
    voxelize_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    voxelize_writes[i].dstSet = voxelize_set_;
    voxelize_writes[i].dstBinding = i;
    voxelize_writes[i].descriptorCount = 1;
    voxelize_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    voxelize_writes[i].pBufferInfo = &voxelize_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 7, voxelize_writes,
                        0, nullptr);

  // Populate render_set_'s bindings that don't depend on the registered
  // static scene: {out_image, indirection, brick_pool, light_buffer,
  // brick_primitive_buffer, primitive_colour_buffer}. Binding 6
  // (scene_textures) is written separately by rebuild_static_scene()
  // below, since it depends on whichever textures are actually registered.
  VkDescriptorImageInfo render_image_info{};
  render_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  render_image_info.imageView = output_image_.view;

  // Buffer bindings, in binding order (binding 0 is the image, 6 the
  // texture array, 12 the skybox texture -- all written separately below/
  // by rebuild_static_scene()/write_skybox_binding()): 1..5 as before,
  // 7/8/9 = the analytic-scene buffers the per-pixel material
  // re-evaluation reads, 10 = whichever probe buffer holds the final
  // bounce's result (see kProbeFinalInBufferB) -- fixed at construction,
  // since which physical buffer is "final" never changes once
  // kProbeBounceCount is compiled in -- 11 = per-primitive
  // pixelation-exempt flags, and 13 = per-primitive texture offset/
  // rotation.
  struct RenderBufferBinding {
    u32 binding;
    VkBuffer buffer;
  } render_buffer_bindings[21] = {
      {1, indirection_buffer_->handle()},
      {2, brick_pool_buffer_->handle()},
      {3, light_buffer_->handle()},
      {4, brick_primitive_buffer_->handle()},
      {5, primitive_colour_buffer_->handle()},
      {7, primitive_buffer_->handle()},
      {8, layer_buffer_->handle()},
      {9, param_expr_buffer_->handle()},
      {10, kProbeFinalInBufferB ? probe_buffer_b_->handle()
                               : probe_buffer_a_->handle()},
      {11, pixelation_exempt_buffer_->handle()},
      {13, tex_transform_buffer_->handle()},
      {15, chunk_table_buffer_->handle()},
      {16, chunk_indirection_buffer_->handle()},
      {17, chunk_brick_pool_buffer_->handle()},
      {18, chunk_brick_primitive_buffer_->handle()},
      // Same kProbeFinalInBufferB parity logic as binding 10 above, applied
      // to the chunked field's own GI cascade buffers (Phase 5).
      {19, kProbeFinalInBufferB ? chunk_gi_probe_buffer_b_->handle()
                                : chunk_gi_probe_buffer_a_->handle()},
      // The splat visibility buffer -- re-pointed by
      // rebind_render_target_descriptors() whenever it's recreated -- plus
      // the point pool/meta the render pass decodes a winning splat with
      // (the same two buffers chunk_point_splat_set_ binds at 0/1).
      {20, splat_visibility_buffer_->handle()},
      {21, chunk_cluster_point_buffer_->handle()},
      {22, chunk_cluster_buffer_->handle()},
      {23, splat_tile_bound_buffer_->handle()},
      // 26 = the imperfect shadow maps the shading pass samples (24/25 are
      // the G-buffer images, written by rebind_render_target_descriptors()).
      {26, shadow_atlas_buffer_->handle()},
  };

  VkDescriptorBufferInfo render_buffer_infos[21];
  VkWriteDescriptorSet render_writes[22]{};
  render_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  render_writes[0].dstSet = render_set_;
  render_writes[0].dstBinding = 0;
  render_writes[0].descriptorCount = 1;
  render_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  render_writes[0].pImageInfo = &render_image_info;

  for (u32 i = 0; i < 21; ++i) {
    render_buffer_infos[i] = {render_buffer_bindings[i].buffer, 0,
                              VK_WHOLE_SIZE};
    render_writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    render_writes[i + 1].dstSet = render_set_;
    render_writes[i + 1].dstBinding = render_buffer_bindings[i].binding;
    render_writes[i + 1].descriptorCount = 1;
    render_writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    render_writes[i + 1].pBufferInfo = &render_buffer_infos[i];
  }

  vkUpdateDescriptorSets(context_->device.logical_device, 22, render_writes,
                        0, nullptr);

  // Binding 12 (skybox_texture) starts pointed at the filler texture --
  // skybox_enabled_ starts false, so Builtin.RaymarchShader.comp.glsl never
  // actually samples it, but Vulkan still requires every combined-image-
  // sampler binding to reference a real image regardless (same reasoning as
  // scene_textures' unused slots -- see rebuild_static_scene()).
  write_skybox_binding(context_->texture_system->default_texture());

  // Populate both probe-bake sets' static bindings (0=indirection,
  // 1=brick_pool, 2=brick_primitive -- the baked field a gather ray
  // marches against, always the current bake's; 3=scene_diffuse_colours,
  // 4=light_buffer -- a gather ray's hit shading; identical in both sets),
  // plus 5/6 (Prev/CurrProbeBuffer), fixed per set instead of rewritten
  // per bounce -- see probe_bake_set_odd_'s declaration comment.
  // probe_bake_set_ is bounce parity "write_to_b" (Prev=a_/Curr=b_);
  // probe_bake_set_odd_ is the reverse.
  struct ProbeBakeBufferBinding {
    u32 binding;
    VkBuffer buffer;
  };
  auto write_probe_bake_set = [&](VkDescriptorSet set, VkBuffer prev_buffer,
                                  VkBuffer curr_buffer) {
    ProbeBakeBufferBinding bindings[7] = {
        {0, indirection_buffer_->handle()},
        {1, brick_pool_buffer_->handle()},
        {2, brick_primitive_buffer_->handle()},
        {3, primitive_colour_buffer_->handle()},
        {4, light_buffer_->handle()},
        {5, prev_buffer},
        {6, curr_buffer},
    };
    VkDescriptorBufferInfo buffer_infos[7];
    VkWriteDescriptorSet writes[7]{};
    for (u32 i = 0; i < 7; ++i) {
      buffer_infos[i] = {bindings[i].buffer, 0, VK_WHOLE_SIZE};
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = bindings[i].binding;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffer_infos[i];
    }
    vkUpdateDescriptorSets(context_->device.logical_device, 7, writes, 0,
                          nullptr);
  };
  write_probe_bake_set(probe_bake_set_, probe_buffer_a_->handle(),
                       probe_buffer_b_->handle());
  write_probe_bake_set(probe_bake_set_odd_, probe_buffer_b_->handle(),
                       probe_buffer_a_->handle());

  // Populate bloom_blur_h_set_: 0 = output_image_ (read), 1 =
  // bloom_temp_image_ (write). Both fixed for the object's lifetime --
  // only on_resized() ever needs to repoint these (the images themselves
  // get recreated then).
  // Binding 0 is taa_output_image_, NOT output_image_: the post-process
  // chain always reads the temporally resolved frame. With TAA off the
  // resolve pass still runs as a straight copy, so this binding never has
  // to change (see set_taa_enabled()).
  VkDescriptorImageInfo bloom_blur_h_image_infos[2] = {
      {VK_NULL_HANDLE, taa_output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, bloom_temp_image_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet bloom_blur_h_writes[2]{};
  for (u32 i = 0; i < 2; ++i) {
    bloom_blur_h_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    bloom_blur_h_writes[i].dstSet = bloom_blur_h_set_;
    bloom_blur_h_writes[i].dstBinding = i;
    bloom_blur_h_writes[i].descriptorCount = 1;
    bloom_blur_h_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bloom_blur_h_writes[i].pImageInfo = &bloom_blur_h_image_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 2, bloom_blur_h_writes,
                        0, nullptr);

  // Populate post_composite_set_: 0 = output_image_ (read), 1 =
  // bloom_temp_image_ (read), 2 = post_process_image_ (write).
  VkDescriptorImageInfo post_composite_image_infos[3] = {
      {VK_NULL_HANDLE, taa_output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, bloom_temp_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, post_process_image_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet post_composite_writes[3]{};
  for (u32 i = 0; i < 3; ++i) {
    post_composite_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    post_composite_writes[i].dstSet = post_composite_set_;
    post_composite_writes[i].dstBinding = i;
    post_composite_writes[i].descriptorCount = 1;
    post_composite_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    post_composite_writes[i].pImageInfo = &post_composite_image_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 3,
                        post_composite_writes, 0, nullptr);

  // Everything that points at a render-target image (render_set_'s 0/24,
  // both TAA parities, bloom/post's inputs) is written by one function, so
  // the initial population and the post-resize repoint can never drift
  // apart -- the sets above only cover bindings that outlive a resize.
  rebind_render_target_descriptors();

  voxelize_pipeline_.emplace(
      *context_, voxelize_stage_,
      std::vector<VkDescriptorSetLayout>{voxelize_set_layout_},
      sizeof(VoxelizePushConstants));
  chunk_voxelize_pipeline_.emplace(
      *context_, chunk_voxelize_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_voxelize_set_layout_},
      sizeof(ChunkVoxelizePushConstants));
  chunk_debug_query_pipeline_.emplace(
      *context_, chunk_debug_query_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_debug_query_set_layout_},
      sizeof(ChunkDebugQueryPushConstants));
  chunk_evict_pipeline_.emplace(
      *context_, chunk_evict_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_evict_set_layout_},
      sizeof(ChunkEvictPushConstants));
  probe_bake_pipeline_.emplace(
      *context_, probe_bake_stage_,
      std::vector<VkDescriptorSetLayout>{probe_bake_set_layout_},
      sizeof(ProbeBakePushConstants));
  chunk_probe_bake_pipeline_.emplace(
      *context_, chunk_probe_bake_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_probe_bake_set_layout_},
      sizeof(ChunkProbeBakePushConstants));
  chunk_cluster_cull_pipeline_.emplace(
      *context_, chunk_cluster_cull_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_cluster_cull_set_layout_},
      sizeof(ChunkClusterCullPushConstants));
  chunk_voxel_cascade_pipeline_.emplace(
      *context_, chunk_voxel_cascade_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_voxel_cascade_set_layout_},
      sizeof(ChunkVoxelCascadePushConstants));
  stochastic_ao_pipeline_.emplace(
      *context_, stochastic_ao_stage_,
      std::vector<VkDescriptorSetLayout>{stochastic_ao_set_layout_},
      sizeof(StochasticAoPushConstants));
  chunk_shadow_splat_pipeline_.emplace(
      *context_, chunk_shadow_splat_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_shadow_splat_set_layout_},
      sizeof(ChunkShadowSplatPushConstants));
  chunk_point_splat_pipeline_.emplace(
      *context_, chunk_point_splat_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_point_splat_set_layout_},
      sizeof(ChunkPointSplatPushConstants));
  render_pipeline_.emplace(*context_, render_stage_,
                           std::vector<VkDescriptorSetLayout>{render_set_layout_},
                           sizeof(PushConstants));
  // Same descriptor set layout and same push constants as the visibility
  // pass -- see deferred_shade_stage_'s comment for why they share.
  deferred_shade_pipeline_.emplace(
      *context_, deferred_shade_stage_,
      std::vector<VkDescriptorSetLayout>{render_set_layout_},
      sizeof(PushConstants));
  bloom_blur_h_pipeline_.emplace(
      *context_, bloom_blur_h_stage_,
      std::vector<VkDescriptorSetLayout>{bloom_blur_h_set_layout_},
      sizeof(BloomBlurHPushConstants));
  post_composite_pipeline_.emplace(
      *context_, post_composite_stage_,
      std::vector<VkDescriptorSetLayout>{post_composite_set_layout_},
      sizeof(PostCompositePushConstants));
  taa_resolve_pipeline_.emplace(
      *context_, taa_resolve_stage_,
      std::vector<VkDescriptorSetLayout>{taa_resolve_set_layout_},
      sizeof(TaaResolvePushConstants));

  if (!voxelize_pipeline_->is_valid() || !chunk_voxelize_pipeline_->is_valid() ||
      !chunk_debug_query_pipeline_->is_valid() || !chunk_evict_pipeline_->is_valid() ||
      !probe_bake_pipeline_->is_valid() || !chunk_probe_bake_pipeline_->is_valid() ||
      !chunk_cluster_cull_pipeline_->is_valid() ||
      !chunk_shadow_splat_pipeline_->is_valid() ||
      !chunk_voxel_cascade_pipeline_->is_valid() ||
      !stochastic_ao_pipeline_->is_valid() ||
      !chunk_point_splat_pipeline_->is_valid() ||
      !render_pipeline_->is_valid() || !deferred_shade_pipeline_->is_valid() ||
      !bloom_blur_h_pipeline_->is_valid() ||
      !post_composite_pipeline_->is_valid() ||
      !taa_resolve_pipeline_->is_valid()) {
    KERROR("Failed to create compute pipeline(s) for the raymarch field.");
    return;
  }

  // One ChunkStreamingManager per clip level, each owning a disjoint
  // slot_offset range of the shared chunk_indirection_buffer_ -- see
  // chunk_streaming_levels_'s own comment for why this can't be a single
  // member-initializer-list construction.
  // One generation stamp per global slot -- see the alias cache.
  slot_content_generation_.assign(
      static_cast<size_t>(kNumLevels) * kMaxResidentChunks, 0);
  chunk_streaming_levels_.reserve(kNumLevels);
  for (u32 level = 0; level < kNumLevels; ++level) {
    // The final argument enables LAZY eviction (see ChunkStreamingManager's
    // constructor comment): with 64 slots against a 27-chunk window, a
    // chunk that leaves the window stays cached until fewer than a quarter
    // of the slots are free, so orbiting or backtracking the camera finds
    // it still resident instead of paying a multi-frame re-bake. A quarter
    // (16) comfortably covers a burst of double-buffered forced rebakes
    // plus new boundary loads before pressure-driven eviction has to kick
    // in.
    chunk_streaming_levels_.emplace_back(kMaxResidentChunks, kStreamRadiusChunks,
                                         kFramesInFlightDelay,
                                         level * kMaxResidentChunks,
                                         kMaxResidentChunks / 4);
  }

  // GPU timing query pools -- see the GraphicsTimestamp enum's comment.
  // timestampPeriod of 0 means the device cannot timestamp at all, and
  // timestampComputeAndGraphics being false would mean the compute queues
  // can't; both simply disable the instrumentation rather than the engine.
  timestamp_period_ns_ = context_->device.properties.limits.timestampPeriod;
  timestamps_supported_ =
      timestamp_period_ns_ > 0.0f &&
      context_->device.properties.limits.timestampComputeAndGraphics == VK_TRUE;
  if (timestamps_supported_) {
    VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_info.queryCount = kGraphicsTimestampCount * kTimestampFrames;
    if (vkCreateQueryPool(context_->device.logical_device, &query_info,
                          context_->allocator,
                          &graphics_timestamp_pool_) != VK_SUCCESS) {
      timestamps_supported_ = false;
    }
    query_info.queryCount = 2 * kAsyncRingDepth;
    if (timestamps_supported_ &&
        vkCreateQueryPool(context_->device.logical_device, &query_info,
                          context_->allocator,
                          &async_timestamp_pool_) != VK_SUCCESS) {
      timestamps_supported_ = false;
    }
  }
  if (!timestamps_supported_) {
    KWARN("GPU timestamps unavailable on this device -- per-pass timing "
         "will not be reported.");
  }

  // async_command_pool_/async_command_buffers_/async_fences_/async_chunk_
  // ready_semaphores_ -- see update_streaming()'s own comment for what
  // this ring is for. On its own queue family now (see VulkanDevice::
  // async_compute_queue_index) rather than sharing the graphics one.
  VkCommandPoolCreateInfo async_pool_info{
      VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  // The async-compute family, which is a DEDICATED compute engine where the
  // device has one -- command pools are per family, so this has to follow
  // whichever queue update_streaming() actually submits to.
  async_pool_info.queueFamilyIndex =
      static_cast<u32>(context_->device.async_compute_queue_index);
  async_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VK_CHECK(vkCreateCommandPool(context_->device.logical_device, &async_pool_info,
                               context_->allocator, &async_command_pool_));

  async_command_buffers_.reserve(kAsyncRingDepth);
  async_fences_.reserve(kAsyncRingDepth);
  async_chain_semaphores_.reserve(kAsyncRingDepth);
  VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (u32 i = 0; i < kAsyncRingDepth; ++i) {
    async_command_buffers_.push_back(std::make_unique<VulkanCommandBuffer>(
        *context_, async_command_pool_, /*is_primary=*/TRUE));
    // Signaled at construction -- the first update_streaming() call to
    // actually need this slot must not wait on a submission that was
    // never made.
    async_fences_.push_back(
        std::make_unique<VulkanFence>(*context_, /*create_signaled=*/TRUE));
    VkSemaphore chain_semaphore;
    VK_CHECK(vkCreateSemaphore(context_->device.logical_device, &semaphore_info,
                               context_->allocator, &chain_semaphore));
    async_chain_semaphores_.push_back(chain_semaphore);
  }

  valid_ = true;

  // Upload the registered static scene and bake it, now that everything
  // above exists.
  rebuild_static_scene();

  // Opt-in bake profiling -- see bake_stats_enabled_. An environment
  // variable rather than a public setter because it is a diagnostic for
  // whoever is optimising the bake, not a runtime feature any caller
  // should be toggling.
  bake_stats_enabled_ = std::getenv("KENGINE_BAKE_STATS") != nullptr;
  if (bake_stats_enabled_) {
    KINFO("Chunk bake statistics enabled (KENGINE_BAKE_STATS).");
  }
  // ON by default now. It was opt-in while it was only a within-chunk
  // trick that had never been run against real content; it is now the
  // mechanism behind both deduplication and the persistent alias cache
  // (see alias_cache_), i.e. the main defence against re-baking geometry
  // this engine has already baked. The escape hatch inverts accordingly.
  chunk_dedup_enabled_ = std::getenv("KENGINE_NO_CHUNK_DEDUP") == nullptr;
  if (!chunk_dedup_enabled_) {
    KINFO("Chunk brick deduplication DISABLED (KENGINE_NO_CHUNK_DEDUP).");
  }

  reset_chunked_field();
}

VulkanRaymarchShader::~VulkanRaymarchShader() {
  // Release the skybox's texture reference (if any) -- every acquire()
  // needs exactly one matching release() (see TextureSystem's contract).
  // Safe to do unconditionally before device/descriptor teardown below:
  // VulkanRendererBackend destroys raymarch_shader before texture_system.
  if (skybox_enabled_) {
    context_->texture_system->release(skybox_texture_name_);
  }

  // Destroy pipelines before the descriptor set layouts they were created
  // with.
  post_composite_pipeline_.reset();
  bloom_blur_h_pipeline_.reset();
  render_pipeline_.reset();
  probe_bake_pipeline_.reset();
  chunk_point_splat_pipeline_.reset();
  chunk_probe_bake_pipeline_.reset();
  chunk_evict_pipeline_.reset();
  chunk_debug_query_pipeline_.reset();
  chunk_voxelize_pipeline_.reset();
  voxelize_pipeline_.reset();

  if (descriptor_pool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(context_->device.logical_device, descriptor_pool_,
                            context_->allocator);
    descriptor_pool_ = VK_NULL_HANDLE;
  }
  if (chunk_descriptor_pool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(context_->device.logical_device,
                            chunk_descriptor_pool_, context_->allocator);
    chunk_descriptor_pool_ = VK_NULL_HANDLE;
  }
  if (chunk_voxel_cascade_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 chunk_voxel_cascade_set_layout_,
                                 context_->allocator);
    chunk_voxel_cascade_set_layout_ = VK_NULL_HANDLE;
  }
  if (stochastic_ao_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 stochastic_ao_set_layout_,
                                 context_->allocator);
    stochastic_ao_set_layout_ = VK_NULL_HANDLE;
  }
  if (chunk_shadow_splat_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 chunk_shadow_splat_set_layout_,
                                 context_->allocator);
    chunk_shadow_splat_set_layout_ = VK_NULL_HANDLE;
  }
  if (chunk_cluster_cull_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 chunk_cluster_cull_set_layout_,
                                 context_->allocator);
    chunk_cluster_cull_set_layout_ = VK_NULL_HANDLE;
  }
  if (chunk_point_splat_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 chunk_point_splat_set_layout_,
                                 context_->allocator);
    chunk_point_splat_set_layout_ = VK_NULL_HANDLE;
  }
  if (chunk_probe_bake_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 chunk_probe_bake_set_layout_,
                                 context_->allocator);
    chunk_probe_bake_set_layout_ = VK_NULL_HANDLE;
  }
  if (chunk_evict_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 chunk_evict_set_layout_, context_->allocator);
    chunk_evict_set_layout_ = VK_NULL_HANDLE;
  }
  if (chunk_debug_query_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 chunk_debug_query_set_layout_,
                                 context_->allocator);
    chunk_debug_query_set_layout_ = VK_NULL_HANDLE;
  }
  if (chunk_voxelize_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 chunk_voxelize_set_layout_,
                                 context_->allocator);
    chunk_voxelize_set_layout_ = VK_NULL_HANDLE;
  }
  if (post_composite_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 post_composite_set_layout_,
                                 context_->allocator);
    post_composite_set_layout_ = VK_NULL_HANDLE;
  }
  if (bloom_blur_h_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 bloom_blur_h_set_layout_, context_->allocator);
    bloom_blur_h_set_layout_ = VK_NULL_HANDLE;
  }
  if (taa_resolve_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 taa_resolve_set_layout_, context_->allocator);
    taa_resolve_set_layout_ = VK_NULL_HANDLE;
  }
  if (render_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 render_set_layout_, context_->allocator);
    render_set_layout_ = VK_NULL_HANDLE;
  }
  if (probe_bake_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 probe_bake_set_layout_, context_->allocator);
    probe_bake_set_layout_ = VK_NULL_HANDLE;
  }
  if (voxelize_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 voxelize_set_layout_, context_->allocator);
    voxelize_set_layout_ = VK_NULL_HANDLE;
  }

  layer_buffer_.reset();
  light_buffer_.reset();
  param_expr_buffer_.reset();
  pixelation_exempt_buffer_.reset();
  tex_transform_buffer_.reset();
  primitive_colour_buffer_.reset();
  brick_primitive_buffer_.reset();
  primitive_buffer_.reset();

  probe_buffer_b_.reset();
  probe_buffer_a_.reset();

  brick_counter_buffer_.reset();
  brick_pool_buffer_.reset();
  indirection_buffer_.reset();

  chunk_gi_probe_buffer_b_.reset();
  chunk_gi_probe_buffer_a_.reset();

  splat_tile_bound_buffer_.reset();
  splat_visibility_buffer_.reset();
  voxel_cascade_buffer_.reset();
  chunk_resident_args_buffer_.reset();
  chunk_resident_cluster_buffer_.reset();
  shadow_atlas_buffer_.reset();
  chunk_shadow_args_buffer_.reset();
  chunk_shadow_pair_buffer_.reset();
  chunk_cell_alias_buffer_.reset();
  chunk_candidate_offset_buffer_.reset();
  chunk_candidate_range_buffer_.reset();
  chunk_candidate_buffer_.reset();
  chunk_cull_args_buffer_.reset();
  chunk_visible_cluster_buffer_.reset();
  chunk_brick_cluster_buffer_.reset();
  chunk_cluster_free_top_buffer_.reset();
  chunk_cluster_free_list_buffer_.reset();
  chunk_cluster_buffer_.reset();
  chunk_cluster_point_buffer_.reset();
  chunk_debug_query_output_buffer_.reset();
  chunk_evict_batch_buffer_.reset();
  chunk_voxelize_batch_data_buffer_.reset();
  chunk_voxelize_batch_slot_buffer_.reset();
  chunk_slot_published_buffer_.reset();
  chunk_brick_free_list_top_buffer_.reset();
  chunk_brick_free_list_buffer_.reset();
  chunk_brick_demand_buffer_.reset();
  chunk_brick_primitive_buffer_.reset();
  chunk_brick_pool_buffer_.reset();
  chunk_indirection_buffer_.reset();
  chunk_table_buffer_.reset();

  vulkan_image_destroy(context_, &taa_output_image_);
  vulkan_image_destroy(context_, &history_images_[1]);
  vulkan_image_destroy(context_, &history_images_[0]);
  vulkan_image_destroy(context_, &ao_image_);
  vulkan_image_destroy(context_, &normal_material_image_);
  vulkan_image_destroy(context_, &depth_image_);
  vulkan_image_destroy(context_, &post_process_image_);
  vulkan_image_destroy(context_, &bloom_temp_image_);
  vulkan_image_destroy(context_, &output_image_);

  // VulkanRendererBackend::shutdown() already vkDeviceWaitIdle()s (waits
  // every queue on the device, not just graphics_queue) before destroying
  // raymarch_shader, so async_compute_queue is guaranteed idle here too --
  // safe to free these unconditionally.
  if (graphics_timestamp_pool_ != VK_NULL_HANDLE) {
    vkDestroyQueryPool(context_->device.logical_device,
                       graphics_timestamp_pool_, context_->allocator);
    graphics_timestamp_pool_ = VK_NULL_HANDLE;
  }
  if (async_timestamp_pool_ != VK_NULL_HANDLE) {
    vkDestroyQueryPool(context_->device.logical_device, async_timestamp_pool_,
                       context_->allocator);
    async_timestamp_pool_ = VK_NULL_HANDLE;
  }
  for (VkSemaphore semaphore : async_chain_semaphores_) {
    vkDestroySemaphore(context_->device.logical_device, semaphore,
                       context_->allocator);
  }
  async_chain_semaphores_.clear();
  async_fences_.clear();
  // Frees each VulkanCommandBuffer's handle back to async_command_pool_
  // (its destructor's job) -- must happen before the pool itself is
  // destroyed below.
  async_command_buffers_.clear();
  if (async_command_pool_ != VK_NULL_HANDLE) {
    vkDestroyCommandPool(context_->device.logical_device, async_command_pool_,
                         context_->allocator);
    async_command_pool_ = VK_NULL_HANDLE;
  }
}

void VulkanRaymarchShader::recreate_render_target_images() {
  render_width_ = std::max(1u, static_cast<u32>(base_width_ * render_scale_));
  render_height_ =
      std::max(1u, static_cast<u32>(base_height_ * render_scale_));

  // Output storage image that pass 3 (render) writes into. Only STORAGE,
  // not TRANSFER_SRC -- the post-process passes (4a/4b) read it via
  // imageLoad, and it's post_process_image_ below that gets blitted to the
  // swapchain now, not this one directly.
  vulkan_image_destroy(context_, &output_image_);
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, render_width_,
                      render_height_, context_->swapchain.image_format.format,
                      VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &output_image_);

  // Half-resolution scratch buffer for the bloom blur's horizontal pass
  // (pass 4a) -- see bloom_temp_image_'s header comment for why half-res.
  vulkan_image_destroy(context_, &bloom_temp_image_);
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, (render_width_ + 1) / 2,
                      (render_height_ + 1) / 2,
                      context_->swapchain.image_format.format,
                      VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &bloom_temp_image_);

  // Final frame, written by pass 4b and blitted (see render_to()) into the
  // swapchain image, upscaling from render_width_/render_height_ to
  // whatever size the swapchain actually is -- TRANSFER_SRC for that blit.
  vulkan_image_destroy(context_, &post_process_image_);
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, render_width_,
                      render_height_, context_->swapchain.image_format.format,
                      VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &post_process_image_);

  // The splat visibility buffer -- sized to the render targets above (one
  // u64 per render-resolution pixel), so it lives and resizes with them;
  // TRANSFER_DST for render_to()'s per-frame vkCmdFillBuffer clear. Stays
  // device-local unlike the point pools (see their own creation comment):
  // it's written and read entirely on the GPU, every frame, at full render
  // resolution -- exactly the traffic that must not cross PCIe.
  splat_visibility_buffer_.reset();
  splat_visibility_buffer_.emplace(
      *context_, static_cast<u64>(render_width_) * render_height_ * sizeof(u64),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  // Its conservative-near-bound companion, one u32 per kSplatTileSize-pixel
  // tile of that same render target (see splat_tile_bound_buffer_'s own
  // comment) -- same lifetime, same device-local/TRANSFER_DST reasoning,
  // ~1/2000th the size.
  // TAA's three render-resolution targets (see their member comments):
  // pass 3's depth, the two ping-ponged history images, and the resolved
  // frame the post-process chain reads. History is 16-bit float rather than
  // the 8-bit output format because a 0.1 blend factor quantizes to nothing
  // in 8 bits and pixels visibly stick.
  vulkan_image_destroy(context_, &depth_image_);
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, render_width_, render_height_,
                      VK_FORMAT_R32_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_STORAGE_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &depth_image_);
  vulkan_image_destroy(context_, &normal_material_image_);
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, render_width_, render_height_,
                      VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_STORAGE_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &normal_material_image_);
  // The ambient-occlusion estimate: one channel, half float, render
  // resolution (see ao_image_'s member comment).
  vulkan_image_destroy(context_, &ao_image_);
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, render_width_, render_height_,
                      VK_FORMAT_R16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_STORAGE_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &ao_image_);
  for (u32 i = 0; i < 2; ++i) {
    vulkan_image_destroy(context_, &history_images_[i]);
    vulkan_image_create(context_, VK_IMAGE_TYPE_2D, render_width_,
                        render_height_, VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                        VK_IMAGE_ASPECT_COLOR_BIT, &history_images_[i]);
  }
  vulkan_image_destroy(context_, &taa_output_image_);
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, render_width_, render_height_,
                      context_->swapchain.image_format.format,
                      VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &taa_output_image_);
  // Every one of those just came back as undefined content, and the
  // reprojection that would have used the old ones no longer applies at
  // this resolution.
  history_valid_ = false;

  splat_tile_bound_buffer_.reset();
  splat_tile_bound_buffer_.emplace(
      *context_,
      static_cast<u64>((render_width_ + kSplatTileSize - 1) / kSplatTileSize) *
          ((render_height_ + kSplatTileSize - 1) / kSplatTileSize) *
          sizeof(u32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

void VulkanRaymarchShader::on_resized(u32 width, u32 height) {
  if (!valid_) {
    return;
  }

  base_width_ = width;
  base_height_ = height;
  recreate_render_target_images();
  rebind_render_target_descriptors();
}

void VulkanRaymarchShader::rebind_render_target_descriptors() {
  // Re-point every descriptor binding that referenced one of the images
  // recreate_render_target_images() just destroyed/recreated -- their old
  // views no longer exist, so the descriptor sets would otherwise
  // reference dangling handles.
  VkDescriptorImageInfo render_image_info{VK_NULL_HANDLE, output_image_.view,
                                          VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo render_depth_info{VK_NULL_HANDLE, depth_image_.view,
                                          VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet render_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  render_write.dstSet = render_set_;
  render_write.dstBinding = 0;
  render_write.descriptorCount = 1;
  render_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  render_write.pImageInfo = &render_image_info;
  VkWriteDescriptorSet render_depth_write = render_write;
  render_depth_write.dstBinding = 24;
  render_depth_write.pImageInfo = &render_depth_info;
  VkDescriptorImageInfo render_normal_info{
      VK_NULL_HANDLE, normal_material_image_.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet render_normal_write = render_write;
  render_normal_write.dstBinding = 25;
  render_normal_write.pImageInfo = &render_normal_info;
  VkDescriptorImageInfo render_ao_info{VK_NULL_HANDLE, ao_image_.view,
                                       VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet render_ao_write = render_write;
  render_ao_write.dstBinding = 27;
  render_ao_write.pImageInfo = &render_ao_info;
  VkWriteDescriptorSet render_writes_images[4] = {
      render_write, render_depth_write, render_normal_write, render_ao_write};
  vkUpdateDescriptorSets(context_->device.logical_device, 4,
                        render_writes_images, 0, nullptr);

  // The AO pass's own set: two G-buffer reads, the AO write, and the voxel
  // cascades it traces against (a buffer, not a render target, but written
  // here so the whole set is populated in one place).
  VkDescriptorImageInfo ao_image_infos[3] = {
      {VK_NULL_HANDLE, depth_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, normal_material_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, ao_image_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkDescriptorBufferInfo ao_cascade_info{voxel_cascade_buffer_->handle(), 0,
                                         VK_WHOLE_SIZE};
  VkWriteDescriptorSet ao_writes[4]{};
  for (u32 i = 0; i < 3; ++i) {
    ao_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ao_writes[i].dstSet = stochastic_ao_set_;
    ao_writes[i].dstBinding = i;
    ao_writes[i].descriptorCount = 1;
    ao_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ao_writes[i].pImageInfo = &ao_image_infos[i];
  }
  ao_writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  ao_writes[3].dstSet = stochastic_ao_set_;
  ao_writes[3].dstBinding = 3;
  ao_writes[3].descriptorCount = 1;
  ao_writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  ao_writes[3].pBufferInfo = &ao_cascade_info;
  vkUpdateDescriptorSets(context_->device.logical_device, 4, ao_writes, 0,
                        nullptr);

  // Both TAA parities: set N reads history_images_[N] and writes
  // history_images_[1-N]; bindings 0/1/4 are the same images in both.
  for (u32 parity = 0; parity < 2; ++parity) {
    VkDescriptorImageInfo taa_infos[5] = {
        {VK_NULL_HANDLE, output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, depth_image_.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, history_images_[parity].view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, history_images_[1 - parity].view,
         VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, taa_output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet taa_writes[5]{};
    for (u32 i = 0; i < 5; ++i) {
      taa_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      taa_writes[i].dstSet = taa_resolve_sets_[parity];
      taa_writes[i].dstBinding = i;
      taa_writes[i].descriptorCount = 1;
      taa_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      taa_writes[i].pImageInfo = &taa_infos[i];
    }
    vkUpdateDescriptorSets(context_->device.logical_device, 5, taa_writes, 0,
                          nullptr);
  }

  // splat_visibility_buffer_ was recreated alongside the images (it's sized to
  // them) -- re-point both bindings that reference it: render_set_'s 20
  // (the raymarcher's read) and chunk_point_splat_set_'s 2 (the splat
  // pass's write).
  VkDescriptorBufferInfo splat_vis_info{splat_visibility_buffer_->handle(), 0,
                                           VK_WHOLE_SIZE};
  VkDescriptorBufferInfo splat_tile_info{splat_tile_bound_buffer_->handle(), 0,
                                            VK_WHOLE_SIZE};
  VkWriteDescriptorSet splat_vis_writes[4]{};
  splat_vis_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  splat_vis_writes[0].dstSet = render_set_;
  splat_vis_writes[0].dstBinding = 20;
  splat_vis_writes[0].descriptorCount = 1;
  splat_vis_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  splat_vis_writes[0].pBufferInfo = &splat_vis_info;
  splat_vis_writes[1] = splat_vis_writes[0];
  splat_vis_writes[1].dstSet = chunk_point_splat_set_;
  splat_vis_writes[1].dstBinding = 2;
  // splat_tile_bound_buffer_ is recreated in the same breath (it's sized to
  // the same render target), so its two bindings need the same treatment:
  // render_set_'s 23 (the raymarcher's trust test) and chunk_point_splat_
  // set_'s 3 (the declined clusters' writes).
  splat_vis_writes[2] = splat_vis_writes[0];
  splat_vis_writes[2].dstBinding = 23;
  splat_vis_writes[2].pBufferInfo = &splat_tile_info;
  splat_vis_writes[3] = splat_vis_writes[2];
  splat_vis_writes[3].dstSet = chunk_point_splat_set_;
  splat_vis_writes[3].dstBinding = 3;
  vkUpdateDescriptorSets(context_->device.logical_device, 4,
                        splat_vis_writes, 0, nullptr);

  VkDescriptorImageInfo bloom_blur_h_image_infos[2] = {
      {VK_NULL_HANDLE, taa_output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, bloom_temp_image_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet bloom_blur_h_writes[2]{};
  for (u32 i = 0; i < 2; ++i) {
    bloom_blur_h_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    bloom_blur_h_writes[i].dstSet = bloom_blur_h_set_;
    bloom_blur_h_writes[i].dstBinding = i;
    bloom_blur_h_writes[i].descriptorCount = 1;
    bloom_blur_h_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bloom_blur_h_writes[i].pImageInfo = &bloom_blur_h_image_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 2, bloom_blur_h_writes,
                        0, nullptr);

  VkDescriptorImageInfo post_composite_image_infos[3] = {
      {VK_NULL_HANDLE, taa_output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, bloom_temp_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, post_process_image_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet post_composite_writes[3]{};
  for (u32 i = 0; i < 3; ++i) {
    post_composite_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    post_composite_writes[i].dstSet = post_composite_set_;
    post_composite_writes[i].dstBinding = i;
    post_composite_writes[i].descriptorCount = 1;
    post_composite_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    post_composite_writes[i].pImageInfo = &post_composite_image_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 3,
                        post_composite_writes, 0, nullptr);
}

void VulkanRaymarchShader::rebake() {
  if (!valid_) {
    KWARN("VulkanRaymarchShader::rebake called on an invalid shader.");
    return;
  }
  // GRAPHICS queue only, deliberately NOT vkDeviceWaitIdle: everything
  // rebuild_static_scene() touches is consumed by graphics-submitted work
  // -- the host-visible scene buffers are read by the render/deferred/GI
  // passes, and the vkUpdateDescriptorSets it does on render_set_ only
  // requires that no PENDING command buffer references that set, which are
  // all graphics submissions too (the async voxelizer binds its own
  // chunk_voxelize_set_, never render_set_). A device-wide idle
  // additionally drained the async compute queue -- i.e. every scene edit
  // blocked on however much of a multi-frame chunk bake was still in
  // flight, which is exactly the "editing stalls" this path kept exhibiting
  // after the bake itself went async. An async bake that reads the scene
  // buffers mid-rewrite can bake stale content, but any chunk still Baking
  // when the edit lands is already queued for a forced re-bake by
  // update_streaming()'s dirty sweep (see its Ready-AND-Baking comment), so
  // the stale result is replaced within a few frames either way.
  vkQueueWaitIdle(context_->device.graphics_queue);
  rebuild_static_scene();
}

void VulkanRaymarchShader::rebuild_static_scene() {
  std::vector<Geometry> all = context_->geometry_system->snapshot();
  const std::vector<SceneLayer> &scene_layers =
      context_->geometry_system->layers();

  std::vector<GpuPrimitive> gpu_primitives(kMaxScenePrimitives, GpuPrimitive{});
  std::vector<f32> gpu_colours(static_cast<size_t>(kMaxScenePrimitives) * 4,
                              1.0f);
  std::vector<f32> gpu_pixelation_exempt(kMaxScenePrimitives, 0.0f);
  // xyz=texture_offset, w=texture_rotation per primitive -- see
  // tex_transform_buffer_'s comment. Left zeroed (no offset, no rotation)
  // for any slot a material/volumetric below doesn't explicitly set.
  std::vector<f32> gpu_tex_transform(static_cast<size_t>(kMaxScenePrimitives) * 4, 0.0f);
  std::vector<VkDescriptorImageInfo> texture_infos(kMaxScenePrimitives);
  // Mirrors texture_infos, one bump-map texture per primitive -- see
  // Material::bump_texture/sample_scene_heights() in Builtin.RaymarchShader.
  // comp.glsl.
  std::vector<VkDescriptorImageInfo> bump_texture_infos(kMaxScenePrimitives);
  // Every alias key is derived from primitive identity and position, so a
  // scene rebuild makes all of them meaningless -- keeping them would let a
  // new bake copy a brick baked from geometry that no longer exists.
  alias_cache_.clear();
  primitive_bounds_.clear();
  std::vector<GpuLayer> gpu_layers(kMaxLayers, GpuLayer{});
  std::vector<GpuParamExpr> gpu_param_exprs(static_cast<size_t>(kMaxScenePrimitives) * 4,
                                            GpuParamExpr{});

  // Default filler for unused slots -- Vulkan requires every element of a
  // fixed-size combined-image-sampler array to be a valid, bound image even
  // if the shader never actually samples that index.
  VulkanTexture &filler_texture = context_->texture_system->default_texture();
  for (auto &info : texture_infos) {
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.imageView = filler_texture.view();
    info.sampler = filler_texture.sampler();
  }
  // Unused bump slots fall back to flat_texture() specifically, not the
  // checkerboard filler_texture above -- a spatially-uniform texture reads
  // as exactly zero bump perturbation (see TextureSystem::flat_texture()'s
  // own comment), whereas the checkerboard would inject a spurious bump
  // pattern into every unused/default-material slot.
  VulkanTexture &flat_texture = context_->texture_system->flat_texture();
  for (auto &info : bump_texture_infos) {
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.imageView = flat_texture.view();
    info.sampler = flat_texture.sampler();
  }

  // geometry.name -> its uploaded index in gpu_primitives, filled in as the
  // loop below assigns each one -- read back afterward by the emissive-
  // light loop (see GpuLight::source_primitive's comment) to record which
  // primitive a synthesized light came from.
  std::unordered_map<std::string, u32> primitive_index_by_name;

  // Primitives must land in primitive_buffer_ grouped contiguously by
  // layer (so each GpuLayer's start/count can slice a contiguous range),
  // but GeometrySystem::snapshot() has no ordering guarantee at all -- so
  // walk layers in index order and, for each, pull out just its own
  // primitives from the snapshot.
  u32 index = 0;
  u32 layer_count = std::min(static_cast<u32>(scene_layers.size()), kMaxLayers);
  // Largest smoothness across every registered layer -- see voxelize()'s
  // own comment for why the fine-sample loop's cull_radius needs this
  // (a smooth blend can reach beyond either operand's own bounding
  // sphere, by up to its smoothness, so cull_radius has to account for
  // whichever layer blends the widest).
  f32 max_smoothness = 0.0f;
  for (u32 layer_i = 0; layer_i < layer_count; ++layer_i) {
    u32 layer_start = index;

    for (const Geometry &geometry : all) {
      if (geometry.layer != layer_i) {
        continue;
      }
      if (index >= kMaxScenePrimitives) {
        KWARN("GeometrySystem has more static primitives than this shader "
             "supports ({}); '{}' will not be rendered.",
             kMaxScenePrimitives, geometry.name);
        continue;
      }

      GpuPrimitive &prim = gpu_primitives[index];
      prim.position_type[0] = geometry.position.x;
      prim.position_type[1] = geometry.position.y;
      prim.position_type[2] = geometry.position.z;
      prim.position_type[3] =
          static_cast<f32>(static_cast<u32>(geometry.type));
      prim.params[0] = geometry.params.x;
      prim.params[1] = geometry.params.y;
      prim.params[2] = geometry.params.z;
      prim.params[3] = geometry.extra_param;

      glm::quat rotation(geometry.rotation);
      prim.rotation[0] = rotation.x;
      prim.rotation[1] = rotation.y;
      prim.rotation[2] = rotation.z;
      prim.rotation[3] = rotation.w;

      prim.expr_scale[0] = geometry.param_expr_scale;
      // Pre-multiplied emissive radiance (see Material::emissive_colour/
      // emissive_intensity and expr_scale's struct comment in
      // Builtin.SdfSceneCommon.inc.glsl) -- (0,0,0) for a non-emissive
      // material, since both fields default to a state that multiplies to
      // zero.
      prim.expr_scale[1] = geometry.material->emissive_colour.r *
                          geometry.material->emissive_intensity;
      prim.expr_scale[2] = geometry.material->emissive_colour.g *
                          geometry.material->emissive_intensity;
      prim.expr_scale[3] = geometry.material->emissive_colour.b *
                          geometry.material->emissive_intensity;

      prim.repeat_mode_cell[0] =
          static_cast<f32>(static_cast<u32>(geometry.repetition_mode));
      prim.repeat_mode_cell[1] = geometry.repetition_cell.x;
      prim.repeat_mode_cell[2] = geometry.repetition_cell.y;
      prim.repeat_mode_cell[3] = geometry.repetition_cell.z;
      prim.repeat_count[0] = geometry.repetition_count.x;
      prim.repeat_count[1] = geometry.repetition_count.y;
      prim.repeat_count[2] = geometry.repetition_count.z;
      prim.repeat_count[3] = geometry_bounding_radius(geometry);

      prim.deform[0] = geometry.twist;
      prim.deform[1] = geometry.bend;
      prim.deform[2] = geometry.displace_amplitude;
      prim.deform[3] = geometry.displace_frequency;

      // Compile each slot's "parametric attribute" formula, if it has one
      // -- an empty string (the default) or a compile failure both just
      // leave instruction_count at 0, meaning "use params[slot]'s plain
      // constant instead" (see evaluate_expr()/resolve_params() in
      // Builtin.RaymarchVoxelize.comp.glsl). compile_expression() already
      // logs why on failure.
      for (size_t slot = 0; slot < geometry.param_expressions.size(); ++slot) {
        const std::string &source = geometry.param_expressions[slot];
        if (source.empty()) {
          continue;
        }
        std::optional<CompiledExpression> compiled = compile_expression(source);
        if (!compiled) {
          continue;
        }
        GpuParamExpr &expr = gpu_param_exprs[static_cast<size_t>(index) * 4 + slot];
        expr.instruction_count = static_cast<i32>(compiled->instructions.size());
        for (size_t i = 0; i < compiled->instructions.size(); ++i) {
          expr.op[i] = static_cast<i32>(compiled->instructions[i].op);
          expr.operand[i] = compiled->instructions[i].operand;
        }
      }

      const glm::vec4 &colour = geometry.material->diffuse_colour;
      gpu_colours[index * 4 + 0] = colour.r;
      gpu_colours[index * 4 + 1] = colour.g;
      gpu_colours[index * 4 + 2] = colour.b;
      // The alpha slot carries this primitive's *effective* texture_scale
      // (world units per texture tile), not colour opacity -- nothing ever
      // read the opacity, and packing the scale here spares a whole extra
      // buffer + binding. See ScenePrimitiveColours in
      // Builtin.RaymarchShader.comp.glsl, the only reader. Multiplied by
      // texture_scale_factor (see its comment on Geometry) rather than
      // uploading material->texture_scale directly, so a scaled scene's
      // texture tiling scales right along with it instead of staying at
      // its authored-size frequency.
      gpu_colours[index * 4 + 3] =
          geometry.material->texture_scale * geometry.texture_scale_factor;

      gpu_pixelation_exempt[index] =
          geometry.material->pixelation_exempt ? 1.0f : 0.0f;

      // Effective texture offset scales with the primitive the same way
      // texture_scale does above (see Geometry::texture_offset_scale's
      // comment); rotation is an angle, so it's used as-is.
      const glm::vec3 effective_offset =
          geometry.material->texture_offset * geometry.texture_offset_scale;
      gpu_tex_transform[index * 4 + 0] = effective_offset.x;
      gpu_tex_transform[index * 4 + 1] = effective_offset.y;
      gpu_tex_transform[index * 4 + 2] = effective_offset.z;
      gpu_tex_transform[index * 4 + 3] = geometry.material->texture_rotation;

      texture_infos[index].imageView =
          geometry.material->diffuse_texture->view();
      texture_infos[index].sampler =
          geometry.material->diffuse_texture->sampler();

      bump_texture_infos[index].imageView =
          geometry.material->bump_texture->view();
      bump_texture_infos[index].sampler =
          geometry.material->bump_texture->sampler();

      primitive_index_by_name[geometry.name] = index;

      // Cached for the bake's per-chunk candidate lists -- see
      // primitive_bounds_. The layer rides in the high 16 bits because the
      // voxelizer needs a candidate's operation and smoothness as well as
      // its shape, and walking a candidate list means it can no longer get
      // those from the enclosing layer loop.
      // The unrepeated instance's own reach: the same bound minus the
      // repetition spread geometry_bounding_radius() adds on top (see
      // geometry_instance_radius()). This is what lets a chunk ask which
      // COPIES touch it rather than only whether the whole tiling does --
      // see PrimitiveBound's own comment for why that distinction is most
      // of a bake's cost on a repetition-heavy scene.
      PrimitiveBound bound;
      bound.position = geometry.position;
      bound.radius = geometry_bounding_radius(geometry);
      bound.packed_index =
          static_cast<i32>(index) | (static_cast<i32>(layer_i) << 16);
      bound.repeat_mode = static_cast<u32>(geometry.repetition_mode);
      bound.repeat_cell = geometry.repetition_cell;
      bound.repeat_count = geometry.repetition_count;
      bound.instance_radius = geometry_instance_radius(geometry);
      bound.rotation = glm::quat(geometry.rotation);
      bound.layer_smoothness = scene_layers[layer_i].smoothness;
      primitive_bounds_.push_back(bound);

      ++index;
    }

    GpuLayer &gpu_layer = gpu_layers[layer_i];
    gpu_layer.op_smoothness[0] =
        static_cast<f32>(static_cast<u32>(scene_layers[layer_i].operation));
    gpu_layer.op_smoothness[1] = scene_layers[layer_i].smoothness;
    gpu_layer.range[0] = static_cast<i32>(layer_start);
    gpu_layer.range[1] = static_cast<i32>(index - layer_start);
    max_smoothness = std::max(max_smoothness, scene_layers[layer_i].smoothness);
  }

  // Volumetric "light shaft" primitives -- appended directly after every
  // opaque primitive, in their own tail range of primitive_buffer_/
  // scene_diffuse_colours/scene_textures that no GpuLayer's range covers, so
  // scene_map() (the voxelize pass, and the render pass's per-pixel material
  // re-evaluation) never iterates them: they're not part of the opaque
  // scene at all. accumulate_volumetrics() in Builtin.RaymarchShader.comp.
  // glsl instead marches through exactly this range directly via
  // primitive_sdf(), which is why the upload below mirrors the opaque loop
  // above closely (same GpuPrimitive/colour/texture layout).
  volumetric_start_ = static_cast<i32>(index);
  std::vector<Volumetric> volumetrics =
      context_->geometry_system->volumetric_snapshot();
  for (const Volumetric &volumetric : volumetrics) {
    if (index >= kMaxScenePrimitives) {
      KWARN("GeometrySystem has more volumetrics than this shader supports "
           "(combined with opaque primitives, cap is {}); '{}' will not be "
           "rendered.",
           kMaxScenePrimitives, volumetric.name);
      continue;
    }

    GpuPrimitive &prim = gpu_primitives[index];
    prim.position_type[0] = volumetric.position.x;
    prim.position_type[1] = volumetric.position.y;
    prim.position_type[2] = volumetric.position.z;
    prim.position_type[3] =
        static_cast<f32>(static_cast<u32>(volumetric.type));
    prim.params[0] = volumetric.params.x;
    prim.params[1] = volumetric.params.y;
    prim.params[2] = volumetric.params.z;
    prim.params[3] = volumetric.extra_param;

    glm::quat rotation(volumetric.rotation);
    prim.rotation[0] = rotation.x;
    prim.rotation[1] = rotation.y;
    prim.rotation[2] = rotation.z;
    prim.rotation[3] = rotation.w;

    // expr_scale.x is param_expr_scale for an opaque primitive's formula-
    // driven params (see resolve_params() in Builtin.SdfSceneCommon.inc.
    // glsl) -- volumetrics never carry a param_expressions formula (every
    // ParamExpr entry at this index is left at instruction_count == 0
    // below), so that value is otherwise dead for an index in this range,
    // and is repurposed here to carry density instead -- the only reader
    // for a volumetric index is accumulate_volumetrics(). yzw stay zero: a
    // volumetric isn't emissive.
    prim.expr_scale[0] = volumetric.density;

    const glm::vec4 &colour = volumetric.material->diffuse_colour;
    gpu_colours[index * 4 + 0] = colour.r;
    gpu_colours[index * 4 + 1] = colour.g;
    gpu_colours[index * 4 + 2] = colour.b;
    gpu_colours[index * 4 + 3] = volumetric.material->texture_scale;

    // No per-instance texture_offset_scale accumulator for volumetrics
    // (mirrors how they already use material->texture_scale directly, with
    // no texture_scale_factor equivalent either).
    gpu_tex_transform[index * 4 + 0] = volumetric.material->texture_offset.x;
    gpu_tex_transform[index * 4 + 1] = volumetric.material->texture_offset.y;
    gpu_tex_transform[index * 4 + 2] = volumetric.material->texture_offset.z;
    gpu_tex_transform[index * 4 + 3] = volumetric.material->texture_rotation;

    texture_infos[index].imageView =
        volumetric.material->diffuse_texture->view();
    texture_infos[index].sampler =
        volumetric.material->diffuse_texture->sampler();

    // No bump_texture_infos write here, deliberately -- a volumetric is
    // never a solid surface with a shaded normal (accumulate_volumetrics()
    // has no bump step at all), so its slot just keeps the flat_texture()
    // filler every index starts at above.

    ++index;
  }
  volumetric_count_ = static_cast<i32>(index) - volumetric_start_;

  // The render pass re-evaluates the analytic scene per hit pixel for
  // material provenance and needs the same layer count the voxelize pass
  // bakes with -- sent as a push constant every render_to() call, like
  // light_count_.
  layer_count_ = static_cast<i32>(layer_count);
  max_smoothness_ = max_smoothness;

  primitive_buffer_->load_data(0, gpu_primitives.size() * sizeof(GpuPrimitive),
                               0, gpu_primitives.data());
  primitive_colour_buffer_->load_data(0, gpu_colours.size() * sizeof(f32), 0,
                                      gpu_colours.data());
  pixelation_exempt_buffer_->load_data(
      0, gpu_pixelation_exempt.size() * sizeof(f32), 0,
      gpu_pixelation_exempt.data());
  tex_transform_buffer_->load_data(0, gpu_tex_transform.size() * sizeof(f32), 0,
                                   gpu_tex_transform.data());
  layer_buffer_->load_data(0, gpu_layers.size() * sizeof(GpuLayer), 0,
                           gpu_layers.data());
  param_expr_buffer_->load_data(0, gpu_param_exprs.size() * sizeof(GpuParamExpr),
                                0, gpu_param_exprs.data());

  std::vector<Light> registered_lights = context_->geometry_system->light_snapshot();
  std::vector<GpuLight> gpu_lights(kMaxLights, GpuLight{});
  if (registered_lights.empty()) {
    // No lights registered anywhere -- fall back to the old hardcoded
    // single directional light so a scene that predates lights (or just
    // doesn't define any) still renders lit exactly as before.
    GpuLight &light = gpu_lights[0];
    light.vector_type[0] = kDefaultLightDirection.x;
    light.vector_type[1] = kDefaultLightDirection.y;
    light.vector_type[2] = kDefaultLightDirection.z;
    light.vector_type[3] = static_cast<f32>(static_cast<u32>(LightType::Directional));
    light.colour_intensity[0] = kDefaultLightColour.r;
    light.colour_intensity[1] = kDefaultLightColour.g;
    light.colour_intensity[2] = kDefaultLightColour.b;
    light.colour_intensity[3] = kDefaultLightIntensity;
    light.source_primitive[0] = -1.0f; // no associated primitive
    light_count_ = 1;
  } else {
    light_count_ = static_cast<i32>(
        std::min(static_cast<u32>(registered_lights.size()), kMaxLights));
    if (registered_lights.size() > kMaxLights) {
      KWARN("GeometrySystem has more lights than this shader supports ({}); "
           "only the first {} will contribute to lighting.",
           kMaxLights, kMaxLights);
    }
    for (i32 i = 0; i < light_count_; ++i) {
      const Light &src = registered_lights[static_cast<size_t>(i)];
      GpuLight &dst = gpu_lights[static_cast<size_t>(i)];
      dst.vector_type[0] = src.vector.x;
      dst.vector_type[1] = src.vector.y;
      dst.vector_type[2] = src.vector.z;
      dst.vector_type[3] = static_cast<f32>(static_cast<u32>(src.type));
      dst.colour_intensity[0] = src.colour.r;
      dst.colour_intensity[1] = src.colour.g;
      dst.colour_intensity[2] = src.colour.b;
      dst.colour_intensity[3] = src.intensity;
      dst.source_primitive[0] = -1.0f; // no associated primitive
    }
  }

  // Emissive primitives (Material::emissive_intensity > 0) also act as
  // light sources -- append one synthesized point light per emissive
  // primitive, positioned at the primitive itself, on top of whatever
  // real Light registrations/fallback populated above. This is what lets
  // an authored "glowing bulb"/"light panel" primitive actually
  // illuminate the rest of the scene (and, since it lands in the same
  // buffer, the GI probe bake too), not just look bright on its own -- see
  // the emissive term in Builtin.RaymarchShader.comp.glsl for the visual
  // half of this.
  for (const Geometry &geometry : all) {
    if (!geometry.material || geometry.material->emissive_intensity <= 0.0f) {
      continue;
    }
    if (static_cast<u32>(light_count_) >= kMaxLights) {
      KWARN("GeometrySystem has more lights (including emissive primitives) "
           "than this shader supports ({}); '{}' will glow but won't "
           "illuminate the rest of the scene.",
           kMaxLights, geometry.name);
      continue;
    }

    // A Plane's own position is always (0,0,0) (see GeometryConfig::
    // plane()/add_plane()) -- only its height (params.x) means anything,
    // so its effective world position for a light is (0, height, 0),
    // mirroring the SDF editor's gizmo_effective_position() convention.
    glm::vec3 position = geometry.type == PrimitiveType::Plane
                             ? glm::vec3(0.0f, geometry.params.x, 0.0f)
                             : geometry.position;

    GpuLight &dst = gpu_lights[static_cast<size_t>(light_count_)];
    dst.vector_type[0] = position.x;
    dst.vector_type[1] = position.y;
    dst.vector_type[2] = position.z;
    dst.vector_type[3] = static_cast<f32>(static_cast<u32>(LightType::Point));
    dst.colour_intensity[0] = geometry.material->emissive_colour.r;
    dst.colour_intensity[1] = geometry.material->emissive_colour.g;
    dst.colour_intensity[2] = geometry.material->emissive_colour.b;
    dst.colour_intensity[3] = geometry.material->emissive_intensity;

    // See GpuLight::source_primitive's comment -- shadow_march() needs
    // this to avoid an emissive primitive's own shell always self-
    // occluding the light it casts. Falls back to -1 (no exclusion) only
    // if this geometry somehow never got uploaded above (e.g. it exceeded
    // kMaxScenePrimitives) -- shouldn't happen in practice since the same
    // cap applies to both loops.
    auto it = primitive_index_by_name.find(geometry.name);
    dst.source_primitive[0] =
        it != primitive_index_by_name.end() ? static_cast<f32>(it->second) : -1.0f;

    ++light_count_;
  }

  // See gpu_index_for_primitive()'s comment -- persisted past this
  // function's return so external callers (tools/sdf_editor) can resolve a
  // primitive's name to its GPU index at any later point, not just here.
  primitive_gpu_index_by_name_ = std::move(primitive_index_by_name);

  ambient_ = context_->geometry_system->ambient();
  light_buffer_->load_data(0, gpu_lights.size() * sizeof(GpuLight), 0,
                          gpu_lights.data());

  VkWriteDescriptorSet texture_writes[2]{};
  texture_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  texture_writes[0].dstSet = render_set_;
  texture_writes[0].dstBinding = 6;
  texture_writes[0].descriptorCount = kMaxScenePrimitives;
  texture_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  texture_writes[0].pImageInfo = texture_infos.data();
  texture_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  texture_writes[1].dstSet = render_set_;
  texture_writes[1].dstBinding = 14;
  texture_writes[1].descriptorCount = kMaxScenePrimitives;
  texture_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  texture_writes[1].pImageInfo = bump_texture_infos.data();
  vkUpdateDescriptorSets(context_->device.logical_device, 2, texture_writes, 0,
                        nullptr);

  // voxelize()/bake_probes() bake the OLD fixed-[-BOUNDS,BOUNDS]-cube field
  // -- once chunked_field_enabled_ is on, render_to() samples the chunked/
  // clipmap field exclusively (see sample_active_field() in Builtin.
  // RaymarchShader.comp.glsl) and never reads this field's output again, so
  // baking it here is pure waste: a full COARSE_DIM^3 voxelize dispatch
  // plus a kProbeBounceCount-bounce GI bake, on EVERY scene edit. This is
  // the single most expensive thing rebuild_static_scene() does, and for a
  // caller like tools/sdf_editor that reloads the whole scene on every
  // gizmo drag/param tweak, it fired constantly for zero visual benefit --
  // confirmed the dominant stall behind the "editor feels slow" reports.
  // Skipped entirely when chunked, not merely made cheaper: nothing below
  // this point reads primitive_buffer_/layer_buffer_/light_buffer_ through
  // the OLD field's own baked output, only through the chunked field's own
  // separate voxelize_chunk()/bake_gi_cascade() (see update_streaming()/
  // update_gi_cascade(), driven by begin_frame() every frame, independent
  // of this function). tools/sdf_editor's synchronous-authoring use case
  // (see rebake()'s own comment) is exactly why this stays gated on the
  // flag rather than removed outright -- sdf_editor sessions that never
  // opt into the chunked field still need this to see anything at all.
  if (!chunked_field_enabled_) {
    // voxelize() and bake_probes() (GI needs the field voxelize() just
    // baked to march gather rays against, and light_count_/ambient_ --
    // just set above -- to shade what those rays hit) share one command
    // buffer and one submission/wait instead of one each (voxelize() +
    // bake_probes()'s zero-fill + kProbeBounceCount bounces used to be 5
    // separate allocate+submit+vkQueueWaitIdle round trips per rebake() --
    // each one fixed overhead on top of its actual GPU work).
    auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
        *context_, context_->device.graphics_command_pool);
    voxelize(*cmd, layer_count, max_smoothness);
    bake_probes(*cmd, static_cast<u32>(light_count_));
    VulkanCommandBuffer::end_single_use(*context_,
                                        context_->device.graphics_command_pool,
                                        std::move(cmd),
                                        context_->device.graphics_queue);
    check_brick_overflow();
  }
}

void VulkanRaymarchShader::write_skybox_binding(VulkanTexture &texture) {
  VkDescriptorImageInfo image_info{};
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  image_info.imageView = texture.view();
  image_info.sampler = texture.sampler();

  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = render_set_;
  write.dstBinding = 12;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &image_info;
  vkUpdateDescriptorSets(context_->device.logical_device, 1, &write, 0, nullptr);
}

void VulkanRaymarchShader::set_skybox(std::string_view texture_name) {
  // Waits for the device to go idle first, like rebake()/remove_scene() --
  // about to rewrite render_set_'s skybox binding directly (and possibly
  // release() the previous texture's last reference, destroying its
  // VkImageView/VkSampler), either of which a still-in-flight render_to()
  // dispatch could be reading through.
  vkDeviceWaitIdle(context_->device.logical_device);

  if (skybox_enabled_) {
    context_->texture_system->release(skybox_texture_name_);
  }

  VulkanTexture &texture =
      context_->texture_system->acquire(texture_name, /*auto_release=*/true);
  skybox_texture_name_ = std::string(texture_name);
  skybox_enabled_ = true;

  write_skybox_binding(texture);
}

void VulkanRaymarchShader::disable_skybox() {
  if (!skybox_enabled_) {
    return;
  }
  vkDeviceWaitIdle(context_->device.logical_device); // see set_skybox()'s comment

  context_->texture_system->release(skybox_texture_name_);
  skybox_texture_name_.clear();
  skybox_enabled_ = false;

  // Re-point the binding at the filler texture rather than leaving it
  // referencing whatever set_skybox() last acquired (now possibly
  // destroyed, if this was its only reference) -- see the constructor's own
  // initial write_skybox_binding() call for the same reasoning.
  write_skybox_binding(context_->texture_system->default_texture());
}

void VulkanRaymarchShader::set_taa_enabled(b8 enabled) noexcept {
  if (taa_enabled_ == enabled) {
    return;
  }
  taa_enabled_ = enabled;
  // Turning TAA on mid-run must not blend against history accumulated while
  // the camera was jittering differently (or not at all).
  history_valid_ = false;
}

void VulkanRaymarchShader::set_render_scale(f32 scale) noexcept {
  scale = std::clamp(scale, 0.05f, 1.0f);
  if (!valid_ || scale == render_scale_) {
    return;
  }
  render_scale_ = scale;

  // Waits for the device to go idle first, like set_skybox() -- about to
  // destroy/recreate output_image_/bloom_temp_image_/post_process_image_
  // and rewrite descriptor bindings pointing at them, either of which a
  // still-in-flight render_to() dispatch could be reading/writing through.
  vkDeviceWaitIdle(context_->device.logical_device);

  recreate_render_target_images();
  rebind_render_target_descriptors();
}

namespace {
// Records one compute-to-compute buffer memory barrier per entry in
// buffers -- shared by voxelize()/bake_probes() below, which each need
// several (their prior write(s) visible to the next dispatch's reads)
// without a full queue idle between every dispatch.
void record_compute_buffer_barriers(VkCommandBuffer cmd,
                                    VkAccessFlags src_access,
                                    VkAccessFlags dst_access,
                                    std::initializer_list<VkBuffer> buffers) {
  std::vector<VkBufferMemoryBarrier> barriers;
  barriers.reserve(buffers.size());
  for (VkBuffer buffer : buffers) {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    barriers.push_back(barrier);
  }
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                      static_cast<u32>(barriers.size()), barriers.data(), 0,
                      nullptr);
}
} // namespace

void VulkanRaymarchShader::voxelize(VulkanCommandBuffer &cmd, u32 layer_count,
                                    f32 max_smoothness) {
  // The pool this pass writes into is allocated on demand -- see
  // ensure_fixed_brick_pool().
  ensure_fixed_brick_pool();

  // Zero the brick allocation counter before the shader's atomicAdd calls.
  vkCmdFillBuffer(cmd.handle(), brick_counter_buffer_->handle(), 0,
                 sizeof(u32), 0);

  VkBufferMemoryBarrier fill_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  fill_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fill_barrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  fill_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  fill_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  fill_barrier.buffer = brick_counter_buffer_->handle();
  fill_barrier.offset = 0;
  fill_barrier.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                      &fill_barrier, 0, nullptr);

  voxelize_pipeline_->bind(cmd);
  vkCmdBindDescriptorSets(cmd.handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         voxelize_pipeline_->layout(), 0, 1, &voxelize_set_, 0,
                         nullptr);

  VoxelizePushConstants push_constants{static_cast<i32>(layer_count), max_smoothness};
  vkCmdPushConstants(cmd.handle(), voxelize_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(VoxelizePushConstants), &push_constants);

  constexpr u32 local_size = 4; // must match local_size_x/y/z in the shader
  u32 groups = (kCoarseDim + local_size - 1) / local_size;
  vkCmdDispatch(cmd.handle(), groups, groups, groups);

  // bake_probes() (recorded right after this into the same command buffer
  // -- see rebuild_static_scene(), the only caller of either) marches
  // gather rays against indirection_buffer_/brick_pool_buffer_ and reads
  // brick_primitive_buffer_ for shading, all just written above -- make
  // that visible explicitly. Previously implicit (voxelize() had its own
  // command buffer, and end_single_use()'s vkQueueWaitIdle enforced this
  // before bake_probes()'s first dispatch could even be submitted); now
  // both share one command buffer with no queue idle between them.
  record_compute_buffer_barriers(
      cmd.handle(), VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT,
      {indirection_buffer_->handle(), brick_pool_buffer_->handle(),
       brick_primitive_buffer_->handle()});
}

void VulkanRaymarchShader::check_brick_overflow() {
  // The counter counts *demand* (every cell that wanted a brick bumps it),
  // not just successful allocations, so once voxelize()'s dispatch has
  // actually finished (the caller waited on the submission this was
  // recorded into) this tells us whether the pool was big enough. Overflow
  // doesn't crash -- the shader just leaves the losing cells brickless, in
  // nondeterministic atomicAdd order -- but it renders as random missing/
  // stray chunks of surface, so make it loud instead of leaving the next
  // person to rediscover that from the artifacts alone.
  if (void *mapped = brick_counter_buffer_->lock(0, sizeof(u32), 0)) {
    u32 bricks_demanded = *static_cast<u32 *>(mapped);
    brick_counter_buffer_->unlock();
    if (bricks_demanded > kMaxBricks) {
      KWARN("Voxelize pass wanted {} bricks but the pool only holds {} -- "
            "{} surface cells were dropped and will render as missing/"
            "corrupted chunks. Raise kMaxBricks (and MAX_BRICKS in "
            "Builtin.RaymarchVoxelize.comp.glsl) or shrink the scene.",
            bricks_demanded, kMaxBricks, bricks_demanded - kMaxBricks);
    }
  }
}

void VulkanRaymarchShader::check_chunk_brick_overflow() {
  // See this method's own header comment for the "per-batch, not per-
  // resident-chunk" caveat and exactly when it's safe to call this.
  if (void *mapped = chunk_brick_demand_buffer_->lock(
          0, sizeof(u32) * kChunkBakeStatCount, 0)) {
    const u32 *stats = static_cast<const u32 *>(mapped);
    u32 bricks_demanded = stats[0];
    // Where a bake's milliseconds actually go -- see ChunkBrickDemand
    // Buffer in the shader. Only populated while bake stats are on.
    if (bake_stats_enabled_ && stats[1] > 0) {
      const u64 evals = static_cast<u64>(stats[1]) + stats[2] + stats[3];
      KINFO("[bakestats] cell-centre {} | sub-block probe {} | voxel {} "
           "(filled-not-evaluated {}, {:.0f}% skipped) | splat positions {} "
           "| TOTAL scene evals {}",
           stats[1], stats[2], stats[3], stats[4],
           100.0 * static_cast<f64>(stats[4]) /
               std::max<f64>(1.0, static_cast<f64>(stats[3]) + stats[4]),
           stats[5], evals);
    }
    chunk_brick_demand_buffer_->unlock();
    if (bricks_demanded > kMaxChunkBricks) {
      KWARN("Chunked-field voxelize batch wanted {} bricks but the shared "
            "pool only holds {} -- {} surface cells were dropped and will "
            "render as missing/corrupted chunks. Raise kMaxChunkBricksPerLevel "
            "(and MAX_BRICKS in Builtin.ChunkVoxelize.comp.glsl/Builtin."
            "ChunkEvict.comp.glsl) or shrink the scene.",
            bricks_demanded, kMaxChunkBricks, bricks_demanded - kMaxChunkBricks);
    }
  }
}

// Seeds a DEVICE-LOCAL buffer from CPU data, via a throwaway host-visible
// staging buffer and a one-time transfer submission.
//
// Exists because the chunked field's free lists, cluster records and brick
// ->cluster owner table all moved out of host memory (see their creation
// comments): the GPU hammers them, the CPU only ever seeds them, and that
// is precisely the shape a staging upload is for. Called only from
// reset_chunked_field() -- a handful of copies at init and on a scene
// reset, never per frame -- so the one-time command buffer's own submit-
// and-wait costs nothing worth optimising.
void VulkanRaymarchShader::upload_to_device_local(VulkanBuffer &dest,
                                                 const void *data, u64 size) {
  if (size == 0) {
    return;
  }
  VulkanBuffer staging(*context_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!staging.is_valid()) {
    KERROR("Failed to allocate a {} byte staging buffer seeding the chunked "
          "field; its pools will start with undefined contents.",
          size);
    return;
  }
  staging.load_data(0, size, 0, data);
  staging.copy_to(dest, 0, 0, size, context_->device.graphics_queue,
                 context_->device.graphics_command_pool);
}

void VulkanRaymarchShader::reset_chunked_field() {
  // Every table entry (across all kNumLevels' worth) starts "no chunk
  // resident" -- voxelize_chunk() fills in exactly the entries it assigns
  // a slot to. Still host-visible (and so still a direct map/fill): unlike
  // everything below it, write_chunk_table_entry() rewrites entries of this
  // one from the CPU every frame that streams a chunk.
  const u64 table_entries =
      static_cast<u64>(kNumLevels) * kChunkTableDim * kChunkTableDim * kChunkTableDim;
  if (void *mapped = chunk_table_buffer_->lock(0, table_entries * sizeof(i32), 0)) {
    i32 *table = static_cast<i32 *>(mapped);
    std::fill(table, table + table_entries, -1);
    chunk_table_buffer_->unlock();
  }

  // No slot holds a published bake yet -- see chunk_slot_published_buffer_.
  {
    std::vector<u32> unpublished(
        static_cast<size_t>(kNumLevels) * kMaxResidentChunks, 0u);
    if (void *mapped = chunk_slot_published_buffer_->lock(
            0, unpublished.size() * sizeof(u32), 0)) {
      std::memcpy(mapped, unpublished.data(), unpublished.size() * sizeof(u32));
      chunk_slot_published_buffer_->unlock();
    }
  }

  // Free list starts full: every brick index available, in order --
  // Builtin.ChunkVoxelize.comp.glsl pops from the top down (see its own
  // comment: allocation order never matters, only that every index is
  // handed out at most once between resets).
  {
    std::vector<i32> free_list(kMaxChunkBricks);
    for (u32 i = 0; i < kMaxChunkBricks; ++i) {
      free_list[i] = static_cast<i32>(i);
    }
    upload_to_device_local(*chunk_brick_free_list_buffer_, free_list.data(),
                           free_list.size() * sizeof(i32));
  }
  {
    const u32 top = kMaxChunkBricks;
    upload_to_device_local(*chunk_brick_free_list_top_buffer_, &top,
                           sizeof(top));
  }

  // The cluster pool gets the same treatment as the brick pool above: every
  // page on the free list, in order.
  {
    std::vector<i32> free_list(kMaxChunkClusters);
    for (u32 i = 0; i < kMaxChunkClusters; ++i) {
      free_list[i] = static_cast<i32>(i);
    }
    upload_to_device_local(*chunk_cluster_free_list_buffer_, free_list.data(),
                           free_list.size() * sizeof(i32));
  }
  {
    const u32 top = kMaxChunkClusters;
    upload_to_device_local(*chunk_cluster_free_top_buffer_, &top, sizeof(top));
  }

  // Every cluster record starts zeroed, which reads as meta.y == 0, "this
  // page is free" -- that is what makes walking the WHOLE pool safe from the
  // first frame, with no residency list, in the splat and cull passes alike.
  {
    std::vector<u8> zeroed(
        static_cast<size_t>(kMaxChunkClusters) * sizeof(GpuChunkCluster), 0u);
    upload_to_device_local(*chunk_cluster_buffer_, zeroed.data(),
                           zeroed.size());
  }

  // ...and no brick owns any page yet (-1 is "no page", not page 0).
  {
    std::vector<i32> owners(
        static_cast<size_t>(kMaxChunkBricks) * kChunkMaxClustersPerBrick, -1);
    upload_to_device_local(*chunk_brick_cluster_buffer_, owners.data(),
                           owners.size() * sizeof(i32));
  }
}

// Writes value into world_chunk_coord's entry of the toroidal chunk table
// (level-offset, wrapped exactly like Builtin.ChunkedFieldCommon.inc.glsl's
// sample_chunked_field_at_level() resolves it GPU-side) -- shared by
// voxelize_chunk() (value = the new gpu_slot) and evict_chunk() (value =
// -1). Host-visible write, picked up by any later dispatch/frame that
// reads chunk_table_buffer_ with no barrier needed (see reset_chunked_
// field()'s identical host-visible-write pattern).
void VulkanRaymarchShader::write_chunk_table_entry(glm::ivec3 world_chunk_coord,
                                                    u32 level, i32 value) {
  auto floor_mod = [](i32 v, i32 m) { return ((v % m) + m) % m; };
  i32 table_dim = static_cast<i32>(kChunkTableDim);
  glm::ivec3 wrapped(floor_mod(world_chunk_coord.x, table_dim),
                     floor_mod(world_chunk_coord.y, table_dim),
                     floor_mod(world_chunk_coord.z, table_dim));
  i32 table_index = static_cast<i32>(level) * table_dim * table_dim * table_dim +
      wrapped.x + wrapped.y * table_dim + wrapped.z * table_dim * table_dim;
  if (void *mapped = chunk_table_buffer_->lock(
          static_cast<u64>(table_index) * sizeof(i32), sizeof(i32), 0)) {
    *static_cast<i32 *>(mapped) = value;
    chunk_table_buffer_->unlock();
  }
}

// Marks (or unmarks) a chunk slot as holding a completed, published bake --
// see chunk_slot_published_buffer_'s own comment. Always changed in the same
// breath as that slot's chunk-table entry: the table is what the field
// SAMPLER resolves through, this is what the cluster pool's flat sweep
// consults, and a disagreement between them means either invisible geometry
// or splats from a half-baked page.
void VulkanRaymarchShader::set_chunk_slot_published(u32 gpu_slot,
                                                    bool published) {
  if (gpu_slot >= kNumLevels * kMaxResidentChunks) {
    return;
  }
  if (void *mapped = chunk_slot_published_buffer_->lock(
          static_cast<u64>(gpu_slot) * sizeof(u32), sizeof(u32), 0)) {
    *static_cast<u32 *>(mapped) = published ? 1u : 0u;
    chunk_slot_published_buffer_->unlock();
  }
}

// Makes finished bakes reachable. Called at the top of update_streaming(),
// every frame, and never blocks.
//
// This is the other half of not waiting on the bake (see update_streaming()'s
// own comment). A bake's results stay completely unreachable from the moment
// it is queued -- the chunk table doesn't name its slot, and the cluster
// cull skips it -- until this sees that submission's fence signalled. Fence
// observation is also what makes the writes VISIBLE to the graphics queue
// without a semaphore: a host fence wait followed by a later queue submission
// is a valid Vulkan memory dependency, and this frame's graphics submission
// happens after this call.
//
// Polling is per ring SLOT, not per chunk, because a slot's fence covers its
// whole batch -- there is no finer completion signal available, and none is
// needed: a batch is a single dispatch.
// See the declaration -- the one operation that keeps the alias cache
// honest. Cheap by construction: a monotonic counter bump, no search.
void VulkanRaymarchShader::invalidate_slot_content(u32 slot) {
  if (slot >= slot_content_generation_.size()) {
    return;
  }
  slot_content_generation_[slot] = next_slot_content_generation_++;
}

void VulkanRaymarchShader::publish_completed_bakes() {
  for (u32 ring = 0; ring < kAsyncRingDepth; ++ring) {
    if (async_pending_publish_[ring].empty()) {
      continue;
    }
    if (ring >= async_fences_.size() || !async_fences_[ring]->poll()) {
      continue; // still baking -- try again next frame
    }
    for (const PendingChunkPublish &pending : async_pending_publish_[ring]) {
      if (pending.level >= chunk_streaming_levels_.size()) {
        continue; // shader torn down or never finished constructing
      }
      ChunkStreamingManager &streaming = chunk_streaming_levels_[pending.level];
      const auto &resident = streaming.resident_chunks();
      auto it = resident.find(pending.key);
      // The chunk may have been evicted, or evicted and reloaded into a
      // different slot, while this bake was in flight -- in which case the
      // slot this entry names is no longer that chunk's, and publishing it
      // would point the table at content the scheduler has already given
      // away. bake_generation distinguishes "still the same load" from "a
      // newer one happened to land on the same slot".
      const bool still_current = it != resident.end() &&
          it->second.gpu_slot == pending.slot &&
          it->second.bake_generation == pending.bake_generation &&
          it->second.state != ChunkState::Evicting;
      if (still_current) {
        glm::ivec3 chunk_coord(static_cast<i32>(pending.key.cx),
                               static_cast<i32>(pending.key.cy),
                               static_cast<i32>(pending.key.cz));
        write_chunk_table_entry(chunk_coord, pending.level,
                                static_cast<i32>(pending.slot));
        set_chunk_slot_published(pending.slot, true);
      }
      // The replaced slot (a double-buffered in-place rebake) is given up
      // either way: if the publish went through, the table now names the
      // new slot and nothing reads the old one; if it didn't, the chunk is
      // gone and neither slot is wanted. Its bricks and cluster pages are
      // freed by the next frame's evict batch.
      if (pending.retire_slot != kInvalidChunkSlot) {
        set_chunk_slot_published(pending.retire_slot, false);
        pending_slot_retirements_.push_back(pending.retire_slot);
        streaming.release_slot(pending.retire_slot);
      }
    }
    async_pending_publish_[ring].clear();
  }
}

// Blocks until every in-flight bake has finished, then publishes them all.
//
// DEBUG AND VERIFICATION ONLY. The entire point of publish_completed_bakes()
// is that the render loop never blocks on a bake; this is the deliberate
// opposite, for the synchronous debug_verify_*() harnesses below, which
// drive update_streaming() in a tight loop with no real frames in between
// and then query the field expecting it to have settled. Without it they
// would finish with every bake still in flight and nothing published.
void VulkanRaymarchShader::drain_streaming_for_debug() {
  for (auto &fence : async_fences_) {
    fence->wait(UINT64_MAX);
  }
  publish_completed_bakes();
}

void VulkanRaymarchShader::voxelize_chunk(VulkanCommandBuffer &cmd,
                                          glm::ivec3 world_chunk_coord, u32 level,
                                          u32 gpu_slot, u32 layer_count,
                                          f32 max_smoothness) {
  // The single-chunk convenience wrapper is only used by the synchronous
  // debug_verify_*() harness, which submits and waits before reading -- so
  // it publishes immediately rather than going through publish_completed_
  // bakes()' deferred path (there is no async ring slot involved at all).
  write_chunk_table_entry(world_chunk_coord, level, static_cast<i32>(gpu_slot));
  set_chunk_slot_published(gpu_slot, true);
  // This slot's contents are about to be replaced -- see the alias cache.
  invalidate_slot_content(gpu_slot);

  f32 level_world_size = chunk_level_world_size(level);
  f32 level_cell_size = level_world_size / static_cast<f32>(kChunkCoarseDim);
  glm::vec3 chunk_world_min = glm::vec3(world_chunk_coord) * level_world_size;
  GpuChunkBatchEntry entry{static_cast<i32>(gpu_slot), chunk_world_min.x,
                           chunk_world_min.y, chunk_world_min.z, level_cell_size};
  voxelize_chunk_batch(cmd, {entry}, layer_count, max_smoothness);
}


// Works out which cells of one chunk bake to identical bricks, so the
// voxelizer can copy them instead of evaluating them (see ChunkCellAlias
// Buffer in Builtin.ChunkVoxelize.comp.glsl).
//
// WHY THIS IS WORTH THE CPU TIME. A chunk's cost is dominated by the brick
// fill: every near-surface cell evaluates the scene at roughly two thousand
// points. Repeated architecture makes most of that redundant -- a level-4
// chunk spans 64 world units in 4-unit cells, and a 10-unit repetition
// period puts cells 5 apart in X and Z at exactly the same place relative to
// the tiling. They see the same scene, so they bake the same numbers. Out of
// 256 cells in an XZ plane about 25 are distinct; the rest are copies.
//
// HOW A CELL IS IDENTIFIED. The key is what the shader would actually fold:
// every candidate that can reach this cell, in the same order, each
// contributing its identity and this cell's position RELATIVE to it. Two
// cells whose keys agree therefore evaluate the same function.
//
// The relative position is reduced modulo the repetition period for a
// candidate the per-chunk instance resolution left unresolved -- which at
// the coarse levels is exactly the repeated architecture this exists for
// (a chunk there spans dozens of copies, far past the resolve cap, so it
// keeps the shader's own repeat_limited() fold). Reduction is done in the
// primitive's LOCAL frame, because that is where domain repetition is
// applied.
//
// WHAT MAKES A CELL INELIGIBLE. Anything that breaks translational
// periodicity: rotational repetition, and the outermost instances of a
// finite tiling, where repeat_limited() clamps and neighbouring copies stop
// being congruent. Those cells simply bake normally.
//
// Quantisation bounds the error rather than risking it: positions are
// snapped to a hundredth of a voxel, so two cells that collide in the key
// describe fields differing by at most that -- far below anything the field
// can represent. Returns how many cells were marked as aliases.
u32 VulkanRaymarchShader::build_cell_alias_map(
    const std::vector<i32> &chunk_candidates,
    const std::vector<glm::vec4> &chunk_candidate_offsets, u32 candidate_count,
    glm::vec3 chunk_min, f32 cell_size, f32 max_smoothness, u32 chunk_slot,
    const std::unordered_set<u32> &excluded_slots, i32 *out_alias) {
  const u32 cells = kChunkCellCount;
  for (u32 i = 0; i < cells; ++i) {
    out_alias[i] = -1;
  }
  if (!chunk_dedup_enabled_) {
    return 0; // opt-in -- see chunk_dedup_enabled_
  }
  if (candidate_count == 0 || candidate_count > kMaxCandidatesPerChunk) {
    return 0; // no list (or the whole-scene fallback) -- nothing to reason on
  }
  if (chunk_slot >= slot_content_generation_.size()) {
    return 0; // defensive: an entry outside the slot space can neither be
             // stamped nor validated, so it must not enter the cache
  }

  // Gather what each candidate needs for the key, once, rather than
  // re-deriving it per cell.
  struct AliasTerm {
    i32 packed_index = 0;
    glm::vec3 position{0.0f};   // primitive world position
    glm::mat3 inverse_rotation{1.0f};
    glm::vec3 local_offset{0.0f}; // resolved instance offset, local space
    glm::vec3 period{0.0f};       // 0 on an axis with no repetition
    glm::vec3 half_span{0.0f};    // in instances, for the clamped edge test
    f32 radius = 0.0f;
    bool periodic = false;  // reduce modulo period rather than use position
    bool blocks = false;    // any cell reaching this one cannot be aliased
    bool always_in_reach = false;
  };
  std::vector<AliasTerm> terms;
  terms.reserve(candidate_count);
  bool any_periodic = false;
  for (u32 c = 0; c < candidate_count; ++c) {
    const i32 packed = chunk_candidates[c];
    const u32 prim = static_cast<u32>(packed) & 0xFFFFu;
    if (prim >= primitive_bounds_.size()) {
      return 0;
    }
    const PrimitiveBound &bound = primitive_bounds_[prim];
    AliasTerm term;
    term.packed_index = packed;
    term.position = bound.position;
    term.inverse_rotation = glm::mat3_cast(glm::inverse(bound.rotation));
    term.radius = bound.instance_radius;
    const glm::vec4 &offset = chunk_candidate_offsets[c];
    const u32 mode = bound.repeat_mode;
    if (offset.w != 0.0f) {
      // Instance-resolved: a single named copy, treated exactly like an
      // unrepeated primitive at a shifted position.
      term.local_offset = glm::vec3(offset);
    } else if (mode == static_cast<u32>(RepetitionMode::None)) {
      // Nothing to do -- plain primitive at its own position.
    } else if (mode == static_cast<u32>(RepetitionMode::Limited) ||
               mode == static_cast<u32>(RepetitionMode::Rectangular) ||
               mode == static_cast<u32>(RepetitionMode::Infinite)) {
      const bool rect = mode == static_cast<u32>(RepetitionMode::Rectangular);
      term.period = rect ? glm::vec3(bound.repeat_cell.x, 0.0f, bound.repeat_cell.z)
                         : bound.repeat_cell;
      const glm::vec3 counts =
          rect ? glm::vec3(bound.repeat_count.x, 1.0f, bound.repeat_count.z)
               : bound.repeat_count;
      term.half_span = glm::max(counts - 1.0f, glm::vec3(0.0f)) * 0.5f;
      if (mode == static_cast<u32>(RepetitionMode::Infinite)) {
        // Never clamps, so every instance is interior.
        term.half_span = glm::vec3(1e9f);
      } else {
        // AN AXIS WITH A COUNT OF ONE IS NOT REPEATED, whatever its cell
        // spacing says. repeat_limited() clamps that axis's instance id to
        // zero, so it keeps the sample point's absolute local coordinate --
        // and the reduction below must do the same rather than treating it
        // as a period.
        //
        // Getting this wrong disabled deduplication completely, in a way
        // that looked like the whole idea failing rather than a bug:
        // DiegosOffice's repeated boxes are authored 20 x 1 x 20, so Y has
        // a spacing of 1.0 but a count of 1. That gave Y a half_span of 0,
        // the interior test below (|id| <= half_span - 2) could never pass,
        // and every single cell was marked unaliasable. Measured 0% of
        // cells copied where the arithmetic predicts 90%.
        for (int axis = 0; axis < 3; ++axis) {
          if (counts[axis] <= 1.0f) {
            term.period[axis] = 0.0f;
          }
        }
      }
      term.periodic = true;
      term.always_in_reach = true;
      any_periodic = true;
    } else {
      // Rotational repetition is periodic in ANGLE, not in translation --
      // no lattice of equivalent cells exists for it.
      term.blocks = true;
    }
    terms.push_back(term);
  }
  if (!any_periodic) {
    // Without a repeating candidate two distinct cells would have to
    // coincidentally sit at matching offsets from every primitive around
    // them, which does not happen in authored scenes.
    return 0;
  }

  // Can this level alias AT ALL? Two cells are equivalent only when the
  // whole-cells displacement between them is also a whole number of
  // repetition periods, so the smallest step that qualifies is
  // lcm(cell_size, period) measured in cells -- and if that exceeds the
  // chunk, no two of its cells can ever match.
  //
  // This is the difference between paying for the sweep and not: for a
  // 10-unit period the step is 40 cells at level 0 and 20 at level 1,
  // both past the 16-cell chunk, while levels 2 to 4 come in at 10, 5 and
  // 5. Checking it costs a few dozen operations and skips roughly a
  // millisecond of hashing per chunk on exactly the levels that stream
  // most often.
  bool level_can_alias = false;
  for (const AliasTerm &term : terms) {
    if (!term.periodic) {
      continue;
    }
    for (int axis = 0; axis < 3 && !level_can_alias; ++axis) {
      if (term.period[axis] <= 1e-5f) {
        continue;
      }
      const f64 ratio = static_cast<f64>(cell_size) /
                        static_cast<f64>(term.period[axis]);
      for (u32 m = 1; m < kChunkCoarseDim; ++m) {
        const f64 steps = ratio * static_cast<f64>(m);
        if (std::abs(steps - std::round(steps)) < 1e-4) {
          level_can_alias = true;
          break;
        }
      }
    }
    if (level_can_alias) {
      break;
    }
  }
  if (!level_can_alias) {
    return 0;
  }

  const f32 voxel_size = cell_size / static_cast<f32>(kChunkBrickDim);
  const f32 quantum = voxel_size * 0.01f;
  // The same bound the brick fill culls against (CULL_RADIUS_CELLS in the
  // shader) -- a candidate farther than this provably cannot change any
  // voxel in the cell, so leaving it out of the key cannot make two
  // different cells look alike.
  const f32 cull_radius = cell_size * 6.0f + max_smoothness;

  auto quantise = [&](f32 v) {
    return static_cast<i64>(std::llround(static_cast<f64>(v) / quantum));
  };
  auto mix64 = [](u64 h, u64 v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  };

  // Entries are only usable while the slot they name still holds the bake
  // that produced them, and while nothing this frame is about to free it.
  auto entry_usable = [&](const AliasCacheEntry &entry) {
    const u32 slot = entry.global_cell / kChunkCellCount;
    if (slot >= slot_content_generation_.size() ||
        slot_content_generation_[slot] != entry.slot_generation) {
      return false; // that slot has been reloaded or freed since
    }
    // An evict recorded EARLIER in this same submission frees the bricks
    // before the voxelize dispatch that would copy from them runs.
    return excluded_slots.find(slot) == excluded_slots.end();
  };

  if (alias_cache_.size() > kMaxAliasCacheEntries) {
    alias_cache_.clear();
  }
  u32 aliased = 0;

  for (u32 cell = 0; cell < cells; ++cell) {
    const glm::ivec3 local(static_cast<i32>(cell % kChunkCoarseDim),
                           static_cast<i32>((cell / kChunkCoarseDim) % kChunkCoarseDim),
                           static_cast<i32>(cell / (kChunkCoarseDim * kChunkCoarseDim)));
    const glm::vec3 cell_min = chunk_min + glm::vec3(local) * cell_size;
    const glm::vec3 cell_max = cell_min + glm::vec3(cell_size);

    // SEEDED WITH THE CELL SIZE, which is what makes a global map safe.
    // The rest of the key is scene-relative -- primitive identity and this
    // cell's position relative to each -- and says nothing about how much
    // world one cell covers. That was fine while the map lived inside a
    // single chunk (hence a single level), but a shared map spans levels,
    // and a level-0 cell and a level-4 cell sitting at the same relative
    // offset would otherwise key identically and copy each other's bricks
    // -- geometry at 1/16th the intended extent, silently.
    u64 key = mix64(1469598103934665603ull,
                    static_cast<u64>(std::llround(
                        static_cast<f64>(cell_size) * 1024.0)));
    bool aliasable = true;
    for (const AliasTerm &term : terms) {
      if (!term.always_in_reach) {
        // The INSTANCE's position, not the primitive's. For an entry the
        // per-chunk resolution pinned to one copy, term.position is the
        // primitive's own origin and the copy sits local_offset away from
        // it -- testing the origin would include or exclude the wrong
        // candidate, and a candidate wrongly left out of the key is how two
        // genuinely different cells come to look identical.
        const glm::vec3 instance_position =
            term.position + glm::transpose(term.inverse_rotation) *
                                term.local_offset;
        const glm::vec3 outside = glm::max(
            glm::max(cell_min - instance_position,
                     instance_position - cell_max),
            glm::vec3(0.0f));
        const f32 slack = term.radius + cull_radius;
        if (glm::dot(outside, outside) > slack * slack) {
          continue; // provably cannot affect this cell
        }
      }
      if (term.blocks) {
        aliasable = false;
        break;
      }
      glm::vec3 rel =
          term.inverse_rotation * (cell_min - term.position) - term.local_offset;
      if (term.periodic) {
        for (int axis = 0; axis < 3; ++axis) {
          if (term.period[axis] <= 1e-5f) {
            continue; // this axis is not repeated -- keep it absolute
          }
          const f32 id = std::round(rel[axis] / term.period[axis]);
          // repeat_limited() checks ONE neighbouring tile either side and
          // clamps ids to the tiling's extent, so periodicity holds exactly
          // while this instance and both its neighbours are unclamped --
          // |id| + 1 <= half_span. A wider margin costs real coverage: on a
          // count of 10 (half_span 4.5) requiring two spare instances leaves
          // only ids -2..2 eligible instead of -3..3, discarding nearly a
          // third of the tiling's interior for nothing.
          if (std::abs(id) + 1.0f > term.half_span[axis]) {
            aliasable = false;
            break;
          }
          rel[axis] -= id * term.period[axis];
        }
        if (!aliasable) {
          break;
        }
      }
      key = mix64(key, static_cast<u64>(term.packed_index));
      key = mix64(key, static_cast<u64>(quantise(rel.x)));
      key = mix64(key, static_cast<u64>(quantise(rel.y)));
      key = mix64(key, static_cast<u64>(quantise(rel.z)));
    }
    if (!aliasable) {
      continue;
    }
    const u32 global_cell = chunk_slot * kChunkCellCount + cell;
    auto it = alias_cache_.find(key);
    if (it != alias_cache_.end() && entry_usable(it->second)) {
      // Already baked somewhere still resident -- copy it. If that
      // representative came from an earlier batch this is the cache doing
      // the work; if it came from this one, it is deduplication. Same key,
      // same copy, different provenance.
      if (it->second.global_cell / kChunkCellCount != chunk_slot) {
        ++alias_cells_cached_;
      }
      out_alias[cell] = static_cast<i32>(it->second.global_cell);
      ++aliased;
      continue;
    }
    // First sighting (or the previous representative is gone): this cell
    // bakes, and becomes what everything matching it copies from.
    alias_cache_[key] =
        AliasCacheEntry{global_cell, slot_content_generation_[chunk_slot]};
  }
  return aliased;
}

// Builds one chunk's primitive candidate list -- the primitives whose
// bounds can reach it, with domain repetition resolved to the individual
// copies that actually come within range (see ChunkCandidateBuffer in
// Builtin.ChunkVoxelize.comp.glsl).
//
// Factored out of voxelize_chunk_batch()'s per-entry loop so the list for
// one chunk can be built (and reasoned about) on its own.
//
// out_candidates/out_offsets must have room for kMaxCandidatesPerChunk.
// Returns the count, or a value GREATER than kMaxCandidatesPerChunk to mean
// "no usable list, fold the whole scene" -- the caller encodes that as -1.
u32 VulkanRaymarchShader::build_chunk_candidates(glm::vec3 chunk_min,
                                                 f32 cell_size,
                                                 f32 max_smoothness,
                                                 i32 *out_candidates,
                                                 glm::vec4 *out_offsets) {
    const glm::vec3 chunk_max =
    chunk_min + glm::vec3(cell_size * static_cast<f32>(kChunkCoarseDim));
  // The widest radius any sample inside this chunk will cull against --
  // the brick loop's own cull_radius (see CULL_RADIUS_CELLS in the
  // shader). Building the list against the LARGEST radius keeps it valid
  // for every query the bake makes, including the tighter cell-centre one.
  const f32 reach = cell_size * 6.0f + max_smoothness;

  // The chunk's box mapped into a primitive's own local space, as a
  // centre + half-extent pair. Repetition is expressed in local space
  // (primitive_sdf() applies the inverse rotation first), so the instance
  // search has to happen there too. A rotated box's local-space AABB is
  // the box's centre plus the row-wise absolute of the rotation applied
  // to its half-extents -- the standard conservative OBB-to-AABB bound.
  auto local_space_box = [&](const PrimitiveBound &bound,
                           glm::vec3 &out_centre, glm::vec3 &out_half) {
    const glm::vec3 world_centre = (chunk_min + chunk_max) * 0.5f;
    const glm::vec3 world_half = (chunk_max - chunk_min) * 0.5f;
    const glm::mat3 inverse_rotation = glm::mat3_cast(glm::inverse(bound.rotation));
    out_centre = inverse_rotation * (world_centre - bound.position);
    out_half = glm::abs(inverse_rotation[0]) * world_half.x +
             glm::abs(inverse_rotation[1]) * world_half.y +
             glm::abs(inverse_rotation[2]) * world_half.z;
  };

  u32 count = 0;
  for (const PrimitiveBound &bound : primitive_bounds_) {
    // AN UNBOUNDED PRIMITIVE JOINS THE LIST; IT DOES NOT DESTROY IT.
    // This used to set count past the cap and break, marking the whole
    // chunk "no list, fold the entire scene" -- so a SINGLE plane,
    // infinitely-repeating primitive, or (much more commonly) any
    // primitive carrying a parametric-attribute formula switched this
    // optimisation off for every chunk of every bake, scene-wide. That
    // is not a rare edge: geometry_bounding_radius() reports unbounded
    // for any non-empty param_expressions (see geometry_system.cpp), and
    // an ordinary authored scene has a handful of those -- DiegosOffice
    // has three, and was therefore baking every chunk at the full
    // un-culled cost while the candidate machinery looked like it was
    // working.
    //
    // "Unbounded" means only that no static bound proves the primitive
    // CAN'T reach this chunk, so it must be evaluated here -- which is
    // exactly what putting it in the list does. The list stays correct
    // (same primitives, same ascending fold order scene_map() would have
    // walked) and stays short, because the many genuinely-bounded
    // primitives around it are still culled.
    if (bound.radius < kUnboundedBoundingRadius) {
    // Distance from the primitive's centre to the chunk box, which is
    // zero when it is inside: the standard clamped point-AABB test.
    const glm::vec3 outside =
        glm::max(glm::max(chunk_min - bound.position,
                          bound.position - chunk_max),
                 glm::vec3(0.0f));
    if (glm::dot(outside, outside) >
        (bound.radius + reach) * (bound.radius + reach)) {
      continue;
    }
    }
    // --- Resolve domain repetition to the instances that actually reach
    // this chunk. See PrimitiveBound's own comment for why the whole-
    // tiling bounding sphere the test above uses is nearly worthless
    // here: for DiegosOffice's structural boxes it is ~134 world units,
    // wider than the entire streamed field, so those primitives survive
    // that test in every chunk and then cost eight evaluations per
    // sample. Enumerating instead usually finds exactly ONE copy within
    // reach, occasionally two, and often none at all.
    const bool limited = bound.repeat_mode ==
      static_cast<u32>(RepetitionMode::Limited);
    const bool rectangular = bound.repeat_mode ==
      static_cast<u32>(RepetitionMode::Rectangular);
    if ((limited || rectangular) &&
      bound.instance_radius < kUnboundedBoundingRadius) {
    glm::vec3 local_centre;
    glm::vec3 local_half;
    local_space_box(bound, local_centre, local_half);
    // Rectangular repetition leaves Y alone entirely -- model that as a
    // single instance on that axis (see repeat_rectangular()).
    const glm::vec3 cell = rectangular
        ? glm::vec3(bound.repeat_cell.x, 0.0f, bound.repeat_cell.z)
        : bound.repeat_cell;
    const glm::vec3 counts = rectangular
        ? glm::vec3(bound.repeat_count.x, 1.0f, bound.repeat_count.z)
        : bound.repeat_count;

    // Per axis, the inclusive instance-id range whose copy can still
    // come within (instance_radius + reach) of the chunk box. An axis
    // with no active cell contributes exactly id 0, matching
    // repeat_limited()'s own "cell <= 0 leaves this axis alone".
    const f32 slack = bound.instance_radius + reach;
    glm::ivec3 lo(0);
    glm::ivec3 hi(0);
    bool enumerable = true;
    i64 instance_total = 1;
    for (int axis = 0; axis < 3 && enumerable; ++axis) {
      if (cell[axis] <= 1e-5f) {
        continue; // inactive axis -- id 0 only, already set
      }
      const f32 half_span =
          std::max(counts[axis] - 1.0f, 0.0f) * 0.5f;
      const f32 reach_lo = local_centre[axis] - local_half[axis] - slack;
      const f32 reach_hi = local_centre[axis] + local_half[axis] + slack;
      // ids are clamped to [-half_span, half_span] by repeat_limited(),
      // so the outermost copies absorb everything beyond the tiling.
      const i32 id_lo = static_cast<i32>(std::ceil(
          std::max(reach_lo / cell[axis], -half_span) - 1e-4f));
      const i32 id_hi = static_cast<i32>(std::floor(
          std::min(reach_hi / cell[axis], half_span) + 1e-4f));
      if (id_lo > id_hi) {
        enumerable = false; // no copy on this axis reaches the chunk
        instance_total = 0;
        break;
      }
      lo[axis] = id_lo;
      hi[axis] = id_hi;
      instance_total *= static_cast<i64>(id_hi - id_lo + 1);
    }

    if (instance_total == 0) {
      continue; // every copy is out of reach -- cull the primitive
    }
    // Splitting instances apart is only equivalent to repeat_*()'s
    // min() when the fold is a hard one. With a real blend radius the
    // second instance would smooth into the first instead, so fall
    // through to the unresolved entry and let the shader do exactly
    // what it always did.
    const bool splittable =
        enumerable && instance_total > 1 && bound.layer_smoothness <= 0.0f;
    if (enumerable && (instance_total == 1 || splittable) &&
        instance_total <= kMaxResolvedInstancesPerPrimitive) {
      bool overflowed = false;
      for (i32 iz = lo.z; iz <= hi.z && !overflowed; ++iz) {
        for (i32 iy = lo.y; iy <= hi.y && !overflowed; ++iy) {
          for (i32 ix = lo.x; ix <= hi.x && !overflowed; ++ix) {
            if (count >= kMaxCandidatesPerChunk) {
              ++count; // overflowed -- fall back to the whole scene
              overflowed = true;
              break;
            }
            out_candidates[count] =
                bound.packed_index;
            out_offsets[count] =
                glm::vec4(static_cast<f32>(ix) * cell.x,
                          static_cast<f32>(iy) * cell.y,
                          static_cast<f32>(iz) * cell.z, 1.0f);
            ++count;
          }
        }
      }
      if (overflowed) {
        break;
      }
      continue;
    }
    }

    if (count >= kMaxCandidatesPerChunk) {
    ++count; // overflowed -- fall back to the whole scene below
    break;
    }
    out_candidates[count] = bound.packed_index;
    // w = 0: not instance-resolved, evaluate the primitive's own
    // repetition fold exactly as before.
    out_offsets[count] = glm::vec4(0.0f);
    ++count;
  }
  return count;
}

// Bakes an entire BATCH of chunks -- one shared dispatch instead of one per
// chunk (see Builtin.ChunkVoxelize.comp.glsl's own header comment and
// update_streaming()'s, the real caller for a non-trivial batch; voxelize_
// chunk() above is a single-entry convenience wrapper kept for the debug_
// verify_*() harness methods, which only ever bake one chunk at a time).
// entries.size() must not exceed kMaxChunkBatchSize (chunk_voxelize_batch_
// slot_buffer_/chunk_voxelize_batch_data_buffer_'s own capacity) --
// callers own staying within that budget (see kMaxChunkBatchSize's own
// comment for the worst-case math that keeps update_streaming() safely
// under it). A no-op if entries is empty -- callers don't need to guard
// that themselves.
void VulkanRaymarchShader::voxelize_chunk_batch(
    VulkanCommandBuffer &cmd, const std::vector<GpuChunkBatchEntry> &entries,
    u32 layer_count, f32 max_smoothness,
    const std::unordered_set<u32> &evicting_slots) {
  if (entries.empty()) {
    return;
  }
  KASSERT(entries.size() <= kMaxChunkBatchSize);
  // Carried alongside this slot's begin/end timestamps so the reported bake
  // cost can be divided down to a per-chunk number -- see
  // async_bake_chunks_'s own comment.
  async_bake_chunks_[async_ring_index_] = static_cast<u32>(entries.size());

  // Split into the two parallel arrays Builtin.ChunkVoxelize.comp.glsl's
  // batch_chunk_slot[]/batch_chunk_data[] actually read -- see chunk_
  // voxelize_batch_slot_buffer_'s own comment for why entries can't be
  // uploaded as one packed array of GpuChunkBatchEntry directly (std430's
  // struct-array-stride rounding would silently misalign every entry
  // past the first).
  std::vector<i32> slots(entries.size());
  std::vector<glm::vec4> data(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    slots[i] = entries[i].chunk_slot;
    data[i] = glm::vec4(entries[i].chunk_world_min_x, entries[i].chunk_world_min_y,
                        entries[i].chunk_world_min_z, entries[i].chunk_cell_size);
  }
  // This ring slot's own region -- see the buffers' creation comment.
  const u32 batch_offset = async_ring_index_ * kMaxChunkBatchSize;

  // --- Per-chunk candidate lists (see chunk_candidate_buffer_). For each
  // chunk, the primitives whose bounding sphere reaches into it, once,
  // instead of every voxel re-deciding that for every primitive. ---
  std::vector<i32> candidates(entries.size() * kMaxCandidatesPerChunk, 0);
  std::vector<glm::vec4> candidate_offsets(
      entries.size() * kMaxCandidatesPerChunk, glm::vec4(0.0f));
  std::vector<glm::ivec4> candidate_ranges(entries.size(), glm::ivec4(0));
  // -1 everywhere means "every cell bakes normally" -- the state any chunk
  // build_cell_alias_map() declines to analyse is left in.
  std::vector<i32> cell_alias(entries.size() * kChunkCellCount, -1);
  u32 aliased_cells = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const f32 cell_size = entries[i].chunk_cell_size;
    const glm::vec3 chunk_min(entries[i].chunk_world_min_x,
                              entries[i].chunk_world_min_y,
                              entries[i].chunk_world_min_z);
    const u32 count = build_chunk_candidates(
        chunk_min, cell_size, max_smoothness,
        candidates.data() + i * kMaxCandidatesPerChunk,
        candidate_offsets.data() + i * kMaxCandidatesPerChunk);

    ++candidate_chunks_total_;
    if (count > kMaxCandidatesPerChunk) {
      ++candidate_chunks_fallback_;
    }
    candidate_ranges[i] = glm::ivec4(
        static_cast<i32>((batch_offset + i) * kMaxCandidatesPerChunk),
        count > kMaxCandidatesPerChunk ? -1 : static_cast<i32>(count), 0, 0);

    // Which of this chunk's cells are copies of another -- see build_cell_
    // alias_map(). Reads the list just built, so it must follow it.
    //
    // Skipped wholesale when deduplication is off rather than relying on
    // build_cell_alias_map()'s own early return: the two per-chunk copies
    // below are built per classify, and copying a few kilobytes to hand to
    // a function that immediately returns is pure waste on the default
    // path.
    if (!chunk_dedup_enabled_) {
      continue;
    }
    alias_cells_total_ += kChunkCellCount;
    std::vector<i32> chunk_slice(
        candidates.begin() + static_cast<ptrdiff_t>(i * kMaxCandidatesPerChunk),
        candidates.begin() +
            static_cast<ptrdiff_t>((i + 1) * kMaxCandidatesPerChunk));
    std::vector<glm::vec4> offset_slice(
        candidate_offsets.begin() +
            static_cast<ptrdiff_t>(i * kMaxCandidatesPerChunk),
        candidate_offsets.begin() +
            static_cast<ptrdiff_t>((i + 1) * kMaxCandidatesPerChunk));
    aliased_cells += build_cell_alias_map(
        chunk_slice, offset_slice,
        count > kMaxCandidatesPerChunk ? 0u : count, chunk_min, cell_size,
        max_smoothness, static_cast<u32>(entries[i].chunk_slot),
        evicting_slots, cell_alias.data() + i * kChunkCellCount);
  }
  if (void *mapped = chunk_candidate_buffer_->lock(
          static_cast<u64>(batch_offset) * kMaxCandidatesPerChunk * sizeof(i32),
          candidates.size() * sizeof(i32), 0)) {
    std::memcpy(mapped, candidates.data(), candidates.size() * sizeof(i32));
    chunk_candidate_buffer_->unlock();
  }
  if (void *mapped = chunk_candidate_offset_buffer_->lock(
          static_cast<u64>(batch_offset) * kMaxCandidatesPerChunk *
              sizeof(glm::vec4),
          candidate_offsets.size() * sizeof(glm::vec4), 0)) {
    std::memcpy(mapped, candidate_offsets.data(),
               candidate_offsets.size() * sizeof(glm::vec4));
    chunk_candidate_offset_buffer_->unlock();
  }
  if (void *mapped = chunk_candidate_range_buffer_->lock(
          static_cast<u64>(batch_offset) * sizeof(glm::ivec4),
          candidate_ranges.size() * sizeof(glm::ivec4), 0)) {
    std::memcpy(mapped, candidate_ranges.data(),
               candidate_ranges.size() * sizeof(glm::ivec4));
    chunk_candidate_range_buffer_->unlock();
  }
  alias_cells_aliased_ += aliased_cells;
  if (void *mapped = chunk_cell_alias_buffer_->lock(
          static_cast<u64>(batch_offset) * kChunkCellCount * sizeof(i32),
          cell_alias.size() * sizeof(i32), 0)) {
    std::memcpy(mapped, cell_alias.data(), cell_alias.size() * sizeof(i32));
    chunk_cell_alias_buffer_->unlock();
  }
  if (void *mapped = chunk_voxelize_batch_slot_buffer_->lock(
          static_cast<u64>(batch_offset) * sizeof(i32),
          slots.size() * sizeof(i32), 0)) {
    std::memcpy(mapped, slots.data(), slots.size() * sizeof(i32));
    chunk_voxelize_batch_slot_buffer_->unlock();
  }
  if (void *mapped = chunk_voxelize_batch_data_buffer_->lock(
          static_cast<u64>(batch_offset) * sizeof(glm::vec4),
          data.size() * sizeof(glm::vec4), 0)) {
    std::memcpy(mapped, data.data(), data.size() * sizeof(glm::vec4));
    chunk_voxelize_batch_data_buffer_->unlock();
  }

  // Zero this batch's shared demand counter -- mirrors voxelize()'s
  // identical vkCmdFillBuffer+barrier for brick_counter_buffer_ (see its
  // comment). Nothing currently reads chunk_brick_demand_buffer_ back for
  // the chunked field specifically (unlike voxelize()'s own check_brick_
  // overflow() for the old field), so zeroing it once per BATCH rather
  // than once per chunk changes nothing observable -- this was already
  // the only thing keeping the old per-chunk voxelize_chunk() from being
  // batchable as-is.
  vkCmdFillBuffer(cmd.handle(), chunk_brick_demand_buffer_->handle(), 0,
                 sizeof(u32), 0);
  VkBufferMemoryBarrier fill_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  fill_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fill_barrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  fill_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  fill_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  fill_barrier.buffer = chunk_brick_demand_buffer_->handle();
  fill_barrier.offset = 0;
  fill_barrier.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                      &fill_barrier, 0, nullptr);

  chunk_voxelize_pipeline_->bind(cmd);
  vkCmdBindDescriptorSets(cmd.handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         chunk_voxelize_pipeline_->layout(), 0, 1,
                         &chunk_voxelize_set_, 0, nullptr);

  ChunkVoxelizePushConstants push_constants{
      static_cast<i32>(layer_count), max_smoothness,
      static_cast<i32>(entries.size()), static_cast<i32>(batch_offset),
      /*pass=*/0, bake_stats_enabled_ ? 1 : 0};
  vkCmdPushConstants(cmd.handle(), chunk_voxelize_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(ChunkVoxelizePushConstants), &push_constants);

  // Z covers every entry's own CHUNK_COARSE_DIM range back to back -- see
  // Builtin.ChunkVoxelize.comp.glsl's main() for how batch_index/local Z
  // get unpacked back out of gl_GlobalInvocationID.z.
  // Must match local_size_x/y/z in the shader: 8x8 over a cell slab, and ONE
  // cell row per invocation in Z.
  constexpr u32 local_size_xy = 8;
  const u32 groups_xy = (kChunkCoarseDim + local_size_xy - 1) / local_size_xy;
  const u32 dispatch_z = kChunkCoarseDim * static_cast<u32>(entries.size());
  vkCmdDispatch(cmd.handle(), groups_xy, groups_xy, dispatch_z);

  // --- Pass 1: the aliased cells, copied from the representatives pass 0
  // just baked. Skipped entirely when nothing aliased, which is every chunk
  // on the fine clip levels (a chunk there is smaller than a repetition
  // period, so no two of its cells can be copies). ---
  if (aliased_cells > 0) {
    // Pass 1 READS the bricks and indirection entries pass 0 WROTE, and two
    // dispatches in one command buffer have no ordering without this.
    // Every buffer pass 1 touches, not just the ones it READS from pass 0.
    // It reads the representative's indirection entry and brick voxels, but
    // it also pops from the SAME brick and cluster free lists pass 0 popped
    // from -- and a dispatch does not see an earlier dispatch's writes,
    // atomic or otherwise, without a barrier. Leaving the free lists out
    // would let pass 1 read a stale stack pointer and hand out brick
    // indices pass 0 had already claimed, which is silent corruption of two
    // chunks at once rather than an error.
    record_compute_buffer_barriers(
        cmd.handle(), VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        {chunk_indirection_buffer_->handle(),
         chunk_brick_pool_buffer_->handle(),
         chunk_brick_primitive_buffer_->handle(),
         chunk_brick_free_list_buffer_->handle(),
         chunk_brick_free_list_top_buffer_->handle(),
         chunk_brick_demand_buffer_->handle(),
         chunk_cluster_point_buffer_->handle(),
         chunk_cluster_buffer_->handle(),
         chunk_cluster_free_list_buffer_->handle(),
         chunk_cluster_free_top_buffer_->handle(),
         chunk_brick_cluster_buffer_->handle()});
    push_constants.pass = 1;
    vkCmdPushConstants(cmd.handle(), chunk_voxelize_pipeline_->layout(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0,
                      sizeof(ChunkVoxelizePushConstants), &push_constants);
    vkCmdDispatch(cmd.handle(), groups_xy, groups_xy, dispatch_z);
  }

  // Make this batch's writes visible to whatever reads the chunked field
  // next in the same command buffer (query_chunked_field(), or a later
  // phase's own dispatches) -- same reasoning as voxelize()'s identical
  // trailing barrier.
  record_compute_buffer_barriers(
      cmd.handle(), VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
      {chunk_indirection_buffer_->handle(), chunk_brick_pool_buffer_->handle(),
       chunk_brick_primitive_buffer_->handle(),
       chunk_cluster_point_buffer_->handle(), chunk_cluster_buffer_->handle(),
       chunk_cluster_free_list_buffer_->handle(),
       chunk_cluster_free_top_buffer_->handle(),
       chunk_brick_cluster_buffer_->handle()});
}

void VulkanRaymarchShader::query_chunked_field(VulkanCommandBuffer &cmd,
                                               glm::vec3 query_world_pos) {
  chunk_debug_query_pipeline_->bind(cmd);
  vkCmdBindDescriptorSets(cmd.handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         chunk_debug_query_pipeline_->layout(), 0, 1,
                         &chunk_debug_query_set_, 0, nullptr);

  ChunkDebugQueryPushConstants push_constants{query_world_pos.x, query_world_pos.y,
                                              query_world_pos.z};
  vkCmdPushConstants(cmd.handle(), chunk_debug_query_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(ChunkDebugQueryPushConstants), &push_constants);

  vkCmdDispatch(cmd.handle(), 1, 1, 1);
}

void VulkanRaymarchShader::query_clipmap_field(VulkanCommandBuffer &cmd,
                                               glm::vec3 query_world_pos,
                                               glm::vec3 camera_pos) {
  chunk_debug_query_pipeline_->bind(cmd);
  vkCmdBindDescriptorSets(cmd.handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         chunk_debug_query_pipeline_->layout(), 0, 1,
                         &chunk_debug_query_set_, 0, nullptr);

  ChunkDebugQueryPushConstants push_constants{
      query_world_pos.x, query_world_pos.y, query_world_pos.z,
      /*use_clipmap=*/1, camera_pos.x, camera_pos.y, camera_pos.z};
  vkCmdPushConstants(cmd.handle(), chunk_debug_query_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(ChunkDebugQueryPushConstants), &push_constants);

  vkCmdDispatch(cmd.handle(), 1, 1, 1);
}

void VulkanRaymarchShader::evict_chunk(VulkanCommandBuffer &cmd,
                                       glm::ivec3 world_chunk_coord, u32 level,
                                       u32 gpu_slot, bool clear_table_entry,
                                       bool insert_barrier) {
  // Clear the table entry FIRST (before the GPU dispatch below even runs)
  // -- see this method's header comment for why leaving it stale, even
  // briefly, would let a query resolve through to gpu_slot's about-to-be-
  // freed (or already-reused) content instead of correctly seeing "no
  // chunk resident." Same level-offset table index as voxelize_chunk().
  //
  // Skipped when clear_table_entry is false (the dirty-in-place-rebake
  // caller in update_streaming()) -- there, gpu_slot never goes back to
  // the free pool at all (the very next dispatch this same command buffer
  // re-voxelizes straight into it), so the table entry stays valid,
  // pointing at the OLD content, right up until that dispatch overwrites
  // it with the new bake -- see this method's header comment.
  if (clear_table_entry) {
    write_chunk_table_entry(world_chunk_coord, level, -1);
  }
  evict_chunk_batch(cmd, {static_cast<i32>(gpu_slot)}, insert_barrier);
}

// Frees a whole BATCH of chunks' bricks in ONE dispatch instead of one per
// chunk -- see Builtin.ChunkEvict.comp.glsl's own header comment and
// update_streaming()'s, the real caller for a non-trivial batch; evict_
// chunk() above is a single-entry convenience wrapper for the debug_
// verify_*() harness methods. Level-agnostic (a gpu_slot alone already
// says which level's sub-range it's in), and does NOT touch chunk_table_
// buffer_ at all -- that's each caller's own concern (see evict_chunk()'s
// clear_table_entry, or update_streaming()'s per-entry write before
// adding to its batch), since which entries need a table clear and which
// don't (the dirty-in-place-rebake case) can differ within the same
// batch. slots.size() must not exceed kMaxChunkBatchSize -- see voxelize_
// chunk_batch()'s identical constraint. A no-op if slots is empty.
void VulkanRaymarchShader::evict_chunk_batch(VulkanCommandBuffer &cmd,
                                             const std::vector<i32> &slots,
                                             bool insert_barrier) {
  if (slots.empty()) {
    return;
  }
  KASSERT(slots.size() <= kMaxChunkBatchSize);

  const u32 batch_offset = async_ring_index_ * kMaxChunkBatchSize;
  if (void *mapped = chunk_evict_batch_buffer_->lock(
          static_cast<u64>(batch_offset) * sizeof(i32),
          slots.size() * sizeof(i32), 0)) {
    std::memcpy(mapped, slots.data(), slots.size() * sizeof(i32));
    chunk_evict_batch_buffer_->unlock();
  }

  chunk_evict_pipeline_->bind(cmd);
  vkCmdBindDescriptorSets(cmd.handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         chunk_evict_pipeline_->layout(), 0, 1,
                         &chunk_evict_set_, 0, nullptr);

  ChunkEvictPushConstants push_constants{static_cast<i32>(slots.size()),
                                        static_cast<i32>(batch_offset)};
  vkCmdPushConstants(cmd.handle(), chunk_evict_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(ChunkEvictPushConstants), &push_constants);

  constexpr u32 local_size = 4; // must match local_size_x/y/z in the shader
  u32 groups_per_chunk = (kChunkCoarseDim + local_size - 1) / local_size;
  vkCmdDispatch(cmd.handle(), groups_per_chunk, groups_per_chunk,
               groups_per_chunk * static_cast<u32>(slots.size()));

  // Make this batch's free-list push visible to whatever pops from it
  // next -- a later voxelize_chunk_batch() this same frame (or a future
  // one). Skipped when insert_barrier is false -- see this method's
  // header comment; the caller is batching several evict_chunk_batch()
  // calls together and will record one shared barrier itself afterward
  // (not expected in practice now that a single batch already covers the
  // whole frame's evictions -- kept for the same reason evict_chunk()'s
  // identical parameter is kept, API symmetry for a future caller that
  // might still want it).
  if (insert_barrier) {
    record_compute_buffer_barriers(
        cmd.handle(), VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        {chunk_indirection_buffer_->handle(),
         chunk_brick_free_list_buffer_->handle(),
         chunk_brick_free_list_top_buffer_->handle(),
         chunk_cluster_buffer_->handle(),
         chunk_cluster_free_list_buffer_->handle(),
         chunk_cluster_free_top_buffer_->handle(),
         chunk_brick_cluster_buffer_->handle()});
  }
}

void VulkanRaymarchShader::update_streaming(glm::vec3 camera_pos) {
  // Nothing below is safe on a shader whose construction failed: chunk_
  // streaming_levels_ is only populated once every chunked-field buffer
  // allocated, so on a failure it is EMPTY while kNumLevels still says
  // there are five of them. The per-level loop then indexes past the end.
  //
  // Reachable in practice, not theoretical -- a device that runs out of
  // memory creating the chunked field's pools leaves exactly this state,
  // and the frame loop keeps calling this every frame regardless. Every
  // other public entry point on this class already guards the same way
  // (rebake(), set_render_scale(), debug_probe_field()); this one was
  // simply missed, and only ever got away with it because nothing it
  // touched was indexed before the streaming work landed.
  if (!valid_) {
    return;
  }

  // Make any bake that finished since last frame reachable, before anything
  // below plans against the resulting residency -- see publish_completed_
  // bakes(). Cheap and non-blocking on a frame where nothing has finished.
  publish_completed_bakes();

  // Whether THIS frame may queue new chunk work at all.
  //
  // The ring slot about to be used still has to be free before its command
  // buffer and batch-buffer region can be rewritten, and that has always
  // been true -- what changed is what happens when it isn't. ensure_async_
  // cmd() below blocks the CPU on the fence, which was fine while the
  // graphics queue waited on the bake anyway (the CPU was already pinned to
  // the bake's cadence). Now that it doesn't, blocking here would be the
  // stall coming back one level down: the render thread would sit in
  // vkWaitForFences for the rest of a multi-second bake instead of drawing.
  //
  // So a frame that finds its slot busy simply doesn't queue anything. It
  // costs nothing to skip: update() is idempotent and re-prioritises by
  // distance every call, so whatever this frame declined is re-offered next
  // frame, nearest-first, against a fresher camera position. The fence is
  // still waited on inside ensure_async_cmd(), where it is now always
  // already signalled -- kept rather than removed so the invariant that a
  // slot is never rewritten under a live submission doesn't depend on this
  // check alone.
  const bool ring_available = async_ring_index_ < async_fences_.size() &&
      async_fences_[async_ring_index_]->poll();

  // --- Camera velocity, for the streaming window's lead bias. Measured in
  // world units per update_streaming() call and converted to a per-second
  // rate below; this runs exactly once per frame, so the frame's own delta
  // is the right clock even though it isn't passed in. ---
  glm::vec3 frame_step(0.0f);
  if (have_last_stream_camera_pos_) {
    frame_step = camera_pos - last_stream_camera_pos_;
  }
  last_stream_camera_pos_ = camera_pos;
  have_last_stream_camera_pos_ = true;
  // A floating-origin recenter teleports the camera in render space (see
  // consume_origin_shift()); so does loading a new scene. Neither is
  // motion, and treating it as motion would fling the window across the
  // world. Anything past a level-0 chunk in one frame is one of those.
  if (glm::length(frame_step) > kChunkWorldSize) {
    frame_step = glm::vec3(0.0f);
    stream_velocity_ = glm::vec3(0.0f);
  }
  stream_velocity_ = stream_velocity_ * (1.0f - kStreamVelocitySmoothing) +
                     frame_step * kStreamVelocitySmoothing;

  // --- How many chunks this submission may bake, from what a chunk has
  // actually been costing (see kSubmissionTargetMs and the estimate's own
  // pessimistic seed). ---
  const f64 affordable =
      kSubmissionTargetMs / std::max(bake_ms_per_chunk_estimate_, 1e-3);
  const u32 submission_budget = static_cast<u32>(
      std::clamp(affordable, 1.0, static_cast<f64>(kMaxChunksPerSubmission)));

  // Chunks this call queues, held back from the chunk table until their
  // bake's fence signals -- see PendingChunkPublish and publish_completed_
  // bakes().
  std::vector<PendingChunkPublish> queued_publishes;


  // Lazily claims this call's slot of async_command_buffers_ the first
  // time any chunk work actually needs recording -- most calls (camera
  // stationary, nothing dirty) never touch this at all, so they never pay
  // for a fence wait or an empty submission. See update_streaming()'s own
  // header comment for the ring's full design; every evict_chunk()/
  // voxelize_chunk()/record_compute_buffer_barriers() call below that used
  // to take the caller's cmd now goes through this instead.
  VulkanCommandBuffer *async_cmd = nullptr;
  auto ensure_async_cmd = [&]() -> VulkanCommandBuffer & {
    if (!async_cmd) {
      // Wait EVERY ring slot's fence, not just this slot's own: chunk_
      // Only THIS ring slot's fence. The batch buffers now carry one region
      // per slot (see their creation comment), so the memcpy below can only
      // collide with this slot's own previous use -- a full ring lap ago,
      // and almost always long finished. Waiting on every slot's fence, as
      // this did while the regions were shared, meant blocking on the most
      // recent bake every frame: invisible in a small scene where chunk
      // work is occasional, and a per-frame stall in a large one where
      // moving the camera generates chunk work continuously.
      async_fences_[async_ring_index_]->wait(UINT64_MAX);
      // Drain this slot's pending publishes BEFORE the fence reset below
      // and the submit that overwrites async_pending_publish_[slot]. A
      // dropped publish is a chunk that baked completely but whose table
      // entry is never written: invisible forever, holding its gpu_slot
      // (and any retire_slot) hostage until the free pool starves.
      // Idempotent and cheap when there is nothing to drain.
      publish_completed_bakes();
      // Reads chunk_brick_demand_buffer_, which a still-in-flight
      // submission from the OTHER slot may be incrementing. That makes the
      // number it sees possibly one batch stale -- fine for what it is, an
      // overflow warning about a pool that is either comfortably large or
      // persistently too small, not a value anything computes with.
      check_chunk_brick_overflow();
      // The async queue has no ordering against the GRAPHICS queue's
      // still-in-flight previous frame either (separate VkQueue -- zero
      // implicit ordering), and that frame's render_to()/GI gather reads
      // the very buffers the dispatches recorded here will rewrite. That
      // write-after-read hazard is now handled on the GPU, by making this
      // submission WAIT on context_->graphics_timeline (see its comment in
      // vulkan_types.inl) rather than by draining the graphics queue from
      // the CPU here. Same ordering, without serializing the two
      // processors against each other on every frame that has chunk work.
      // This slot's previous bake is confirmed finished by the fence wait
      // above, so its two timestamps are readable now, without blocking.
      if (timestamps_supported_ && async_bake_recorded_[async_ring_index_]) {
        u64 stamps[2] = {0, 0};
        if (vkGetQueryPoolResults(context_->device.logical_device,
                                  async_timestamp_pool_,
                                  async_ring_index_ * 2, 2, sizeof(stamps),
                                  stamps, sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            stamps[1] > stamps[0]) {
          const f64 ms = static_cast<f64>(stamps[1] - stamps[0]) *
                         static_cast<f64>(timestamp_period_ns_) * 1e-6;
          async_bake_ms_total_ += ms;
          async_bake_ms_peak_ = std::max(async_bake_ms_peak_, ms);
          ++async_bake_samples_;
          // Feed the adaptive submission size (see kSubmissionTargetMs).
          //
          // The estimate is a DECAYING PEAK, not an average: chunk cost is
          // bimodal -- an empty chunk bakes in 0.05ms and a dense one in
          // hundreds -- and a mean describes neither. Any sample above it
          // replaces it outright so one expensive chunk immediately shrinks
          // the next submission, and the 3% decay per measured submission
          // keeps a single monster chunk from pinning it high across tens
          // of seconds of cheap content. Floored at 1ms so it can never
          // decay to nothing.
          bake_ms_per_chunk_estimate_ =
              std::max(bake_ms_per_chunk_estimate_ * 0.97, 1.0);
          if (async_bake_chunks_[async_ring_index_] > 0) {
            async_bake_chunks_total_ +=
                static_cast<f64>(async_bake_chunks_[async_ring_index_]);
            const f64 per_chunk =
                ms / static_cast<f64>(async_bake_chunks_[async_ring_index_]);
            bake_ms_per_chunk_estimate_ =
                std::max(per_chunk, bake_ms_per_chunk_estimate_);
          }
          // Hard ceiling, purely defensive: the budget floors at one chunk
          // long before this, so a larger value adds no scheduling
          // information -- it can only be a measurement anomaly, and
          // letting one persist turns a wrong log line into a system-wide
          // slowdown.
          if (bake_ms_per_chunk_estimate_ > 4000.0) {
            bake_ms_per_chunk_estimate_ = 4000.0;
          }
        }
      }
      async_fences_[async_ring_index_]->reset();
      // An evict-only submission never calls voxelize_chunk_batch(), so
      // without this reset it would inherit the previous occupant's chunk
      // count and misattribute its (tiny) measured time as a full-chunk
      // sample.
      async_bake_chunks_[async_ring_index_] = 0;
      VulkanCommandBuffer &buf = *async_command_buffers_[async_ring_index_];
      buf.reset();
      buf.begin(/*is_single_use=*/false, /*is_renderpass_continue=*/false,
               /*is_simultaneous_use=*/false);
      async_cmd = &buf;
      if (timestamps_supported_) {
        vkCmdResetQueryPool(buf.handle(), async_timestamp_pool_,
                           async_ring_index_ * 2, 2);
        // BOTTOM_OF_PIPE for the START stamp, deliberately matching the end
        // stamp's stage -- NOT TOP_OF_PIPE. A bottom-of-pipe timestamp as
        // the submission's first command fires when everything BEFORE this
        // submission has finished executing, i.e. exactly when this
        // submission's own work can begin; the delta to the end stamp is
        // then this submission's own execution time regardless of how long
        // it sat queued. The TOP_OF_PIPE stamp it replaces fires when the
        // queue frontend merely REACHES the submission -- which, once
        // several submissions are in flight at once
        // (kMaxDrainSubmissionsPerFrame, kAsyncRingDepth), includes all the
        // time spent waiting on the predecessor's chain semaphore. That
        // queue-wait inflation fed straight into the cost estimate and ran
        // it away to seconds (live-confirmed under the old sliced scheme:
        // est=1000-3400ms for ~200ms chunks), collapsing streaming
        // throughput -- a runaway loop with the measurement itself as the
        // amplifier. The hazard applies unchanged to the per-item estimate.
        vkCmdWriteTimestamp(buf.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                           async_timestamp_pool_, async_ring_index_ * 2);
        async_bake_recorded_[async_ring_index_] = true;
      }
    }
    return *async_cmd;
  };

  // Boundary evictions for one level, shared by the ordinary planning pass
  // below and the bake-drain branch. Capped by the BATCH's capacity only:
  // an eviction is a near-free free-list push, and throttling it by any
  // bake-time notion meant slots stopped recycling exactly when streaming
  // was busiest -- confirmed live (under the old sliced scheme) as the
  // "missing chunks" report: planning frames were rare AND each one evicted
  // a single chunk across all five levels, so the free pool starved,
  // acquire_free_slot() returned kInvalidChunkSlot every frame, and loads
  // simply stopped while the camera sat surrounded by unbaked chunks.
  auto collect_boundary_evictions = [&](u32 level,
                                        ChunkStreamingManager &streaming,
                                        const std::vector<ChunkKey> &to_evict,
                                        std::vector<i32> &evict_slots) {
    u32 evicted = 0;
    for (ChunkKey key : to_evict) {
      if (evicted >= kMaxChunkBakesPerFrame ||
          evict_slots.size() >= kMaxChunkBatchSize) {
        break;
      }
      ChunkState state = streaming.state_of(key);
      if (state != ChunkState::Ready) {
        // Raced with tick()/update() in between planning and acting --
        // defensive, not expected in practice.
        continue;
      }
      u32 slot = streaming.resident_chunks().at(key).gpu_slot;
      glm::ivec3 chunk_coord(static_cast<i32>(key.cx),
                             static_cast<i32>(key.cy),
                             static_cast<i32>(key.cz));
      // Boundary eviction -- this slot IS going back to the free pool, so
      // the table entry must be cleared (see evict_chunk()'s own comment).
      write_chunk_table_entry(chunk_coord, level, -1);
      set_chunk_slot_published(slot, false);
      // The evict dispatch frees this slot's bricks, so anything the alias
      // cache remembers about them stops being true right here.
      invalidate_slot_content(slot);
      evict_slots.push_back(static_cast<i32>(slot));
      streaming.commit_evict(key);
      ++evicted;
    }
  };

  // Slots whose double-buffered replacement has been published still hold
  // their old bricks and cluster pages -- freed via the same evict batch
  // as ordinary evictions. Only as many as actually fit; erasing a fixed
  // count instead would drop the overflow on the floor, leaking the slot
  // and every brick and cluster page it still owns.
  auto drain_slot_retirements = [&](std::vector<i32> &evict_slots) {
    size_t consumed = 0;
    while (consumed < pending_slot_retirements_.size() &&
           evict_slots.size() < kMaxChunkBatchSize &&
           consumed < kMaxChunksPerSubmission) {
      invalidate_slot_content(pending_slot_retirements_[consumed]);
      evict_slots.push_back(
          static_cast<i32>(pending_slot_retirements_[consumed]));
      ++consumed;
    }
    pending_slot_retirements_.erase(pending_slot_retirements_.begin(),
                                    pending_slot_retirements_.begin() +
                                        static_cast<ptrdiff_t>(consumed));
  };

  // A scene edit (renderer_load_scene()/translate_scene()/etc. -- see
  // GeometrySystem::mark_dirty()'s own comment, which names this exact
  // spot as the "later phase" meant to read dirty_since_last_snapshot())
  // invalidates every already-Ready resident chunk's baked content:
  // voxelize_chunk() only ever runs when a chunk newly enters a level's
  // streaming window below, never again just because the primitives inside
  // an already-resident chunk changed. Without this, a scene edited in
  // place while the camera stays put (sdf_editor's entire workflow -- see
  // SceneViewport::tick()'s per-frame renderer_set_camera() plus
  // sync_viewport_scene()'s clear+reload on every edit) would show
  // permanently stale -- usually empty, from before any primitive existed
  // -- content forever, since nothing ever asks an already-Ready chunk to
  // re-bake. testbed/games/SH never hit this: their scenes load once and
  // are never edited again.
  //
  // Fixed by capturing every currently-Ready chunk, across every level, as
  // pending_forced_evictions_ the frame a dirty primitive is first noticed
  // (a one-shot sweep, not a live re-check every frame -- re-checking
  // dirty_since_last_snapshot() every frame would keep re-evicting chunks
  // an earlier sweep had already refreshed, forever, since clearing it
  // immediately after the sweep is what lets already-fresh reloads stay
  // put). Each forced eviction drains through the exact same ring-delay-
  // safe evict_chunk()/commit_evict() path as an ordinary boundary
  // eviction below (see its own comment for why that delay matters
  // regardless of the reason a chunk is being evicted), so a freed chunk
  // simply gets reloaded fresh a few frames later by the ordinary to_load
  // logic below -- the camera is still near it, so update() will want it
  // resident again immediately, this time reading the current scene.
  //
  // Brute-force (every resident chunk, not just ones a dirty primitive's
  // own chunks_touched_by() would name) is still the fallback for most
  // dirty reasons: a REMOVED primitive is already gone from GeometrySystem
  // by the time this runs (release() already erased it), leaving no bounds
  // to compute which chunks it used to occupy, and a MOVED primitive's old
  // position is equally unrecoverable here. Re-baking every resident chunk
  // near the camera costs at most kMaxResidentChunks*kNumLevels evictions
  // spread over kMaxChunkBakesPerFrame-sized budgets across a handful of
  // frames -- nowhere near the cost of the old fixed-cube rebake() this
  // replaces.
  //
  // Two dirty reasons are fully surgical -- for both, EVERY position this
  // primitive could have baked content at (across this whole cycle) is
  // knowable, so chunks_touched_by() can name exactly which chunks need
  // re-baking instead of sweeping every resident one:
  //   - A BRAND NEW primitive (GeometrySystem::newly_added_since_last_
  //     snapshot()) never existed before this cycle at all (not even under
  //     a different shape/position that got released and replaced -- see
  //     any_released_since_last_snapshot()'s own comment), so only its
  //     CURRENT bounds matter.
  //   - An EXISTING primitive edited in place with NOTHING about its own
  //     LAYER changing (GeometrySystem::dirty_previous_state() -- see its
  //     own comment for exactly why layer-driven dirtying is excluded)
  //     has both a captured OLD snapshot and its CURRENT (post-edit)
  //     state, so the UNION of both bounds' chunks_touched_by() covers
  //     everywhere it used to be baked and everywhere it needs to be now.
  // Eligible only when EVERY dirty name this cycle falls into one of these
  // two buckets and nothing was released -- a single primitive dirtied for
  // any OTHER reason (removed, a layer-wide operation/smoothness or
  // reassignment change, or mutated through some other in-place path like
  // translate_scene()/rotate_scene()/scale_scene()/shift_all(), none of
  // which populate dirty_previous_state()) still forces the full sweep
  // below, matching this method's own longstanding "when in doubt, sweep
  // everything" policy for anything that isn't provably safe. An unbounded
  // primitive on EITHER side (a Plane, infinite repetition, or any live
  // parametric-attribute formula -- see geometry_bounding_radius())
  // disqualifies the whole cycle too: chunks_touched_by() returns empty
  // for those (not "every chunk"), so treating that as "nothing to evict"
  // would silently leave it unbaked wherever it's actually visible.
  GeometrySystem &geo = *context_->geometry_system;
  if (!geo.dirty_since_last_snapshot().empty()) {
    bool surgical = !geo.any_released_since_last_snapshot();
    struct SurgicalEntry {
      Geometry *current;
      const Geometry *previous; // nullptr for a brand-new primitive
    };
    std::vector<SurgicalEntry> surgical_entries;
    if (surgical) {
      surgical_entries.reserve(geo.dirty_since_last_snapshot().size());
      for (const std::string &name : geo.dirty_since_last_snapshot()) {
        Geometry *geometry = geo.find(name);
        if (!geometry) {
          surgical = false; // defensive -- shouldn't happen, a removed
                            // name's release() already set any_released_
                            // since_last_snapshot() above.
          break;
        }
        const Geometry *previous = nullptr;
        if (!geo.newly_added_since_last_snapshot().count(name)) {
          auto it = geo.dirty_previous_state().find(name);
          if (it == geo.dirty_previous_state().end()) {
            surgical = false; // dirtied for an untracked reason
            break;
          }
          previous = &it->second;
        }
        if (geometry_bounding_radius(*geometry) >= kUnboundedBoundingRadius ||
            (previous &&
             geometry_bounding_radius(*previous) >= kUnboundedBoundingRadius)) {
          surgical = false;
          break;
        }
        surgical_entries.push_back({geometry, previous});
      }
    }

    if (surgical) {
      std::unordered_set<ChunkKey, ChunkKeyHash> already_queued;
      for (u32 level = 0; level < kNumLevels; ++level) {
        f32 level_world_size = chunk_level_world_size(level);
        ChunkStreamingManager &streaming = chunk_streaming_levels_[level];
        for (const SurgicalEntry &entry : surgical_entries) {
          // Skip queueing THIS level's touched chunks for a primitive too
          // small to meaningfully change what that level's much coarser
          // voxels resolve to -- see primitive_matters_at_level()'s own
          // comment. Checked separately for current/previous: an entry
          // dirtied by a size change (e.g. a radius edit) can matter at
          // one but not the other.
          // Queue Ready AND Baking chunks alike -- a chunk still Baking
          // right now had its voxelize_chunk() dispatch recorded (and,
          // depending on exactly when this dirty cycle landed relative to
          // that dispatch, possibly already executed on the GPU) against
          // the PRE-edit primitive_buffer_/layer_buffer_, so it's just as
          // stale as a Ready one once it settles -- only its ring-delay
          // hasn't caught up yet. Excluding it here (Ready-only, the
          // original bug) meant it would tick() its way to Ready holding
          // pre-edit content with nothing left in dirty_since_last_
          // snapshot() (cleared unconditionally below) to ever re-trigger
          // its rebake -- it would then stay stale until camera motion
          // evicted it by boundary. The drain loop further down is what
          // actually defers a still-Baking entry rather than dropping it;
          // this producer only needs to widen which states get queued.
          //
          // Gated on pending_forced_eviction_keys_ (persistent ACROSS
          // dirty cycles), not just already_queued (this CALL's own
          // dedup) -- a rapid sequence of edits (a gizmo drag firing one
          // dirty cycle per tick) can easily land a NEW cycle before the
          // PREVIOUS one's forced-rebakes finished draining, especially
          // for a still-Baking chunk deferred rather than dropped. Without
          // this, every such tick would re-queue the SAME chunk again on
          // top of its still-pending earlier entry -- confirmed live as
          // an ever-growing backlog (hundreds of duplicate entries within
          // seconds of dragging) that the per-frame drain budget can never
          // catch up to, since new duplicates keep outpacing it: each
          // duplicate still costs a real evict+voxelize dispatch when its
          // turn comes, for a chunk that's already going to be (or already
          // was) correctly rebaked by an earlier entry for the same key.
          if (primitive_matters_at_level(geometry_bounding_radius(*entry.current),
                                         level)) {
            for (ChunkKey key : chunks_touched_by(*entry.current, level_world_size,
                                                 max_smoothness_)) {
              ChunkState state = streaming.state_of(key);
              if ((state == ChunkState::Ready || state == ChunkState::Baking) &&
                  already_queued.insert(key).second &&
                  pending_forced_eviction_keys_.emplace(level, key).second) {
                pending_forced_evictions_.emplace_back(level, key);
              }
            }
          }
          if (entry.previous &&
              primitive_matters_at_level(geometry_bounding_radius(*entry.previous),
                                         level)) {
            for (ChunkKey key :
                chunks_touched_by(*entry.previous, level_world_size,
                                  max_smoothness_)) {
              ChunkState state = streaming.state_of(key);
              if ((state == ChunkState::Ready || state == ChunkState::Baking) &&
                  already_queued.insert(key).second &&
                  pending_forced_eviction_keys_.emplace(level, key).second) {
                pending_forced_evictions_.emplace_back(level, key);
              }
            }
          }
        }
        already_queued.clear();
      }
    } else {
      for (u32 level = 0; level < kNumLevels; ++level) {
        for (const auto &[key, record] : chunk_streaming_levels_[level].resident_chunks()) {
          // See the surgical branch's identical Ready-or-Baking and
          // pending_forced_eviction_keys_ comments above -- same
          // reasoning applies to the brute-force sweep.
          if ((record.state == ChunkState::Ready || record.state == ChunkState::Baking) &&
              pending_forced_eviction_keys_.emplace(level, key).second) {
            pending_forced_evictions_.emplace_back(level, key);
          }
        }
      }
    }
    // A scene edit invalidates the GI cascade's baked indirect-bounce term
    // exactly as much as it invalidates chunk content -- see gi_cascade_
    // dirty_'s own comment for why update_gi_cascade() (called right after
    // this, same frame, by begin_frame()) needs this signal to rebake even
    // when the camera itself hasn't moved.
    gi_cascade_dirty_ = true;
    geo.clear_dirty();
  }

  // One independent PLANNING pass per clip level -- see chunk_streaming_
  // levels_'s own comment. Each level has its own budget (kMaxChunkBakes
  // PerFrame/kMaxForcedRebakesPerFrame), so total per-frame chunk work
  // scales with kNumLevels; still far cheaper than one full rebake either
  // way. Phase 6: unlike the CPU-side scheduling decisions here (ChunkStreamingManager,
  // completely unchanged), the actual GPU work is no longer recorded per
  // chunk as this loop finds it -- every level's evictions/voxelizations
  // are collected into these two flat, cross-level lists first, and
  // recorded as exactly one evict_chunk_batch() + one voxelize_chunk_
  // batch() call after the loop, instead of dozens of individual evict_
  // chunk()/voxelize_chunk() dispatch+barrier pairs. See kMaxChunkBatchSize's
  // own comment for why the worst-case total across every level's budget
  // still fits.
  std::vector<i32> evict_batch_slots;
  std::vector<GpuChunkBatchEntry> voxelize_batch_entries;

  for (u32 level = 0; level < kNumLevels; ++level) {
    ChunkStreamingManager &streaming = chunk_streaming_levels_[level];
    f32 level_world_size = chunk_level_world_size(level);
    f32 level_cell_size = level_world_size / static_cast<f32>(kChunkCoarseDim);
    auto chunk_world_min_of = [&](ChunkKey key) {
      glm::ivec3 chunk_coord(static_cast<i32>(key.cx), static_cast<i32>(key.cy),
                             static_cast<i32>(key.cz));
      return glm::vec3(chunk_coord) * level_world_size;
    };

    streaming.tick();
    // Plan against a point AHEAD of the camera rather than the camera
    // itself -- see kStreamLeadFrames. The lead is capped per level in
    // units of that level's own chunk, so a coarse level (whose chunks are
    // 16x wider) leads proportionally further in world units while every
    // level keeps the same guarantee that the camera's own chunk stays
    // inside the window.
    //
    // update() uses this position twice: to choose the window, and to sort
    // to_load nearest-first. The second is arguably the bigger win --
    // with a per-frame budget far smaller than a boundary crossing's worth
    // of chunks, WHICH chunks get baked first decides whether the viewer
    // sees the frontier or not.
    glm::vec3 lead = stream_velocity_ * kStreamLeadFrames;
    const f32 lead_cap = level_world_size * kMaxStreamLeadChunks;
    const f32 lead_len = glm::length(lead);
    if (lead_len > lead_cap) {
      lead *= lead_cap / lead_len;
    }
    ChunkStreamingManager::Plan plan =
        streaming.update(camera_pos + lead, level_world_size);

    // tick() still runs on a frame that can't queue -- the ring-delays it
    // ages are measured in frames, not in submissions -- but nothing below
    // may record GPU work. See ring_available's own comment.
    if (!ring_available) {
      continue;
    }

    // Boundary evictions -- shared with the bake-drain branch; see
    // collect_boundary_evictions above for why they're capped by batch
    // capacity, never by any bake-time budget.
    collect_boundary_evictions(level, streaming, plan.to_evict,
                               evict_batch_slots);

    // Drain this level's share of pending_forced_evictions_ under its OWN
    // budget (kMaxForcedRebakesPerFrame) -- see that constant's own
    // comment for why a dirty-edit-triggered burst deserves more per-
    // frame room than continuous camera-driven boundary streaming.
    //
    // Re-baked IN PLACE, straight back into the same gpu_slot, rather than
    // routed through the boundary path's evict+commit_evict()+(eventually,
    // a future frame's) acquire_free_slot()+voxelize round trip: a dirty-
    // triggered chunk is, unlike a boundary one, still exactly where the
    // camera wants it -- nothing will claim its slot for anything else --
    // so ring-delaying it through the free list only bought several
    // frames of the chunk (and anything else sharing it) sampling as
    // flat-out empty, since sample_chunked_field() trusts a -1 table
    // entry completely (see evict_chunk()'s header comment) and has no
    // "still show the old content" fallback. Live-tested as the actual
    // "moving one ordinary primitive flickers the scene" report -- for a
    // large enough primitive (or a coarse level whose one touched chunk
    // covers most of a small scene) that empty window reads as the whole
    // view blanking out, not just the edited primitive. The table entry
    // is deliberately left untouched here (still pointing at slot, the
    // OLD bake, valid the whole time) -- it doesn't need re-writing since
    // the re-bake reuses the exact same slot the table already names.
    u32 forced_rebaked = 0;
    // Bounded against voxelize_batch_entries, not evict_batch_slots: a
    // forced rebake now costs a bake into a fresh slot and no eviction at
    // all this frame (the slot it replaces is retired later, once the new
    // bake is published), so the batch it can overflow is the voxelize one.
    for (auto it = pending_forced_evictions_.begin();
        it != pending_forced_evictions_.end() &&
        forced_rebaked < kMaxForcedRebakesPerFrame &&
        voxelize_batch_entries.size() < kMaxChunksPerSubmission; ) {
      if (it->first != level) {
        ++it;
        continue;
      }
      ChunkKey key = it->second;
      ChunkState state = streaming.state_of(key);
      if (state == ChunkState::Baking) {
        // Still mid-flight from a bake dispatched before this dirty cycle
        // landed -- its slot may still have a GPU dispatch in flight
        // against it, so it isn't safe to evict+re-voxelize into that same
        // slot yet (see evict_chunk_batch()'s own ring-delay reasoning).
        // Leave the entry queued (don't erase) and retry it on a later
        // frame, once tick() carries it from Baking to Ready -- dropping
        // it here (the original bug) is what let a chunk settle into
        // Ready holding pre-edit content with nothing left to ever
        // re-trigger its rebake.
        ++it;
        continue;
      }
      it = pending_forced_evictions_.erase(it);
      // Keep pending_forced_eviction_keys_ in lockstep with the vector --
      // see its own comment for why: as soon as this entry leaves the
      // vector (regardless of whether it's about to actually be
      // rebaked or just discarded below), a FUTURE dirty cycle touching
      // this same chunk must be free to queue it again.
      pending_forced_eviction_keys_.erase({level, key});
      if (state != ChunkState::Ready) {
        // Already handled by the boundary pass above this same frame (it
        // can appear in both plan.to_evict and here), or otherwise no
        // longer resident -- nothing left to do for this entry.
        continue;
      }
      const u32 old_slot = streaming.resident_chunks().at(key).gpu_slot;
      // DOUBLE-BUFFERED, not in place. The rebake goes into a FRESH slot
      // while the old one keeps serving the renderer untouched, and the
      // table only flips to the new slot once its fence confirms the bake
      // finished (see publish_completed_bakes()); the old slot is then
      // retired and its bricks freed.
      //
      // This replaces "evict the slot, then re-voxelize into it in the same
      // batch", which was itself a fix for the eviction FLICKER -- routing a
      // dirty chunk through the ordinary evict+reload path left it sampling
      // as flat empty for several frames. Rebaking in place fixed the
      // flicker but made the stall unavoidable: the slot the renderer reads
      // is the slot the bake writes, so the only thing standing between a
      // frame and a half-written chunk was the graphics queue waiting for
      // the bake. Two slots buy both properties at once -- no empty window,
      // and no reason for the frame to wait.
      u32 new_slot = streaming.acquire_free_slot();
      if (new_slot == kInvalidChunkSlot) {
        // Nothing free this frame. Re-queue rather than drop: the entry was
        // already erased from pending_forced_evictions_ above, and losing it
        // here would leave the chunk holding pre-edit content with nothing
        // left to re-trigger it (the exact bug the Baking case guards).
        pending_forced_evictions_.emplace_back(level, key);
        pending_forced_eviction_keys_.emplace(level, key);
        continue;
      }
      ++forced_rebaked;
      glm::vec3 chunk_world_min = chunk_world_min_of(key);
      invalidate_slot_content(new_slot);
      voxelize_batch_entries.push_back(
          {static_cast<i32>(new_slot), chunk_world_min.x, chunk_world_min.y,
           chunk_world_min.z, level_cell_size});
      streaming.commit_load(key, new_slot); // record moves to the new slot;
                                            // the table still names the old
                                            // one until publication.
      queued_publishes.push_back(
          {level, key, new_slot, old_slot,
           streaming.resident_chunks().at(key).bake_generation});
    }

    u32 loaded = 0;
    for (ChunkKey key : plan.to_load) {
      if (loaded >= kMaxChunkBakesPerFrame ||
          voxelize_batch_entries.size() >= kMaxChunksPerSubmission) {
        break;
      }
      u32 slot = streaming.acquire_free_slot();
      if (slot == kInvalidChunkSlot) {
        // No slot free this frame (every slot resident or still ring-
        // delaying an eviction) -- try again next frame; update() will
        // re-offer this same chunk (still nearest-first) as long as it's
        // still wanted.
        break;
      }
      glm::ivec3 chunk_coord(static_cast<i32>(key.cx), static_cast<i32>(key.cy),
                             static_cast<i32>(key.cz));
      glm::vec3 chunk_world_min = glm::vec3(chunk_coord) * level_world_size;
      invalidate_slot_content(slot);
      voxelize_batch_entries.push_back(
          {static_cast<i32>(slot), chunk_world_min.x, chunk_world_min.y,
           chunk_world_min.z, level_cell_size});
      streaming.commit_load(key, slot);
      // NO write_chunk_table_entry() here. Naming the slot before the bake
      // has run is what forced the graphics queue to wait on it -- the table
      // would otherwise point every sampler straight at a chunk being
      // written. Deferred to publish_completed_bakes(); until then this
      // chunk simply reads as not resident, which for a newly streamed-in
      // chunk is exactly what it was a moment ago anyway.
      queued_publishes.push_back(
          {level, key, slot, kInvalidChunkSlot,
           streaming.resident_chunks().at(key).bake_generation});
      ++loaded;
    }
  }

  // Exactly two dispatches for the WHOLE frame (down from one pair per
  // chunk) -- evict_chunk_batch()'s default insert_barrier=true makes its
  // free-list push visible before voxelize_chunk_batch() reads it right
  // after, exactly the ordering the forced-rebake entries above need
  // (their evict and re-voxelize share a slot's bricks). Boundary to_load
  // entries don't strictly need to wait on this frame's own evict batch
  // (their slot's bricks were already freed frames ago), but folding them
  // into the same voxelize dispatch anyway is harmless -- one dispatch
  // for everything is simpler than splitting hairs over which entries
  // truly need the barrier.
  // Slots whose double-buffered replacement has now been published still
  // hold their old bricks and cluster pages -- free them here, in the same
  // batch as every ordinary eviction. Deferred to this point rather than
  // done inside publish_completed_bakes() because an eviction is GPU work
  // and that function records none.
  if (ring_available) {
    drain_slot_retirements(evict_batch_slots);
  }

  if (!evict_batch_slots.empty() || !voxelize_batch_entries.empty()) {
    // The evict dispatch below frees these slots' bricks BEFORE the
    // voxelize dispatch runs, so the alias cache must not offer any of them
    // as a copy source this frame -- see build_cell_alias_map()'s
    // entry_usable(). Their generations have already been bumped, which
    // covers every LATER frame; this set covers this one.
    std::unordered_set<u32> evicting_slots;
    evicting_slots.reserve(evict_batch_slots.size());
    for (i32 slot : evict_batch_slots) {
      evicting_slots.insert(static_cast<u32>(slot));
    }
    evict_chunk_batch(ensure_async_cmd(), evict_batch_slots);
    voxelize_chunk_batch(ensure_async_cmd(), voxelize_batch_entries,
                         static_cast<u32>(layer_count_), max_smoothness_,
                         evicting_slots);
  }

  submit_async_chunk_work(async_cmd, queued_publishes);
}

// Ends and submits whatever update_streaming() recorded this frame, and
// hands its chunks to the ring slot whose fence will report them finished.
void VulkanRaymarchShader::submit_async_chunk_work(
    VulkanCommandBuffer *async_cmd,
    std::vector<PendingChunkPublish> &queued_publishes) {
  if (!async_cmd) {
    // Nothing recorded this call -- no submission. Anything queued_publishes
    // collected can only have come from work that WAS recorded, so it is
    // necessarily empty here.
    return;
  }
  if (timestamps_supported_) {
    vkCmdWriteTimestamp(async_cmd->handle(),
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                       async_timestamp_pool_, async_ring_index_ * 2 + 1);
  }
  async_cmd->end();

  // ONE signal: async_chain_semaphores_[.], for whichever FUTURE update_
  // streaming() call submits next.
  //
  // There used to be a second, async_chunk_ready_semaphores_[.], for this
  // frame's graphics submission to wait on. That wait is gone (see this
  // function's header comment), and leaving the semaphore signalled anyway
  // is not harmless: a binary semaphore signalled twice with no wait in
  // between is invalid, and the validation layer flags every submission
  // after the first. Signalling only what is actually waited on is the
  // whole fix.
  VkSemaphore chain_signal_semaphore = async_chain_semaphores_[async_ring_index_];
  VkSemaphore signal_semaphores[1] = {chain_signal_semaphore};
  VkCommandBuffer raw_handle = async_cmd->handle();
  VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &raw_handle;
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = signal_semaphores;
  // Two waits, both GPU-side:
  //   - graphics_timeline at whatever value the PREVIOUS frame's graphics
  //     submission signalled: this submission's writes must not begin until
  //     that frame's reads of the same buffers are done (the write-after-read
  //     hazard that used to be a vkQueueWaitIdle on the CPU).
  //   - the previous async submission's chain semaphore, ordering bakes
  //     against each other.
  // A timeline wait on an already-reached value costs nothing, so the first
  // one is free once streaming has caught up with the camera.
  VkSemaphore wait_semaphores[2];
  VkPipelineStageFlags wait_stages[2];
  uint64_t wait_values[2];
  u32 wait_count = 0;
  if (context_->graphics_timeline != VK_NULL_HANDLE) {
    wait_semaphores[wait_count] = context_->graphics_timeline;
    wait_stages[wait_count] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    wait_values[wait_count] = context_->graphics_timeline_value;
    ++wait_count;
  }
  if (pending_async_chain_semaphore_ != VK_NULL_HANDLE) {
    wait_semaphores[wait_count] = pending_async_chain_semaphore_;
    wait_stages[wait_count] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    wait_values[wait_count] = 0; // binary -- the value is ignored
    ++wait_count;
  }
  const uint64_t signal_values[1] = {0}; // the chain signal is binary
  VkTimelineSemaphoreSubmitInfo timeline_submit{
      VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  timeline_submit.waitSemaphoreValueCount = wait_count;
  timeline_submit.pWaitSemaphoreValues = wait_values;
  timeline_submit.signalSemaphoreValueCount = 1;
  timeline_submit.pSignalSemaphoreValues = signal_values;
  if (wait_count > 0) {
    submit_info.waitSemaphoreCount = wait_count;
    submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages;
  }
  submit_info.pNext = &timeline_submit;
  VK_CHECK(vkQueueSubmit(context_->device.async_compute_queue, 1, &submit_info,
                        async_fences_[async_ring_index_]->handle()));
  async_cmd->update_submitted();

  // Hand this submission's chunks to the slot that will report their
  // completion. Must happen before async_ring_index_ advances, and after
  // the submit -- the fence is only meaningful once there is work behind it.
  async_pending_publish_[async_ring_index_] = std::move(queued_publishes);

  pending_async_chain_semaphore_ = chain_signal_semaphore;
  async_ring_index_ = (async_ring_index_ + 1) % kAsyncRingDepth;
}

void VulkanRaymarchShader::debug_probe_field(glm::vec3 world_pos,
                                            glm::vec3 camera_pos) {
  if (!valid_) {
    return;
  }
  // The field is written on the async queue and read here on the graphics
  // one, with no semaphore between them -- a full device wait is the honest
  // way to be sure this sees finished work rather than a race (the real
  // render path never needs it; see update_streaming()'s callers).
  vkDeviceWaitIdle(context_->device.logical_device);

  // Two queries at the same point: the single-level lookup the bake writes,
  // and the clipmap lookup the renderer actually samples. They disagree
  // exactly when the wrong clip level is answering, which is worth seeing.
  auto run = [&](bool clipmap) -> ChunkDebugQueryOutput {
    auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
        *context_, context_->device.graphics_command_pool);
    if (clipmap) {
      query_clipmap_field(*cmd, world_pos, camera_pos);
    } else {
      query_chunked_field(*cmd, world_pos);
    }
    VulkanCommandBuffer::end_single_use(*context_,
                                        context_->device.graphics_command_pool,
                                        std::move(cmd),
                                        context_->device.graphics_queue);
    ChunkDebugQueryOutput result{};
    if (void *mapped = chunk_debug_query_output_buffer_->lock(
            0, sizeof(ChunkDebugQueryOutput), 0)) {
      result = *static_cast<ChunkDebugQueryOutput *>(mapped);
      chunk_debug_query_output_buffer_->unlock();
    }
    return result;
  };

  const ChunkDebugQueryOutput level0 = run(false);
  const ChunkDebugQueryOutput clip = run(true);

  // skip_dist != 0 means "no brick here, dist is a placeholder" -- i.e. the
  // field believes this point is empty space. dist < 0 means solid.
  KINFO("FIELD PROBE at ({:.3f}, {:.3f}, {:.3f}):", world_pos.x, world_pos.y,
       world_pos.z);
  KINFO("  level 0 : dist={:.4f} skip={:.4f} material={} -> {}", level0.dist,
       level0.skip_dist, level0.material_index,
       level0.skip_dist != 0.0f ? "EMPTY (no brick)"
                                : (level0.dist < 0.0f ? "SOLID" : "outside surface"));
  KINFO("  clipmap : dist={:.4f} skip={:.4f} material={} -> {}", clip.dist,
       clip.skip_dist, clip.material_index,
       clip.skip_dist != 0.0f ? "EMPTY (no brick)"
                              : (clip.dist < 0.0f ? "SOLID" : "outside surface"));
  KINFO("  If this reads EMPTY/outside but the pixel renders solid, the bake "
       "is fine and the splat points are stale. If it reads SOLID inside a "
       "subtracted cavity, the bake itself is wrong.");
}

void VulkanRaymarchShader::debug_verify_chunked_field() {
  // Hand-place one test primitive directly into primitive_buffer_/
  // layer_buffer_ -- bypassing GeometrySystem/rebuild_static_scene()
  // entirely, since this runs from the constructor before a game has
  // necessarily registered anything. A sphere, radius 1, centered at
  // world-space (2.1, 2.1, 2.1) -- deliberately not grid-aligned (0.25-unit
  // coarse cells, so a plain 2.0 would put the surface's most interesting
  // query points suspiciously close to a cell boundary -- see the query
  // points below for why exact alignment would make this test's own
  // reasoning about which cell contains a query point ambiguous) and not
  // exactly 0 on any axis (a primitive at the render-space origin would
  // straddle 8 chunks at once here, an unnecessary complication for a
  // single-chunk test; see GeometrySystem's chunks_touched_by() unit
  // tests, tests/src/systems/geometry_chunk_tests.cpp, for the same
  // reasoning applied to chunk *selection* rather than voxelization).
  GpuPrimitive test_primitive{};
  test_primitive.position_type[0] = 2.1f;
  test_primitive.position_type[1] = 2.1f;
  test_primitive.position_type[2] = 2.1f;
  test_primitive.position_type[3] =
      static_cast<f32>(static_cast<u32>(PrimitiveType::Sphere));
  test_primitive.params[0] = 1.0f; // radius
  test_primitive.rotation[3] = 1.0f; // identity quaternion (0,0,0,1)
  test_primitive.expr_scale[0] = 1.0f;
  test_primitive.repeat_count[0] = 1.0f;
  test_primitive.repeat_count[1] = 1.0f;
  test_primitive.repeat_count[2] = 1.0f;
  test_primitive.repeat_count[3] = 1.0f; // bounding radius (exact, for a plain sphere)
  test_primitive.deform[3] = 20.0f; // displace_frequency default; amplitude 0 so unused

  GpuLayer test_layer{};
  test_layer.op_smoothness[0] =
      static_cast<f32>(static_cast<u32>(LayerOperation::Union));
  test_layer.op_smoothness[1] = 0.0f;
  test_layer.range[0] = 0; // primitive_start
  test_layer.range[1] = 1; // primitive_count

  // instruction_count == 0 means "no formula, use the plain constant" --
  // see GpuParamExpr's own comment. primitive_buffer_/layer_buffer_'s
  // memory isn't guaranteed zeroed on creation, so this must be written
  // explicitly rather than assumed.
  GpuParamExpr empty_expr{};
  if (void *mapped = param_expr_buffer_->lock(0, sizeof(GpuParamExpr) * 4, 0)) {
    auto *exprs = static_cast<GpuParamExpr *>(mapped);
    exprs[0] = empty_expr;
    exprs[1] = empty_expr;
    exprs[2] = empty_expr;
    exprs[3] = empty_expr;
    param_expr_buffer_->unlock();
  }
  if (void *mapped = primitive_buffer_->lock(0, sizeof(GpuPrimitive), 0)) {
    *static_cast<GpuPrimitive *>(mapped) = test_primitive;
    primitive_buffer_->unlock();
  }
  if (void *mapped = layer_buffer_->lock(0, sizeof(GpuLayer), 0)) {
    *static_cast<GpuLayer *>(mapped) = test_layer;
    layer_buffer_->unlock();
  }

  // The sphere (center (2.1,2.1,2.1), radius 1) sits entirely inside the
  // level-0 chunk at world_chunk_coord (0,0,0) -- kChunkWorldSize is 4.0
  // (16 * 0.25), so that chunk spans [0,4) on every axis, comfortably
  // containing the sphere's full [1.1,3.1] extent on each axis with room
  // to spare.
  glm::ivec3 chunk_coord(0, 0, 0);
  u32 slot = 0;

  auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);
  voxelize_chunk(*cmd, chunk_coord, /*level=*/0, slot, /*layer_count=*/1,
                /*max_smoothness=*/0.0f);
  // First query: a point exactly ON the sphere's surface, +X of center
  // (2.1+1.0, 2.1, 2.1) = (3.1, 2.1, 2.1) -- expected dist == 0.0.
  // Deliberately ON the surface rather than merely "near" it: only coarse
  // cells within half_diagonal (~0.2165, see Builtin.ChunkVoxelize.comp.
  // glsl's CULL_RADIUS_CELLS-independent coarse test) of the true surface
  // get a brick allocated at all, and a cell's *center* -- not the query
  // point -- is what that test measures. For a point at true surface
  // distance d, its containing cell's center can be up to half_diagonal
  // away, so by the SDF's 1-Lipschitz property the cell center's own
  // distance can be as large as d + half_diagonal -- which only stays
  // within the half_diagonal cull threshold FOR EVERY POSSIBLE grid
  // alignment when d == 0 exactly. A nonzero-but-small offset (e.g. 0.1)
  // was tried first and empirically DID land in a cell that failed the
  // cull test (verified by hand against this exact sphere/grid), which is
  // what motivates querying exactly on the surface instead.
  query_chunked_field(*cmd, glm::vec3(3.1f, 2.1f, 2.1f));
  // (Sequential single-invocation dispatches into the same command buffer
  // would all write the same debug-output binding before the previous
  // result is read back -- so this reads back after each dispatch
  // individually instead, at the cost of three submissions instead of
  // one. Verification-only code; not a pattern for the real render path.)
  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(cmd), context_->device.graphics_queue);

  auto read_result = [this]() -> ChunkDebugQueryOutput {
    ChunkDebugQueryOutput result{};
    if (void *mapped = chunk_debug_query_output_buffer_->lock(
            0, sizeof(ChunkDebugQueryOutput), 0)) {
      result = *static_cast<ChunkDebugQueryOutput *>(mapped);
      chunk_debug_query_output_buffer_->unlock();
    }
    return result;
  };

  bool all_passed = true;
  auto check = [&](const char *name, f32 actual, f32 expected, f32 tolerance) {
    if (std::abs(actual - expected) > tolerance) {
      KERROR("Phase 3a chunked-field verification FAILED: {} expected {}, "
            "got {}.",
            name, expected, actual);
      all_passed = false;
    }
  };

  ChunkDebugQueryOutput surface_x_result = read_result();
  check("point on sphere surface (+X)", surface_x_result.dist, 0.0f, 0.01f);

  // Second query: a different point also exactly ON the surface, -Y of
  // center (2.1, 2.1-1.0, 2.1) = (2.1, 1.1, 2.1) -- same reasoning as
  // above, on a different axis/cell, expected dist == 0.0 again.
  auto cmd2 = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);
  query_chunked_field(*cmd2, glm::vec3(2.1f, 1.1f, 2.1f));
  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(cmd2), context_->device.graphics_queue);
  ChunkDebugQueryOutput surface_y_result = read_result();
  check("point on sphere surface (-Y)", surface_y_result.dist, 0.0f, 0.01f);

  auto cmd3 = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);
  query_chunked_field(*cmd3, glm::vec3(500.0f, 500.0f, 500.0f));
  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(cmd3), context_->device.graphics_queue);
  ChunkDebugQueryOutput unbaked_result = read_result();
  if (unbaked_result.material_index != -1) {
    KERROR("Phase 3a chunked-field verification FAILED: query far outside "
          "any baked chunk expected material_index -1 (no chunk resident), "
          "got {}.",
          unbaked_result.material_index);
    all_passed = false;
  }

  if (all_passed) {
    KINFO("Phase 3a chunked-field verification PASSED (surface +X dist {}, "
         "surface -Y dist {}, unbaked-chunk material_index {}).",
         surface_x_result.dist, surface_y_result.dist,
         unbaked_result.material_index);
  }
}

void VulkanRaymarchShader::debug_verify_multi_level_field() {
  // A sphere far enough along +X that it sits in a level-1 chunk outside
  // level 0's own streaming window (radius kStreamRadiusChunks=1 around a
  // camera at the origin) but inside level 1's (chunk_level_world_size(1)
  // is 2x level 0's, so level 1's window reaches twice as far in world
  // units for the same chunk-count radius -- the entire point of a
  // clipmap). Center (10.1, 2.1, 2.1): non-grid-aligned on both level 0's
  // (cell size 0.25) and level 1's (cell size 0.5) fine-voxel grids, same
  // reasoning as debug_verify_chunked_field()'s sphere placement.
  GpuPrimitive test_primitive{};
  test_primitive.position_type[0] = 10.1f;
  test_primitive.position_type[1] = 2.1f;
  test_primitive.position_type[2] = 2.1f;
  test_primitive.position_type[3] =
      static_cast<f32>(static_cast<u32>(PrimitiveType::Sphere));
  test_primitive.params[0] = 1.0f; // radius
  test_primitive.rotation[3] = 1.0f;
  test_primitive.expr_scale[0] = 1.0f;
  test_primitive.repeat_count[0] = 1.0f;
  test_primitive.repeat_count[1] = 1.0f;
  test_primitive.repeat_count[2] = 1.0f;
  test_primitive.repeat_count[3] = 1.0f;
  test_primitive.deform[3] = 20.0f;

  GpuLayer test_layer{};
  test_layer.op_smoothness[0] =
      static_cast<f32>(static_cast<u32>(LayerOperation::Union));
  test_layer.op_smoothness[1] = 0.0f;
  test_layer.range[0] = 0;
  test_layer.range[1] = 1;

  GpuParamExpr empty_expr{};
  if (void *mapped = param_expr_buffer_->lock(0, sizeof(GpuParamExpr) * 4, 0)) {
    auto *exprs = static_cast<GpuParamExpr *>(mapped);
    exprs[0] = empty_expr;
    exprs[1] = empty_expr;
    exprs[2] = empty_expr;
    exprs[3] = empty_expr;
    param_expr_buffer_->unlock();
  }
  if (void *mapped = primitive_buffer_->lock(0, sizeof(GpuPrimitive), 0)) {
    *static_cast<GpuPrimitive *>(mapped) = test_primitive;
    primitive_buffer_->unlock();
  }
  if (void *mapped = layer_buffer_->lock(0, sizeof(GpuLayer), 0)) {
    *static_cast<GpuLayer *>(mapped) = test_layer;
    layer_buffer_->unlock();
  }

  // chunk_level_world_size(1) = 8.0 -- (10.1, 2.1, 2.1) falls in level-1
  // chunk (1, 0, 0) ([8,16) x [0,8) x [0,8)). Slot chosen directly
  // (kMaxResidentChunks, level 1's first slot) rather than through
  // ChunkStreamingManager -- this test drives voxelize_chunk()/
  // query_clipmap_field() directly, exactly like debug_verify_chunked_
  // field() does for level 0.
  glm::ivec3 chunk_coord(1, 0, 0);
  u32 level = 1;
  u32 slot = kMaxResidentChunks; // level 1's first global slot

  auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);
  voxelize_chunk(*cmd, chunk_coord, level, slot, /*layer_count=*/1,
                /*max_smoothness=*/0.0f);
  // Camera at the origin: level 0's window (chunks -1..1, world roughly
  // [-4,8)) does NOT reach this sphere; level 1's window (chunks -1..1 at
  // 2x the world size, roughly [-8,16)) does -- so sample_clipmap_field()
  // must fall through past level 0 to find this data.
  glm::vec3 camera_pos(0.0f, 0.0f, 0.0f);
  query_clipmap_field(*cmd, glm::vec3(11.1f, 2.1f, 2.1f), camera_pos);
  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(cmd), context_->device.graphics_queue);

  ChunkDebugQueryOutput result{};
  if (void *mapped = chunk_debug_query_output_buffer_->lock(
          0, sizeof(ChunkDebugQueryOutput), 0)) {
    result = *static_cast<ChunkDebugQueryOutput *>(mapped);
    chunk_debug_query_output_buffer_->unlock();
  }

  if (std::abs(result.dist - 0.0f) > 0.02f) {
    KERROR("Phase 4 multi-level verification FAILED: point on level-1 "
          "sphere surface expected dist ~0, got {} (material_index {}) -- "
          "sample_clipmap_field() likely failed to fall through to level 1.",
          result.dist, result.material_index);
  } else {
    KINFO("Phase 4 multi-level verification PASSED (level-1 surface dist "
         "{}, material_index {}).",
         result.dist, result.material_index);
  }
}

void VulkanRaymarchShader::debug_verify_extended_range_field() {
  // Regression test for the kNumLevels=3->5 fix: a sphere placed at x=40.1
  // (camera at the origin) sits at chunk_level_world_size(3)=32's chunk
  // coord (1,0,0), world range [32,64) -- level 3's own streaming window
  // (chunks -1..1, world range roughly [-32,64)) reaches it, but NEITHER
  // level 2's window (chunk_level_world_size(2)=16, world range roughly
  // [-16,32)) NOR any coarser level that existed under the old kNumLevels=3
  // config (levels 0/1/2 only) would have. Confirms the fix actually
  // extends the field's camera-relative reach past ~32 units, not just that
  // levels 3/4 exist and compile.
  GpuPrimitive test_primitive{};
  test_primitive.position_type[0] = 40.1f;
  test_primitive.position_type[1] = 2.1f;
  test_primitive.position_type[2] = 2.1f;
  test_primitive.position_type[3] =
      static_cast<f32>(static_cast<u32>(PrimitiveType::Sphere));
  test_primitive.params[0] = 1.0f;
  test_primitive.rotation[3] = 1.0f;
  test_primitive.expr_scale[0] = 1.0f;
  test_primitive.repeat_count[0] = 1.0f;
  test_primitive.repeat_count[1] = 1.0f;
  test_primitive.repeat_count[2] = 1.0f;
  test_primitive.repeat_count[3] = 1.0f;
  test_primitive.deform[3] = 20.0f;

  GpuLayer test_layer{};
  test_layer.op_smoothness[0] =
      static_cast<f32>(static_cast<u32>(LayerOperation::Union));
  test_layer.op_smoothness[1] = 0.0f;
  test_layer.range[0] = 0;
  test_layer.range[1] = 1;

  GpuParamExpr empty_expr{};
  if (void *mapped = param_expr_buffer_->lock(0, sizeof(GpuParamExpr) * 4, 0)) {
    auto *exprs = static_cast<GpuParamExpr *>(mapped);
    exprs[0] = empty_expr;
    exprs[1] = empty_expr;
    exprs[2] = empty_expr;
    exprs[3] = empty_expr;
    param_expr_buffer_->unlock();
  }
  if (void *mapped = primitive_buffer_->lock(0, sizeof(GpuPrimitive), 0)) {
    *static_cast<GpuPrimitive *>(mapped) = test_primitive;
    primitive_buffer_->unlock();
  }
  if (void *mapped = layer_buffer_->lock(0, sizeof(GpuLayer), 0)) {
    *static_cast<GpuLayer *>(mapped) = test_layer;
    layer_buffer_->unlock();
  }

  glm::ivec3 chunk_coord(1, 0, 0);
  u32 level = 3;
  u32 slot = kMaxResidentChunks * level; // level 3's first global slot

  auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);
  voxelize_chunk(*cmd, chunk_coord, level, slot, /*layer_count=*/1,
                /*max_smoothness=*/0.0f);
  glm::vec3 camera_pos(0.0f, 0.0f, 0.0f);
  query_clipmap_field(*cmd, glm::vec3(41.1f, 2.1f, 2.1f), camera_pos);
  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(cmd), context_->device.graphics_queue);

  ChunkDebugQueryOutput result{};
  if (void *mapped = chunk_debug_query_output_buffer_->lock(
          0, sizeof(ChunkDebugQueryOutput), 0)) {
    result = *static_cast<ChunkDebugQueryOutput *>(mapped);
    chunk_debug_query_output_buffer_->unlock();
  }

  if (std::abs(result.dist - 0.0f) > 0.02f) {
    KERROR("Extended-range verification FAILED: point on level-3 sphere "
          "surface (40 units from camera) expected dist ~0, got {} "
          "(material_index {}) -- sample_clipmap_field() likely failed to "
          "fall through to level 3.",
          result.dist, result.material_index);
  } else {
    KINFO("Extended-range verification PASSED (level-3 surface dist {}, "
         "material_index {}, 40 units from camera -- unreachable under the "
         "old kNumLevels=3 config).",
         result.dist, result.material_index);
  }
}

void VulkanRaymarchShader::debug_verify_dirty_rebake() {
  // Regression test for update_streaming()'s dirty-chunk re-voxelization
  // fix (see its own comment): simulates exactly the bug sdf_editor hit --
  // a chunk becomes resident while empty, then a primitive is added inside
  // it while the camera never moves. Without the fix, an already-Ready
  // chunk is never re-baked just because the scene changed, so this would
  // stay permanently empty; with the fix, the dirty flag force-evicts and
  // reloads it. Each "frame" below is its own single-use command buffer,
  // submitted and waited on synchronously, so ChunkStreamingManager's
  // ring-delay (kFramesInFlightDelay tick()s between commit_load()/commit_
  // evict() and Baking/Evicting resolving) advances exactly like real
  // frames would, without needing a live render loop.
  glm::vec3 camera_pos(0.0f, 0.0f, 0.0f);

  // Drains synchronously after each step -- see drain_streaming_for_debug().
  auto simulate_frame = [&]() {
    update_streaming(camera_pos);
    drain_streaming_for_debug();
  };

  // Phase A: let the level-0 chunk containing (2.1,2.1,2.1) stream in while
  // the scene is empty (layer_count_ defaults to 0, so voxelize_chunk()'s
  // dispatch touches no primitives -- correctly bakes "empty", not garbage).
  // 4 frames: 1 to issue the load (commit_load() stamps bake_generation at
  // the CURRENT generation, before that frame's own tick() has run again),
  // 3 more for kFramesInFlightDelay's age check to pass Baking -> Ready.
  for (int i = 0; i < 4; ++i) {
    simulate_frame();
  }
  if (chunk_streaming_levels_[0].state_of(ChunkKey{0, 0, 0, 0}) != ChunkState::Ready) {
    KERROR("Dirty-rebake verification FAILED: level-0 chunk (0,0,0) never "
          "reached Ready during the initial empty-scene load -- test setup "
          "itself is broken, not the fix under test.");
    return;
  }

  // Phase B: inject a test sphere directly into primitive_buffer_/layer_
  // buffer_ (bypassing GeometrySystem/rebuild_static_scene(), exactly like
  // debug_verify_chunked_field() -- this method runs standalone, not from
  // a real scene load) and mark the scene dirty. mark_dirty()'s name
  // doesn't need to correspond to a real GeometrySystem entry --
  // update_streaming() only checks whether dirty_since_last_snapshot() is
  // non-empty, never what's in it.
  GpuPrimitive test_primitive{};
  test_primitive.position_type[0] = 2.1f;
  test_primitive.position_type[1] = 2.1f;
  test_primitive.position_type[2] = 2.1f;
  test_primitive.position_type[3] =
      static_cast<f32>(static_cast<u32>(PrimitiveType::Sphere));
  test_primitive.params[0] = 1.0f;
  test_primitive.rotation[3] = 1.0f;
  test_primitive.expr_scale[0] = 1.0f;
  test_primitive.repeat_count[0] = 1.0f;
  test_primitive.repeat_count[1] = 1.0f;
  test_primitive.repeat_count[2] = 1.0f;
  test_primitive.repeat_count[3] = 1.0f;
  test_primitive.deform[3] = 20.0f;

  GpuLayer test_layer{};
  test_layer.op_smoothness[0] =
      static_cast<f32>(static_cast<u32>(LayerOperation::Union));
  test_layer.op_smoothness[1] = 0.0f;
  test_layer.range[0] = 0;
  test_layer.range[1] = 1;

  GpuParamExpr empty_expr{};
  if (void *mapped = param_expr_buffer_->lock(0, sizeof(GpuParamExpr) * 4, 0)) {
    auto *exprs = static_cast<GpuParamExpr *>(mapped);
    exprs[0] = empty_expr;
    exprs[1] = empty_expr;
    exprs[2] = empty_expr;
    exprs[3] = empty_expr;
    param_expr_buffer_->unlock();
  }
  if (void *mapped = primitive_buffer_->lock(0, sizeof(GpuPrimitive), 0)) {
    *static_cast<GpuPrimitive *>(mapped) = test_primitive;
    primitive_buffer_->unlock();
  }
  if (void *mapped = layer_buffer_->lock(0, sizeof(GpuLayer), 0)) {
    *static_cast<GpuLayer *>(mapped) = test_layer;
    layer_buffer_->unlock();
  }
  layer_count_ = 1;
  context_->geometry_system->mark_dirty("debug_verify_dirty_rebake_test");

  // Phase C: enough frames for the force-evict + ring-delay + reload +
  // ring-delay cycle to fully complete (roughly 1 + 3 + 1 + 3, see update_
  // streaming()'s comment) -- generous padding since this only runs on
  // demand, not every startup.
  for (int i = 0; i < 16; ++i) {
    simulate_frame();
  }
  if (chunk_streaming_levels_[0].state_of(ChunkKey{0, 0, 0, 0}) != ChunkState::Ready) {
    KERROR("Dirty-rebake verification FAILED: level-0 chunk (0,0,0) never "
          "returned to Ready after the forced re-voxelization cycle.");
    return;
  }

  // Phase D: query the sphere's surface -- PASSED only if the chunk's
  // reload actually picked up layer_count_=1 (the primitive added in Phase
  // B), not a second empty bake. update_streaming()'s async submissions
  // (async_compute_queue) have no semaphore link to this query (graphics_
  // queue) -- Ready above only means CPU-side ring-delay bookkeeping
  // elapsed, not that the GPU work is confirmed done, so a real device-
  // wide wait is needed here before trusting the query's result (the real
  // render path never needs this: render_to()'s own submission always
  // waits on whatever update_streaming() last returned -- see both
  // callers' own comments).
  vkDeviceWaitIdle(context_->device.logical_device);
  auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);
  query_clipmap_field(*cmd, glm::vec3(3.1f, 2.1f, 2.1f), camera_pos);
  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(cmd), context_->device.graphics_queue);

  ChunkDebugQueryOutput result{};
  if (void *mapped = chunk_debug_query_output_buffer_->lock(
          0, sizeof(ChunkDebugQueryOutput), 0)) {
    result = *static_cast<ChunkDebugQueryOutput *>(mapped);
    chunk_debug_query_output_buffer_->unlock();
  }

  if (std::abs(result.dist - 0.0f) > 0.02f) {
    KERROR("Dirty-rebake verification FAILED: point on sphere surface added "
          "AFTER the chunk was already resident expected dist ~0, got {} "
          "(material_index {}) -- an already-Ready chunk did not pick up "
          "the scene edit.",
          result.dist, result.material_index);
  } else {
    KINFO("Dirty-rebake verification PASSED (surface dist {}, material_index "
         "{} -- an already-resident chunk correctly re-baked after a scene "
         "edit with the camera stationary).",
         result.dist, result.material_index);
  }
}

void VulkanRaymarchShader::debug_verify_bulk_sweep_TEMP() {
  set_chunked_field_enabled(true);
  GeometrySystem &geo = *context_->geometry_system;

  // A grid of small spheres spread across several chunks/levels -- meant
  // to get a good number of chunks resident across multiple levels, like
  // a real scene, before triggering the brute-force sweep.
  SdfScene scene;
  SdfLayerDef layer;
  layer.name = "layer0";
  std::vector<glm::vec3> sphere_positions;
  int idx = 0;
  for (int x = -3; x <= 3; ++x) {
    for (int z = -3; z <= 3; ++z) {
      SdfPrimitiveDef prim;
      prim.name = "s" + std::to_string(idx++);
      prim.type = SdfPrimitiveType::Sphere;
      prim.position = glm::vec3(x * 3.0f + 0.3f, 2.1f, z * 3.0f + 0.3f);
      prim.params = glm::vec3(0.4f, 0.0f, 0.0f);
      prim.material_name = "default";
      sphere_positions.push_back(prim.position);
      layer.primitives.push_back(prim);
    }
  }
  scene.layers.push_back(layer);

  LoadedSceneNames loaded;
  geo.reconcile_scene(scene, loaded, true, "bulktest/");

  glm::vec3 camera_pos(0.0f, 0.0f, 0.0f);
  // Drains synchronously after each step -- see drain_streaming_for_debug().
  auto simulate_frame = [&]() {
    update_streaming(camera_pos);
    drain_streaming_for_debug();
  };
  for (int i = 0; i < 60; ++i) {
    simulate_frame();
  }

  auto query_point = [&](glm::vec3 p) -> f32 {
    auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
        *context_, context_->device.graphics_command_pool);
    query_clipmap_field(*cmd, p, camera_pos);
    VulkanCommandBuffer::end_single_use(*context_,
                                        context_->device.graphics_command_pool,
                                        std::move(cmd), context_->device.graphics_queue);
    ChunkDebugQueryOutput result{};
    if (void *mapped = chunk_debug_query_output_buffer_->lock(
            0, sizeof(ChunkDebugQueryOutput), 0)) {
      result = *static_cast<ChunkDebugQueryOutput *>(mapped);
      chunk_debug_query_output_buffer_->unlock();
    }
    return result.dist;
  };

  vkDeviceWaitIdle(context_->device.logical_device);
  int baseline_bad = 0;
  for (size_t i = 0; i < sphere_positions.size(); ++i) {
    glm::vec3 surface_point = sphere_positions[i] + glm::vec3(0.4f, 0.0f, 0.0f);
    f32 d = query_point(surface_point);
    if (std::abs(d) > 0.05f) {
      ++baseline_bad;
      KWARN("[bulktest] BASELINE sphere {} at ({},{},{}) surface dist={} "
           "(expected ~0)",
           i, sphere_positions[i].x, sphere_positions[i].y, sphere_positions[i].z, d);
    }
  }
  KINFO("[bulktest] baseline check: {} of {} spheres wrong before the sweep",
       baseline_bad, sphere_positions.size());

  // Now trigger the EXACT brute-force sweep update_streaming() does when
  // surgical=false -- every currently-Ready chunk, across every level.
  // Also mirrored into pending_forced_eviction_keys_ -- see its own
  // comment for why the drain loop and every OTHER producer of pending_
  // forced_evictions_ depend on that invariant actually holding.
  for (u32 level = 0; level < kNumLevels; ++level) {
    for (const auto &[key, record] : chunk_streaming_levels_[level].resident_chunks()) {
      if (record.state == ChunkState::Ready &&
          pending_forced_eviction_keys_.emplace(level, key).second) {
        pending_forced_evictions_.emplace_back(level, key);
      }
    }
  }
  KINFO("[bulktest] queued {} chunks for brute-force sweep",
       pending_forced_evictions_.size());
  for (int i = 0; i < 20; ++i) {
    simulate_frame();
  }
  vkDeviceWaitIdle(context_->device.logical_device);

  int after_bad = 0;
  for (size_t i = 0; i < sphere_positions.size(); ++i) {
    glm::vec3 surface_point = sphere_positions[i] + glm::vec3(0.4f, 0.0f, 0.0f);
    f32 d = query_point(surface_point);
    if (std::abs(d) > 0.05f) {
      ++after_bad;
      KWARN("[bulktest] AFTER-SWEEP sphere {} at ({},{},{}) surface dist={} "
           "(expected ~0) -- MISSING/CORRUPTED",
           i, sphere_positions[i].x, sphere_positions[i].y, sphere_positions[i].z, d);
    }
  }
  KINFO("[bulktest] after-sweep check: {} of {} spheres wrong after the "
       "brute-force sweep (baseline was {})",
       after_bad, sphere_positions.size(), baseline_bad);
}

void VulkanRaymarchShader::debug_verify_rapid_drag_crash_TEMP() {
  set_chunked_field_enabled(true);
  GeometrySystem &geo = *context_->geometry_system;

  // A dense grid of spheres, like debug_verify_bulk_sweep_TEMP()'s, so a
  // lot of chunks end up resident across every level near the camera --
  // maximizes how many entries a single dirty cycle's forced-rebake sweep
  // can queue.
  SdfScene scene;
  SdfLayerDef layer;
  layer.name = "layer0";
  int idx = 0;
  for (int x = -3; x <= 3; ++x) {
    for (int z = -3; z <= 3; ++z) {
      SdfPrimitiveDef prim;
      prim.name = "s" + std::to_string(idx++);
      prim.type = SdfPrimitiveType::Sphere;
      prim.position = glm::vec3(x * 3.0f + 0.3f, 2.1f, z * 3.0f + 0.3f);
      prim.params = glm::vec3(0.4f, 0.0f, 0.0f);
      prim.material_name = "default";
      layer.primitives.push_back(prim);
    }
  }
  // One extra, LARGE primitive -- the one we'll drag every frame.
  SdfPrimitiveDef dragged;
  dragged.name = "dragged";
  dragged.type = SdfPrimitiveType::Sphere;
  dragged.position = glm::vec3(0.0f, 0.0f, 0.0f);
  dragged.params = glm::vec3(2.0f, 0.0f, 0.0f);
  dragged.material_name = "default";
  layer.primitives.push_back(dragged);
  scene.layers.push_back(layer);

  LoadedSceneNames loaded;
  geo.reconcile_scene(scene, loaded, true, "dragtest/");
  rebake();

  glm::vec3 camera_pos(0.0f, 0.0f, 0.0f);
  // Drains synchronously after each step -- see drain_streaming_for_debug().
  auto simulate_frame = [&]() {
    update_streaming(camera_pos);
    drain_streaming_for_debug();
  };

  // Let the initial grid fully stream in and go Ready before the drag
  // starts -- a real user doesn't start dragging before the scene finishes
  // its first load.
  for (int i = 0; i < 80; ++i) {
    simulate_frame();
  }

  // Simulate a rapid gizmo drag / spinbox-scrub: EVERY frame, move the
  // dragged primitive a small amount and reconcile+rebake immediately,
  // exactly like on_live_edit_changed()/on_viewport_primitives_
  // transformed() do on every tick -- NOT waiting for the prior frame's
  // forced-rebake queue to fully drain first (kMaxForcedRebakesPerFrame
  // caps how much drains per frame, so a fast-enough drag keeps piling new
  // dirty cycles on top of an already-backlogged queue, exactly like a
  // real mouse-move-driven drag would).
  KINFO("[dragtest] starting rapid drag simulation...");
  for (int frame = 0; frame < 300; ++frame) {
    dragged.position = glm::vec3(std::sin(frame * 0.05f) * 3.0f, 0.0f,
                                 std::cos(frame * 0.05f) * 3.0f);
    scene.layers[0].primitives.back() = dragged;
    geo.reconcile_scene(scene, loaded, true, "dragtest/");
    rebake();
    simulate_frame();
    if (frame % 20 == 0) {
      KINFO("[dragtest] frame {} survived", frame);
    }
  }
  KINFO("[dragtest] rapid drag simulation completed all 300 frames without "
       "crashing.");
}

void VulkanRaymarchShader::debug_verify_lod_blend() {
  // Regression test for sample_clipmap_field()'s cross-fade fix (both the
  // original hard-cutoff pop AND the later skip_dist==min() sparse-
  // voxelization bug it was hiding -- see that function's own comment):
  // confirms a query point in the outer 25% of a level-0 chunk's outermost
  // ring blends with level 1 -- and specifically that it only does so when
  // BOTH sides have a real baked brick there, not whenever EITHER does.
  //
  // Level 0 (chunk_level_world_size(0)=4) chunk (1,0,0) spans x in [4,8),
  // center.x=6, half-width=2. Only x is at this chunk's Chebyshev limit
  // (delta=(1,0,0) relative to the origin camera's chunk (0,0,0)), so
  // sample_clipmap_field()'s outward = (query.x-6)/2. Solving outward =
  // (1-LOD_BLEND_FRACTION) + t*LOD_BLEND_FRACTION = 0.75 + t*0.25 for
  // t=0.5 (LOD_BLEND_FRACTION=0.25, Builtin.ChunkedFieldCommon.inc.glsl)
  // gives outward=0.875, i.e. query.x = 6 + 0.875*2 = 7.75.
  //
  // Level 0's chunk is baked with a zero-radius sphere centered EXACTLY on
  // the query point (7.75, 2, 2) -- its own pure value there is ~0.
  //
  // Level 1 (chunk_level_world_size(1)=8, cell_size=0.5) chunk (0,0,0)
  // spans x in [0,8) (comfortably contains x=7.75, chebyshev=0 there -- no
  // blending needed at level 1's own evaluation, exactly as sample_
  // clipmap_field()'s own comment on level L+1 always already covering
  // level L's blend zone predicts). Its cell allocation test (Builtin.
  // ChunkVoxelize.comp.glsl) checks distance from the CELL's CENTER, not
  // the query point -- the query point's own level-1 cell center works out
  // to (7.75, 2.25, 2.25) -- so the sphere is centered THERE (radius 0.1,
  // comfortably inside that cell's half-diagonal ~0.433, guaranteeing a
  // brick), giving a real (not placeholder) dist_b at the query point of
  // sqrt(0.25^2+0.25^2) - 0.1 = sqrt(0.125) - 0.1 ~= 0.254.
  //
  // Two separate submissions, not one shared command buffer: primitive_
  // buffer_ is host-visible CPU-written memory read by the GPU at DISPATCH
  // time, not at CPU-write time -- overwriting it for level 1's sphere
  // before level 0's own voxelize_chunk() dispatch has actually executed
  // (only guaranteed once its own submission's fence is waited, i.e. after
  // end_single_use() returns) would race, and every earlier debug_verify_*
  // method sidesteps this by using only one primitive per submission.
  //
  // A genuine 50/50 blend of ~0 and ~0.254 should read ~0.127 -- distinctly
  // between either level's own pure value, not equal to one of them.
  auto bake_one = [&](glm::vec3 sphere_pos, f32 radius, glm::ivec3 chunk_coord,
                      u32 level, u32 slot) {
    GpuPrimitive test_primitive{};
    test_primitive.position_type[0] = sphere_pos.x;
    test_primitive.position_type[1] = sphere_pos.y;
    test_primitive.position_type[2] = sphere_pos.z;
    test_primitive.position_type[3] =
        static_cast<f32>(static_cast<u32>(PrimitiveType::Sphere));
    test_primitive.params[0] = radius;
    test_primitive.rotation[3] = 1.0f;
    test_primitive.expr_scale[0] = 1.0f;
    test_primitive.repeat_count[0] = 1.0f;
    test_primitive.repeat_count[1] = 1.0f;
    test_primitive.repeat_count[2] = 1.0f;
    test_primitive.repeat_count[3] = 1.0f;
    test_primitive.deform[3] = 20.0f;

    GpuLayer test_layer{};
    test_layer.op_smoothness[0] =
        static_cast<f32>(static_cast<u32>(LayerOperation::Union));
    test_layer.op_smoothness[1] = 0.0f;
    test_layer.range[0] = 0;
    test_layer.range[1] = 1;

    GpuParamExpr empty_expr{};
    if (void *mapped = param_expr_buffer_->lock(0, sizeof(GpuParamExpr) * 4, 0)) {
      auto *exprs = static_cast<GpuParamExpr *>(mapped);
      exprs[0] = empty_expr;
      exprs[1] = empty_expr;
      exprs[2] = empty_expr;
      exprs[3] = empty_expr;
      param_expr_buffer_->unlock();
    }
    if (void *mapped = primitive_buffer_->lock(0, sizeof(GpuPrimitive), 0)) {
      *static_cast<GpuPrimitive *>(mapped) = test_primitive;
      primitive_buffer_->unlock();
    }
    if (void *mapped = layer_buffer_->lock(0, sizeof(GpuLayer), 0)) {
      *static_cast<GpuLayer *>(mapped) = test_layer;
      layer_buffer_->unlock();
    }

    auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
        *context_, context_->device.graphics_command_pool);
    voxelize_chunk(*cmd, chunk_coord, level, slot, /*layer_count=*/1,
                   /*max_smoothness=*/0.0f);
    VulkanCommandBuffer::end_single_use(*context_,
                                        context_->device.graphics_command_pool,
                                        std::move(cmd), context_->device.graphics_queue);
  };

  bake_one(glm::vec3(7.75f, 2.0f, 2.0f), /*radius=*/0.0f, glm::ivec3(1, 0, 0),
          /*level=*/0, /*slot=*/0);
  bake_one(glm::vec3(7.75f, 2.25f, 2.25f), /*radius=*/0.1f, glm::ivec3(0, 0, 0),
          /*level=*/1, /*slot=*/kMaxResidentChunks);

  auto query_cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);
  glm::vec3 camera_pos(0.0f, 0.0f, 0.0f);
  query_clipmap_field(*query_cmd, glm::vec3(7.75f, 2.0f, 2.0f), camera_pos);
  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(query_cmd),
                                      context_->device.graphics_queue);

  ChunkDebugQueryOutput result{};
  if (void *mapped = chunk_debug_query_output_buffer_->lock(
          0, sizeof(ChunkDebugQueryOutput), 0)) {
    result = *static_cast<ChunkDebugQueryOutput *>(mapped);
    chunk_debug_query_output_buffer_->unlock();
  }

  constexpr f32 kExpected = 0.127f;
  if (std::abs(result.dist - kExpected) > 0.08f) {
    KERROR("LOD blend verification FAILED: expected a ~50/50 blend of "
          "level 0's ~0 and level 1's ~0.254 (~{}), got {} (material_index "
          "{}) -- either the blend isn't engaging, or its weighting is "
          "off.",
          kExpected, result.dist, result.material_index);
  } else {
    KINFO("LOD blend verification PASSED (blended dist {}, expected ~{} -- "
         "distinctly between level 0's ~0 and level 1's ~0.254, confirming a "
         "genuine cross-fade instead of a hard cutoff to either side).",
         result.dist, kExpected);
  }
}

void VulkanRaymarchShader::debug_verify_thin_geometry_at_coarse_level() {
  // Regression test for kChunkBrickDim's fix -- the actual reported bug
  // this addresses: a primitive thinner than a coarse level's own voxel
  // spacing can lose its surface crossing entirely between adjacent
  // samples, reading as geometry that "loses its hits" specifically once
  // it falls under that level's responsibility (see kChunkBrickDim's own
  // comment for the full mechanism).
  //
  // A box with a 0.3-unit-thick Z extent (half-extent 0.15), baked at
  // level 4 of 5 (the coarsest -- chunk_level_world_size(4)=64, cell_size=
  // 4.0). At the OLD kChunkBrickDim=8, voxel_size was 4.0/8=0.5 -- wider
  // than the box's entire thickness, so neighbouring voxel samples could
  // both land outside it with nothing between them ever going negative,
  // washing the surface out of the trilinear-interpolated result entirely.
  // At the fixed kChunkBrickDim=16, voxel_size is 4.0/16=0.25 -- narrower
  // than the box's thickness, so a sample can actually land inside it.
  //
  // Queried exactly at the box's own center (70.1, 2.1, 2.1) -- x=70.1
  // deliberately, not something near the origin: sample_clipmap_field()
  // picks the FINEST level whose window contains the query point, and
  // levels 0-3 (sizes 4/8/16/32) all still window the origin's own
  // vicinity (this method runs after several earlier debug_verify_*()
  // calls that left real, unrelated data resident at levels 0-3 near
  // (2.1,2.1,2.1) -- the very bug this test's first draft actually hit:
  // "FAILED, dist=0.25" was that stale level-0 data, never level 4 at
  // all). x=70.1 clears every finer level's window (floor(70.1/32)=2, two
  // full chunks past a radius-1 camera-chunk-0 window) while still landing
  // in level 4's own (chunk_level_world_size(4)=64, floor(70.1/64)=1,
  // Chebyshev 1 from camera's chunk 0 -- exactly at the edge of, not past,
  // its radius-1 window), so this genuinely exercises level 4's own
  // voxels, not a fall-through from a finer level that happens to share
  // this test's coincidental numbers.
  //
  // Where the primitive's own thinnest half-extent (Z, 0.15) makes it the
  // true analytic nearest face: expected dist = -0.15 exactly. A positive
  // (or near-zero) result here means the coarse level's voxels genuinely
  // missed this thin feature -- reproducing, precisely, "geometry missing
  // its hits" -- rather than the query resolving imprecisely.
  GpuPrimitive test_primitive{};
  test_primitive.position_type[0] = 70.1f;
  test_primitive.position_type[1] = 2.1f;
  test_primitive.position_type[2] = 2.1f;
  test_primitive.position_type[3] =
      static_cast<f32>(static_cast<u32>(PrimitiveType::Box));
  test_primitive.params[0] = 1.0f;  // half-extent x
  test_primitive.params[1] = 1.0f;  // half-extent y
  test_primitive.params[2] = 0.15f; // half-extent z -- the thin dimension
  test_primitive.rotation[3] = 1.0f;
  test_primitive.expr_scale[0] = 1.0f;
  test_primitive.repeat_count[0] = 1.0f;
  test_primitive.repeat_count[1] = 1.0f;
  test_primitive.repeat_count[2] = 1.0f;
  // Bounding radius: length((1,1,0.15)) = sqrt(2.0225) ~= 1.4222, rounded
  // up generously (a too-small value here could wrongly self-cull the
  // primitive during scene_map()'s evaluation -- see repeat_count.w's own
  // comment; too-large only ever costs a slightly wider cull test).
  test_primitive.repeat_count[3] = 1.5f;
  test_primitive.deform[3] = 20.0f;

  GpuLayer test_layer{};
  test_layer.op_smoothness[0] =
      static_cast<f32>(static_cast<u32>(LayerOperation::Union));
  test_layer.op_smoothness[1] = 0.0f;
  test_layer.range[0] = 0;
  test_layer.range[1] = 1;

  GpuParamExpr empty_expr{};
  if (void *mapped = param_expr_buffer_->lock(0, sizeof(GpuParamExpr) * 4, 0)) {
    auto *exprs = static_cast<GpuParamExpr *>(mapped);
    exprs[0] = empty_expr;
    exprs[1] = empty_expr;
    exprs[2] = empty_expr;
    exprs[3] = empty_expr;
    param_expr_buffer_->unlock();
  }
  if (void *mapped = primitive_buffer_->lock(0, sizeof(GpuPrimitive), 0)) {
    *static_cast<GpuPrimitive *>(mapped) = test_primitive;
    primitive_buffer_->unlock();
  }
  if (void *mapped = layer_buffer_->lock(0, sizeof(GpuLayer), 0)) {
    *static_cast<GpuLayer *>(mapped) = test_layer;
    layer_buffer_->unlock();
  }

  glm::ivec3 chunk_coord(1, 0, 0);
  u32 level = 4;
  u32 slot = kMaxResidentChunks * level;

  auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);
  voxelize_chunk(*cmd, chunk_coord, level, slot, /*layer_count=*/1,
                 /*max_smoothness=*/0.0f);
  glm::vec3 camera_pos(0.0f, 0.0f, 0.0f);
  query_clipmap_field(*cmd, glm::vec3(70.1f, 2.1f, 2.1f), camera_pos);
  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(cmd), context_->device.graphics_queue);

  ChunkDebugQueryOutput result{};
  if (void *mapped = chunk_debug_query_output_buffer_->lock(
          0, sizeof(ChunkDebugQueryOutput), 0)) {
    result = *static_cast<ChunkDebugQueryOutput *>(mapped);
    chunk_debug_query_output_buffer_->unlock();
  }

  if (std::abs(result.dist - (-0.15f)) > 0.05f) {
    KERROR("Thin-geometry-at-coarse-level verification FAILED: box center "
          "at level 4 (coarsest, voxel_size=0.25) expected dist ~-0.15, "
          "got {} (material_index {}) -- a coarse level's voxels are still "
          "missing this thin feature's surface crossing.",
          result.dist, result.material_index);
  } else {
    KINFO("Thin-geometry-at-coarse-level verification PASSED (box center "
         "dist {}, expected ~-0.15 -- a 0.3-unit-thick feature correctly "
         "resolved even at the coarsest clip level).",
         result.dist);
  }
}

void VulkanRaymarchShader::bake_probes(VulkanCommandBuffer &cmd,
                                       u32 light_count) {
  // Zero probe_buffer_a_ -- bounce 0's "previous bounce" input, which must
  // start at zero (see Builtin.ProbeBake.comp.glsl's file header comment:
  // this is what makes bounce 0 naturally gather direct light only, with
  // no special-casing needed in the shader). probe_buffer_b_ doesn't need
  // zeroing -- it's always a write target before it's ever read.
  vkCmdFillBuffer(cmd.handle(), probe_buffer_a_->handle(), 0, VK_WHOLE_SIZE,
                 0);

  VkBufferMemoryBarrier fill_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  fill_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fill_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  fill_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  fill_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  fill_barrier.buffer = probe_buffer_a_->handle();
  fill_barrier.offset = 0;
  fill_barrier.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                      &fill_barrier, 0, nullptr);

  ProbeBakePushConstants push_constants{static_cast<i32>(light_count), ambient_};
  constexpr u32 local_size = 4; // must match local_size_x/y/z in the shader
  u32 groups = (kProbeDim + local_size - 1) / local_size;

  // Same pipeline every bounce -- bound once rather than redundantly
  // re-binding it each iteration.
  probe_bake_pipeline_->bind(cmd);

  for (u32 bounce = 0; bounce < kProbeBounceCount; ++bounce) {
    // Bounce 0 reads probe_buffer_a_ (just zeroed above) and writes
    // probe_buffer_b_; each subsequent bounce swaps which buffer is which
    // -- see kProbeFinalInBufferB's comment for why this alternation
    // determines which buffer holds the result after the loop. Each
    // direction has its own fixed descriptor set (probe_bake_set_ /
    // probe_bake_set_odd_ -- see their declaration comment) instead of one
    // set rewritten every bounce: all kProbeBounceCount dispatches are
    // recorded into the same command buffer now, so a rewrite here would
    // retroactively corrupt every earlier bounce's already-recorded bind
    // once the GPU actually executes them.
    bool write_to_b = (bounce % 2) == 0;
    VkDescriptorSet set = write_to_b ? probe_bake_set_ : probe_bake_set_odd_;
    VkBuffer curr = write_to_b ? probe_buffer_b_->handle() : probe_buffer_a_->handle();

    vkCmdBindDescriptorSets(cmd.handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                           probe_bake_pipeline_->layout(), 0, 1, &set, 0,
                           nullptr);
    vkCmdPushConstants(cmd.handle(), probe_bake_pipeline_->layout(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0,
                      sizeof(ProbeBakePushConstants), &push_constants);
    vkCmdDispatch(cmd.handle(), groups, groups, groups);

    // The next bounce (or, for the last bounce, render_set_'s ProbeBuffer
    // binding once this rebake() completes) reads curr, so make this
    // dispatch's write to it visible before moving on. prev was this
    // bounce's read-only input, already fully written by an earlier
    // barrier -- no need to re-barrier it.
    record_compute_buffer_barriers(cmd.handle(), VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT, {curr});
  }
}

void VulkanRaymarchShader::bake_gi_cascade_bounce(VulkanCommandBuffer &cmd,
                                                 u32 light_count, u32 bounce) {
  // First bake ever: chunk_gi_probe_buffer_a_ (bounce 0's "previous
  // bounce" input) holds device-local garbage and must be zeroed once.
  // Every bake after that deliberately SKIPS the fill and lets bounce 0
  // seed from the previous cascade's content instead -- see
  // gi_probes_initialized_'s declaration comment for both halves of the
  // reasoning (no flicker on the buffer the render pass is still reading,
  // and the stale seed's error decays away over the bounce chain anyway).
  if (bounce == 0 && !gi_probes_initialized_) {
    gi_probes_initialized_ = true;
    vkCmdFillBuffer(cmd.handle(), chunk_gi_probe_buffer_a_->handle(), 0,
                   VK_WHOLE_SIZE, 0);

    VkBufferMemoryBarrier fill_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    fill_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fill_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    fill_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fill_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fill_barrier.buffer = chunk_gi_probe_buffer_a_->handle();
    fill_barrier.offset = 0;
    fill_barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                        &fill_barrier, 0, nullptr);
  }

  ChunkProbeBakePushConstants push_constants{
      static_cast<i32>(light_count), ambient_, gi_cascade_center_.x,
      gi_cascade_center_.y,          gi_cascade_center_.z,
      last_camera_pos_.x,            last_camera_pos_.y, last_camera_pos_.z};
  constexpr u32 local_size = 4; // must match local_size_x/y/z in the shader
  u32 groups = (kGiProbeDim + local_size - 1) / local_size;

  chunk_probe_bake_pipeline_->bind(cmd);

  // Same alternation reasoning as bake_probes() -- chunk_probe_bake_set_
  // for even bounces (write chunk_gi_probe_buffer_b_), chunk_probe_bake_
  // set_odd_ for odd bounces, each a fixed descriptor set rather than one
  // rewritten per bounce. Bounces now land in consecutive frames'
  // submissions to the same graphics queue, so the trailing barrier below
  // still orders bounce N's writes before bounce N+1's reads exactly as it
  // did when they shared one command buffer.
  bool write_to_b = (bounce % 2) == 0;
  VkDescriptorSet set =
      write_to_b ? chunk_probe_bake_set_ : chunk_probe_bake_set_odd_;
  VkBuffer curr = write_to_b ? chunk_gi_probe_buffer_b_->handle()
                             : chunk_gi_probe_buffer_a_->handle();

  vkCmdBindDescriptorSets(cmd.handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         chunk_probe_bake_pipeline_->layout(), 0, 1, &set, 0,
                         nullptr);
  vkCmdPushConstants(cmd.handle(), chunk_probe_bake_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(ChunkProbeBakePushConstants), &push_constants);
  vkCmdDispatch(cmd.handle(), groups, groups, groups);

  // The next bounce (or, for the last bounce, render_set_'s
  // ChunkProbeBuffer binding once this bake completes) reads curr -- same
  // reasoning as bake_probes()'s identical trailing barrier.
  record_compute_buffer_barriers(cmd.handle(), VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_ACCESS_SHADER_READ_BIT, {curr});
}

void VulkanRaymarchShader::update_gi_cascade(VulkanCommandBuffer &cmd,
                                             glm::vec3 camera_pos) {
  // Called unconditionally every frame by begin_frame(), and it binds
  // pipelines and buffers that only exist once construction succeeded. On a
  // shader that failed to build (a device out of memory for the chunked
  // field's pools is the realistic case) those optionals are disengaged and
  // dereferencing them takes the process down. Same guard every other entry
  // point on this class already carries.
  if (!valid_) {
    return;
  }
  last_camera_pos_ = camera_pos;
  // A scene edit this frame (see gi_cascade_dirty_'s own comment) forces a
  // rebake below regardless of camera drift -- consumed here (read then
  // cleared) rather than left for update_streaming() to clear, since this
  // is the one and only consumer.
  bool force_rebake = gi_cascade_dirty_;
  gi_cascade_dirty_ = false;
  if (!force_rebake && glm::length(camera_pos - gi_cascade_center_) <=
      kGiCascadeRecenterThreshold) {
    // No new trigger -- but an amortized bake already in progress still
    // gets its one bounce for this frame (see gi_cascade_next_bounce_'s
    // comment for the whole one-bounce-per-frame design).
    if (gi_cascade_next_bounce_ < kProbeBounceCount) {
      bake_gi_cascade_bounce(cmd, static_cast<u32>(light_count_),
                             gi_cascade_next_bounce_);
      ++gi_cascade_next_bounce_;
    }
    return;
  }
  // Discrete whole-cell-step recenter -- classic terrain-clipmap texture
  // behavior (see kGiCascadeRecenterThreshold's declaration comment), not a
  // smooth follow: snapping to the nearest kGiCascadeCellSize multiple keeps
  // probe_world_position()'s GPU-side grid alignment exactly matching what
  // this recenter just committed, the same way voxelize_chunk()'s chunk_
  // world_min is always an exact multiple of the chunk's own cell size.
  //
  // round() only actually changes VALUE once camera_pos crosses into a new
  // kGiCascadeCellSize bucket -- but the distance-from-center check above
  // stays past kGiCascadeRecenterThreshold for a long stretch of frames
  // approaching that crossing (the threshold is exactly half a cell, so a
  // camera continuing straight past it doesn't fall back under threshold
  // again until center itself moves). Without this guard, EVERY one of
  // those frames would re-run the full kProbeBounceCount-bounce bake below
  // against an identical, unchanged center -- confirmed live: roughly half
  // of all frames during sustained camera movement, not the roughly-one-
  // per-cell-crossing this was designed for. Only the frame that actually
  // crosses into a new cell needs a new bake.
  glm::vec3 new_center =
      glm::round(camera_pos / kGiCascadeCellSize) * kGiCascadeCellSize;
  if (!force_rebake && new_center == gi_cascade_center_) {
    // Past the distance threshold but not yet into a new cell (see the
    // guard's own comment above) -- same continue-in-progress handling as
    // the first early-out.
    if (gi_cascade_next_bounce_ < kProbeBounceCount) {
      bake_gi_cascade_bounce(cmd, static_cast<u32>(light_count_),
                             gi_cascade_next_bounce_);
      ++gi_cascade_next_bounce_;
    }
    return;
  }
  gi_cascade_center_ = new_center;
  // (Re)start the amortized bake from bounce 0 -- a trigger landing while
  // an earlier bake is still mid-chain just restarts it against the new
  // center/scene (see gi_cascade_next_bounce_'s comment); its first bounce
  // goes out with THIS frame, the rest one per subsequent frame via the
  // early-outs above.
  bake_gi_cascade_bounce(cmd, static_cast<u32>(light_count_), 0);
  gi_cascade_next_bounce_ = 1;
}

void VulkanRaymarchShader::transition_image(
    VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
    VkImageLayout new_layout, VkAccessFlags src_access,
    VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
    VkPipelineStageFlags dst_stage) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;

  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);
}

namespace {
void write_vec3(f32 (&dest)[4], glm::vec3 v) {
  dest[0] = v.x;
  dest[1] = v.y;
  dest[2] = v.z;
  dest[3] = 0.0f;
}
} // namespace

void VulkanRaymarchShader::render_to(VulkanCommandBuffer &command_buffer,
                                    VkImage swapchain_image, u32 width,
                                    u32 height, f32 delta_time,
                                    const Camera &camera) {
  if (!valid_) {
    KWARN("VulkanRaymarchShader::render_to called on an invalid shader.");
    return;
  }

  PushConstants push_constants{};
  write_vec3(push_constants.camera_position, camera.position());
  write_vec3(push_constants.camera_forward, camera.forward());
  write_vec3(push_constants.camera_right, camera.right());
  write_vec3(push_constants.camera_up, camera.up());

  push_constants.light_count = light_count_;
  push_constants.ambient = ambient_;
  push_constants.selected_primitive_index = selected_primitive_index_;
  push_constants.flags = (grid_visible_ ? kRenderFlagGrid : 0);
  push_constants.layer_count = layer_count_;
  push_constants.volumetric_start = volumetric_start_;
  push_constants.volumetric_count = volumetric_count_;
  elapsed_time_ += delta_time;
  push_constants.time = elapsed_time_;
  push_constants.flags |= skybox_enabled_ ? kRenderFlagSkybox : 0;
  push_constants.flags |= chunked_field_enabled_ ? kRenderFlagChunkedField : 0;
  // The imperfect shadow maps are only sampled on frames that actually built
  // them, which needs the splat pipeline (the maps are splatted from the
  // same clusters) -- see the shadow splat in the prepass below.
  push_constants.flags |=
      (ism_enabled_ && chunked_field_enabled_ && splat_mode_ != SplatMode::Off)
          ? kRenderFlagIsm
          : 0;
  push_constants.frame_index = static_cast<i32>(noise_frame_);
  push_constants.gi_cascade_center_x = gi_cascade_center_.x;
  push_constants.gi_cascade_center_y = gi_cascade_center_.y;
  push_constants.gi_cascade_center_z = gi_cascade_center_.z;
  // The splat prepass only runs against the chunked field (the fixed-cube
  // field bakes no point cloud), so the mode the shader sees is forced to
  // Off everywhere else regardless of what set_splat_mode() was told -- the
  // one place that decision is made, so the push constant and whether the
  // dispatch below is recorded can never disagree.
  const bool splat_active =
      chunked_field_enabled_ && splat_mode_ != SplatMode::Off;
  push_constants.splat_mode =
      splat_active ? static_cast<i32>(splat_mode_) : 0;

  // This frame's sub-pixel sample offset, in pixels, centred on zero (see
  // kTaaJitterCount). Without TAA the offset is zero and pass 3 samples
  // pixel centres exactly as it always did.
  const f32 jitter_x =
      taa_enabled_ ? halton(taa_frame_ + 1, 2) - 0.5f : 0.0f;
  const f32 jitter_y =
      taa_enabled_ ? halton(taa_frame_ + 1, 3) - 0.5f : 0.0f;
  push_constants.taa_jitter_x = jitter_x;
  push_constants.taa_jitter_y = jitter_y;

  // The binary voxel cascades' geometry (step 6). Each cascade is centred
  // on the camera and SNAPPED to its own voxel grid: without the snap the
  // whole structure shifts by a fraction of a voxel every frame and the
  // occlusion it produces crawls. Both the build and the trace derive
  // everything from these four vec4s, so they cannot disagree about where a
  // voxel is.
  f32 cascade_origin[kVoxelCascadeCount][4]{};
  {
    const glm::vec3 camera_pos = camera.position();
    for (u32 i = 0; i < kVoxelCascadeCount; ++i) {
      const f32 extent =
          kVoxelCascadeFinestExtent * static_cast<f32>(1u << i);
      const f32 voxel = extent / static_cast<f32>(kVoxelCascadeDim);
      for (u32 axis = 0; axis < 3; ++axis) {
        const f32 unsnapped = camera_pos[axis] - extent * 0.5f;
        cascade_origin[i][axis] = std::floor(unsnapped / voxel) * voxel;
      }
      cascade_origin[i][3] = voxel;
    }
  }
  const bool ao_active =
      ao_enabled_ && chunked_field_enabled_ && splat_mode_ != SplatMode::Off;
  push_constants.flags |= ao_active ? kRenderFlagAo : 0;

  VkCommandBuffer cmd = command_buffer.handle();

  // --- Pass timing. The pool rotates over kTimestampFrames so the results
  // read back here belong to a frame that finished long ago; measuring
  // therefore costs no stall of its own. Every write below sits at a point
  // that executes unconditionally -- a query inside an `if` that didn't run
  // reads back unavailable and poisons the frame. ---
  const u32 ts_base = (timestamp_frame_ % kTimestampFrames) *
                      kGraphicsTimestampCount;
  auto write_timestamp = [&](GraphicsTimestamp slot) {
    if (timestamps_supported_) {
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         graphics_timestamp_pool_, ts_base + slot);
    }
  };
  if (timestamps_supported_) {
    vkCmdResetQueryPool(cmd, graphics_timestamp_pool_, ts_base,
                       kGraphicsTimestampCount);
    collect_frame_timings();
  }
  write_timestamp(kTsFrameBegin);

  constexpr u32 local_size = 16; // must match local_size_x/y in every pass 3/4 shader
  // Dispatches size off render_width_/render_height_ (output_image_/
  // bloom_temp_image_/post_process_image_'s actual size, per render_scale_
  // -- see set_render_scale()), not width/height (the swapchain's size,
  // used below only for the final upscale blit).
  u32 group_x = (render_width_ + local_size - 1) / local_size;
  u32 group_y = (render_height_ + local_size - 1) / local_size;

  // --- Splat prepass (only meaningful for the chunked field -- its
  // per-brick point clouds are what gets splatted; the fixed-cube field
  // has none): clear the visibility buffer to "no splat", then splat every
  // resident brick's baked surface points into it. Pass 3 below then
  // either starts its rays just short of each pixel's nearest splat
  // (SplatMode::Prime) or shades that splat directly (::Visibility) -- see
  // Builtin.ChunkPointSplat.comp.glsl's header comment for the whole
  // scheme. The chunked-field point buffers this reads were written on the
  // async compute queue -- safe here because this frame's graphics
  // submission already waits on update_streaming()'s semaphore (see
  // VulkanRendererBackend::begin_frame()), the same guarantee pass 3's own
  // chunked-field reads rely on. ---
  if (splat_active) {
    // vkCmdFillBuffer's fill value is a u32 pattern repeated across the
    // range, which is exactly the ~0 sentinel the shader compares each
    // u64 entry against (all bits set either way).
    vkCmdFillBuffer(cmd, splat_visibility_buffer_->handle(), 0, VK_WHOLE_SIZE,
                   0xFFFFFFFFu);
    // Same ~0 sentinel, same reasoning, for the per-tile near bound: "no
    // brick declined to splat over this tile" (see splat_tile_bound_
    // buffer_). As raw uint it also compares above every positive float's
    // bits, which is what makes the shader's atomicMin land correctly on
    // the first real bound written.
    vkCmdFillBuffer(cmd, splat_tile_bound_buffer_->handle(), 0, VK_WHOLE_SIZE,
                   0xFFFFFFFFu);
    VkBufferMemoryBarrier clear_barriers[2]{};
    clear_barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    clear_barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clear_barriers[0].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    clear_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clear_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clear_barriers[0].buffer = splat_visibility_buffer_->handle();
    clear_barriers[0].offset = 0;
    clear_barriers[0].size = VK_WHOLE_SIZE;
    clear_barriers[1] = clear_barriers[0];
    clear_barriers[1].buffer = splat_tile_bound_buffer_->handle();
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2,
                        clear_barriers, 0, nullptr);

    // --- Cluster cull. Compacts the pool down to the clusters that can
    // actually contribute a pixel this frame, and writes the indirect
    // arguments the splat dispatch below is launched with -- see
    // Builtin.ChunkClusterCull.comp.glsl. ---
    // The whole four-uint block (count and dispatch arguments alike) starts
    // at zero every frame: the count is accumulated with atomicAdd and the
    // row count with atomicMax, both of which need a known floor.
    vkCmdFillBuffer(cmd, chunk_cull_args_buffer_->handle(), 0, VK_WHOLE_SIZE,
                   0u);
    vkCmdFillBuffer(cmd, chunk_shadow_args_buffer_->handle(), 0, VK_WHOLE_SIZE,
                   0u);
    // ~0 is "nothing splatted in this direction" for the atlas, the same
    // sentinel the visibility buffer uses and for the same reason: as a raw
    // uint it compares above every positive float's bits, so the first real
    // atomicMin always wins.
    vkCmdFillBuffer(cmd, shadow_atlas_buffer_->handle(), 0, VK_WHOLE_SIZE,
                   0xFFFFFFFFu);
    vkCmdFillBuffer(cmd, chunk_resident_args_buffer_->handle(), 0,
                   VK_WHOLE_SIZE, 0u);
    // Zero, not ~0: these are occupancy BITS, and the frame starts with
    // nothing known to be solid anywhere.
    vkCmdFillBuffer(cmd, voxel_cascade_buffer_->handle(), 0, VK_WHOLE_SIZE, 0u);
    VkBufferMemoryBarrier cull_clear_barrier{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    cull_clear_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    cull_clear_barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    cull_clear_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cull_clear_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cull_clear_barrier.buffer = chunk_cull_args_buffer_->handle();
    cull_clear_barrier.offset = 0;
    cull_clear_barrier.size = VK_WHOLE_SIZE;
    VkBufferMemoryBarrier cull_clear_barriers[5] = {
        cull_clear_barrier, cull_clear_barrier, cull_clear_barrier,
        cull_clear_barrier, cull_clear_barrier};
    cull_clear_barriers[1].buffer = chunk_shadow_args_buffer_->handle();
    cull_clear_barriers[2].buffer = shadow_atlas_buffer_->handle();
    cull_clear_barriers[3].buffer = chunk_resident_args_buffer_->handle();
    cull_clear_barriers[4].buffer = voxel_cascade_buffer_->handle();
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 5,
                        cull_clear_barriers, 0, nullptr);
    write_timestamp(kTsPrepassClears);

    ChunkClusterCullPushConstants cull_push{};
    write_vec3(cull_push.camera_position, camera.position());
    write_vec3(cull_push.camera_forward, camera.forward());
    write_vec3(cull_push.camera_right, camera.right());
    write_vec3(cull_push.camera_up, camera.up());
    // The same uv extents pass 3 builds its rays over: uv.y spans +-0.5 and
    // uv.x is scaled by the aspect ratio (see its uv construction).
    cull_push.half_extent_y = 0.5f;
    cull_push.half_extent_x = 0.5f * static_cast<f32>(render_width_) /
                             static_cast<f32>(render_height_);
    cull_push.cluster_count = static_cast<i32>(kMaxChunkClusters);
    cull_push.light_count = light_count_;
    cull_push.ism_count = static_cast<i32>(kIsmCount);
    // The shadow maps are only worth building when something will sample
    // them -- see set_ism_enabled().
    const bool ism_active = ism_enabled_;
    cull_push.shadow_enabled = ism_active ? 1 : 0;

    chunk_cluster_cull_pipeline_->bind(command_buffer);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                           chunk_cluster_cull_pipeline_->layout(), 0, 1,
                           &chunk_cluster_cull_set_, 0, nullptr);
    vkCmdPushConstants(cmd, chunk_cluster_cull_pipeline_->layout(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0,
                      sizeof(ChunkClusterCullPushConstants), &cull_push);
    constexpr u32 kCullLocalSize = 64; // must match the shader's local_size_x
    vkCmdDispatch(cmd, (kMaxChunkClusters + kCullLocalSize - 1) / kCullLocalSize,
                 1, 1);

    // The cull's writes feed both the splat pass's reads AND the indirect
    // dispatch's own argument fetch, which is a separate pipeline stage with
    // its own access type -- missing that second barrier is the classic way
    // an indirect dispatch reads last frame's arguments.
    VkBufferMemoryBarrier cull_done_barriers[2]{};
    cull_done_barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    cull_done_barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    cull_done_barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    cull_done_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cull_done_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cull_done_barriers[0].buffer = chunk_visible_cluster_buffer_->handle();
    cull_done_barriers[0].offset = 0;
    cull_done_barriers[0].size = VK_WHOLE_SIZE;
    cull_done_barriers[1] = cull_done_barriers[0];
    cull_done_barriers[1].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    cull_done_barriers[1].buffer = chunk_cull_args_buffer_->handle();
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                        0, 0, nullptr, 2, cull_done_barriers, 0, nullptr);
    write_timestamp(kTsPrepassCull);

    // --- Voxel cascade build: rasterize the resident point cloud into the
    // binary cascades the ambient-occlusion pass traces against. Indirect
    // over the cull's resident list -- see Builtin.ChunkVoxelCascade.comp.
    // glsl. ---
    if (ao_active) {
      VkBufferMemoryBarrier cascade_ready_barriers[2]{};
      cascade_ready_barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      cascade_ready_barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      cascade_ready_barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      cascade_ready_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      cascade_ready_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      cascade_ready_barriers[0].buffer =
          chunk_resident_cluster_buffer_->handle();
      cascade_ready_barriers[0].offset = 0;
      cascade_ready_barriers[0].size = VK_WHOLE_SIZE;
      cascade_ready_barriers[1] = cascade_ready_barriers[0];
      cascade_ready_barriers[1].dstAccessMask =
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
      cascade_ready_barriers[1].buffer = chunk_resident_args_buffer_->handle();
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                              VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                          0, 0, nullptr, 2, cascade_ready_barriers, 0, nullptr);

      ChunkVoxelCascadePushConstants cascade_push{};
      std::memcpy(cascade_push.cascade_origin, cascade_origin,
                 sizeof(cascade_origin));
      chunk_voxel_cascade_pipeline_->bind(command_buffer);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                             chunk_voxel_cascade_pipeline_->layout(), 0, 1,
                             &chunk_voxel_cascade_set_, 0, nullptr);
      vkCmdPushConstants(cmd, chunk_voxel_cascade_pipeline_->layout(),
                        VK_SHADER_STAGE_COMPUTE_BIT, 0,
                        sizeof(ChunkVoxelCascadePushConstants), &cascade_push);
      vkCmdDispatchIndirect(cmd, chunk_resident_args_buffer_->handle(), 0);

      record_compute_buffer_barriers(cmd, VK_ACCESS_SHADER_WRITE_BIT,
                                     VK_ACCESS_SHADER_READ_BIT,
                                     {voxel_cascade_buffer_->handle()});
    }
    write_timestamp(kTsPrepassCascade);

    // --- Shadow splat: the imperfect shadow maps, built from the same
    // clusters. Indirect over the (cluster, light) pairs the cull just
    // emitted -- see Builtin.ChunkShadowSplat.comp.glsl. ---
    if (ism_active) {
      VkBufferMemoryBarrier shadow_ready_barriers[2]{};
      shadow_ready_barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      shadow_ready_barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      shadow_ready_barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      shadow_ready_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      shadow_ready_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      shadow_ready_barriers[0].buffer = chunk_shadow_pair_buffer_->handle();
      shadow_ready_barriers[0].offset = 0;
      shadow_ready_barriers[0].size = VK_WHOLE_SIZE;
      shadow_ready_barriers[1] = shadow_ready_barriers[0];
      shadow_ready_barriers[1].dstAccessMask =
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
      shadow_ready_barriers[1].buffer = chunk_shadow_args_buffer_->handle();
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                              VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                          0, 0, nullptr, 2, shadow_ready_barriers, 0, nullptr);

      ChunkShadowSplatPushConstants shadow_push{};
      shadow_push.frame_index = static_cast<i32>(noise_frame_);
      chunk_shadow_splat_pipeline_->bind(command_buffer);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                             chunk_shadow_splat_pipeline_->layout(), 0, 1,
                             &chunk_shadow_splat_set_, 0, nullptr);
      vkCmdPushConstants(cmd, chunk_shadow_splat_pipeline_->layout(),
                        VK_SHADER_STAGE_COMPUTE_BIT, 0,
                        sizeof(ChunkShadowSplatPushConstants), &shadow_push);
      vkCmdDispatchIndirect(cmd, chunk_shadow_args_buffer_->handle(), 0);

      // Atlas writes -> the shading pass's reads of it.
      record_compute_buffer_barriers(cmd, VK_ACCESS_SHADER_WRITE_BIT,
                                     VK_ACCESS_SHADER_READ_BIT,
                                     {shadow_atlas_buffer_->handle()});
    }
    write_timestamp(kTsPrepassShadow);

    ChunkPointSplatPushConstants splat_push{};
    write_vec3(splat_push.camera_position, camera.position());
    write_vec3(splat_push.camera_forward, camera.forward());
    write_vec3(splat_push.camera_right, camera.right());
    write_vec3(splat_push.camera_up, camera.up());
    splat_push.width = static_cast<i32>(render_width_);
    splat_push.height = static_cast<i32>(render_height_);
    splat_push.splat_mode = static_cast<i32>(splat_mode_);
    // Reused as the seed for the splat pass's stochastic decisions (Russian
    // roulette thinning, clip-level dithering) -- it only has to differ from
    // frame to frame for TAA to average them out.
    splat_push.frame_index = static_cast<i32>(noise_frame_);

    chunk_point_splat_pipeline_->bind(command_buffer);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                           chunk_point_splat_pipeline_->layout(), 0, 1,
                           &chunk_point_splat_set_, 0, nullptr);
    vkCmdPushConstants(cmd, chunk_point_splat_pipeline_->layout(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0,
                      sizeof(ChunkPointSplatPushConstants), &splat_push);
    // One workgroup (kChunkClusterPoints threads) per VISIBLE cluster,
    // folded into rows of kSplatDispatchStrideX to stay under the
    // per-dimension workgroup limit -- the cull pass wrote exactly these
    // arguments, and the shader bounds-checks the last, partial row against
    // the count it also wrote.
    vkCmdDispatchIndirect(cmd, chunk_cull_args_buffer_->handle(), 0);

    // Splat writes -> pass 3's reads of the visibility buffer and of the
    // tile bounds it has to check each winning splat against.
    record_compute_buffer_barriers(cmd, VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT,
                                   {splat_visibility_buffer_->handle(),
                                    splat_tile_bound_buffer_->handle()});
  }

  if (!splat_active) {
    // The four prepass stamps above live inside the splat_active block, and
    // collect_frame_timings() throws away any frame whose stamps are not
    // monotonically increasing. Writing them here keeps a no-splat frame
    // reportable instead of silently dropping every frame's timings.
    write_timestamp(kTsPrepassClears);
    write_timestamp(kTsPrepassCull);
    write_timestamp(kTsPrepassCascade);
    write_timestamp(kTsPrepassShadow);
  }
  write_timestamp(kTsStreamingPrepass);

  // --- Pass 3: render the scene into output_image_ (rgb=colour,
  // a=pixelation-exempt flag). ---
  // Storage image -> GENERAL for the compute shader to write into. Old
  // layout is claimed UNDEFINED (discard) since every pixel gets
  // overwritten unconditionally each frame; src stage/access wait on the
  // post-process passes' compute-shader reads of it from the previous
  // frame -- output_image_ is read by compute now (pass 4a/4b below), not
  // copied via transfer directly, so that's what this write must wait on.
  transition_image(cmd, output_image_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  // Both G-buffer halves get the same discard-and-write treatment, waiting
  // on the previous frame's reads of them (shading and TAA).
  transition_image(cmd, depth_image_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  transition_image(cmd, normal_material_image_.handle,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  // The AO image is transitioned unconditionally, even on frames the AO pass
  // is skipped: the shading pass declares its binding whether or not the
  // flag tells it to read it, and a descriptor pointing at an image in
  // UNDEFINED layout is a validation error on the strength of the
  // declaration alone.
  transition_image(cmd, ao_image_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  render_pipeline_->bind(command_buffer);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                         render_pipeline_->layout(), 0, 1, &render_set_, 0,
                         nullptr);
  vkCmdPushConstants(cmd, render_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants),
                    &push_constants);
  vkCmdDispatch(cmd, group_x, group_y, 1);

  write_timestamp(kTsVisibility);

  // --- Pass 3ao: ambient occlusion. One cosine-weighted ray per pixel
  // against the depth buffer and then the voxel cascades -- see Builtin.
  // StochasticAo.comp.glsl. Runs between visibility and shading because it
  // consumes the G-buffer and produces a term the shading pass applies. ---
  if (ao_active) {
    transition_image(cmd, depth_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    transition_image(cmd, normal_material_image_.handle,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    StochasticAoPushConstants ao_push{};
    write_vec3(ao_push.camera_position, camera.position());
    write_vec3(ao_push.camera_forward, camera.forward());
    write_vec3(ao_push.camera_right, camera.right());
    write_vec3(ao_push.camera_up, camera.up());
    std::memcpy(ao_push.cascade_origin, cascade_origin, sizeof(cascade_origin));
    ao_push.jitter_x = jitter_x;
    ao_push.jitter_y = jitter_y;
    ao_push.frame_index = static_cast<i32>(noise_frame_);

    stochastic_ao_pipeline_->bind(command_buffer);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                           stochastic_ao_pipeline_->layout(), 0, 1,
                           &stochastic_ao_set_, 0, nullptr);
    vkCmdPushConstants(cmd, stochastic_ao_pipeline_->layout(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0,
                      sizeof(StochasticAoPushConstants), &ao_push);
    vkCmdDispatch(cmd, group_x, group_y, 1);

    transition_image(cmd, ao_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  }

  write_timestamp(kTsAmbientOcclusion);

  // --- Pass 3a: deferred shading. Reads the G-buffer pass 3 just wrote and
  // produces the actual image -- see Builtin.DeferredShade.comp.glsl. Same
  // descriptor set and push constants as the visibility pass; only the
  // pipeline differs. ---
  transition_image(cmd, depth_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  transition_image(cmd, normal_material_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  deferred_shade_pipeline_->bind(command_buffer);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                         deferred_shade_pipeline_->layout(), 0, 1, &render_set_,
                         0, nullptr);
  vkCmdPushConstants(cmd, deferred_shade_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants),
                    &push_constants);
  vkCmdDispatch(cmd, group_x, group_y, 1);

  write_timestamp(kTsDeferredShade);

  // --- Pass 3b: TAA resolve. Reprojects the previous resolved frame into
  // this view, clamps it to this frame's local colour range and blends the
  // new sample in -- see Builtin.TaaResolve.comp.glsl. Always recorded,
  // even with TAA off: the blend factor becomes 1.0 and it degenerates to a
  // copy, which keeps the post-process chain bound to one static image. ---
  transition_image(cmd, output_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  transition_image(cmd, depth_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  const u32 history_read = taa_history_parity_;
  const u32 history_write = 1 - taa_history_parity_;
  // The image being written is discarded first (every pixel is overwritten);
  // the one being read holds last frame's resolve and must keep its
  // contents -- unless there is no valid history, in which case it holds
  // undefined memory and gets the same discard so no layout is ever a lie.
  transition_image(cmd, history_images_[history_write].handle,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  transition_image(cmd, history_images_[history_read].handle,
                   history_valid_ ? VK_IMAGE_LAYOUT_GENERAL
                                  : VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  transition_image(cmd, taa_output_image_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  TaaResolvePushConstants taa_push{};
  write_vec3(taa_push.camera_position, camera.position());
  write_vec3(taa_push.camera_forward, camera.forward());
  write_vec3(taa_push.camera_right, camera.right());
  write_vec3(taa_push.camera_up, camera.up());
  write_vec3(taa_push.prev_camera_position, prev_camera_position_);
  write_vec3(taa_push.prev_camera_forward, prev_camera_forward_);
  write_vec3(taa_push.prev_camera_right, prev_camera_right_);
  write_vec3(taa_push.prev_camera_up, prev_camera_up_);
  taa_push.jitter_x = jitter_x;
  taa_push.jitter_y = jitter_y;
  taa_push.blend =
      (taa_enabled_ && history_valid_) ? kTaaBlendFactor : 1.0f;

  taa_resolve_pipeline_->bind(command_buffer);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                         taa_resolve_pipeline_->layout(), 0, 1,
                         &taa_resolve_sets_[history_read], 0, nullptr);
  vkCmdPushConstants(cmd, taa_resolve_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(TaaResolvePushConstants), &taa_push);
  vkCmdDispatch(cmd, group_x, group_y, 1);

  // This frame's resolve becomes next frame's history, and next frame
  // reprojects through the basis used here.
  taa_history_parity_ = history_write;
  taa_frame_ = (taa_frame_ + 1) % kTaaJitterCount;
  noise_frame_ = (noise_frame_ + 1) % kNoiseFrameWrap;
  history_valid_ = true;
  prev_camera_position_ = camera.position();
  prev_camera_forward_ = camera.forward();
  prev_camera_right_ = camera.right();
  prev_camera_up_ = camera.up();

  write_timestamp(kTsTaaResolve);

  // --- Pass 4a: bloom bright-pass + horizontal blur, into
  // bloom_temp_image_ (half-resolution). ---
  // taa_output_image_ stays in GENERAL (no layout change, just a memory
  // barrier): pass 4a reads it via imageLoad right after the resolve wrote
  // it, so that write must be visible before this read.
  transition_image(cmd, taa_output_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  // bloom_temp_image_ -> GENERAL to write into; discard (every half-res
  // pixel is overwritten unconditionally), waiting on the previous
  // frame's read of it by pass 4b.
  transition_image(cmd, bloom_temp_image_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  BloomBlurHPushConstants bloom_blur_h_push{static_cast<i32>(render_width_),
                                            static_cast<i32>(render_height_),
                                            bloom_threshold_};
  bloom_blur_h_pipeline_->bind(command_buffer);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                         bloom_blur_h_pipeline_->layout(), 0, 1,
                         &bloom_blur_h_set_, 0, nullptr);
  vkCmdPushConstants(cmd, bloom_blur_h_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(BloomBlurHPushConstants), &bloom_blur_h_push);
  u32 half_group_x = ((render_width_ + 1) / 2 + local_size - 1) / local_size;
  u32 half_group_y = ((render_height_ + 1) / 2 + local_size - 1) / local_size;
  vkCmdDispatch(cmd, half_group_x, half_group_y, 1);

  write_timestamp(kTsBloom);

  // --- Pass 4b: finish the bloom blur vertically, composite bloom +
  // vignette + pixelation, write the final frame into
  // post_process_image_. ---
  transition_image(cmd, bloom_temp_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  transition_image(cmd, post_process_image_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  PostCompositePushConstants post_composite_push{
      static_cast<i32>(render_width_),
      static_cast<i32>(render_height_),
      bloom_enabled_ ? bloom_intensity_ : 0.0f,
      vignette_enabled_ ? vignette_strength_ : 0.0f,
      vignette_radius_,
      pixelation_enabled_ ? 1 : 0,
      static_cast<i32>(pixelation_block_size_),
  };
  post_composite_pipeline_->bind(command_buffer);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                         post_composite_pipeline_->layout(), 0, 1,
                         &post_composite_set_, 0, nullptr);
  vkCmdPushConstants(cmd, post_composite_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(PostCompositePushConstants), &post_composite_push);
  vkCmdDispatch(cmd, group_x, group_y, 1);

  // Final frame -> transfer source.
  transition_image(cmd, post_process_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);

  // Swapchain image -> transfer destination. The render pass that ran
  // earlier this frame wrote it via COLOR_ATTACHMENT_OUTPUT; wait for that
  // before overwriting. Old layout is claimed UNDEFINED since the copy
  // below overwrites every pixel, so the swapchain image's prior contents
  // (the render pass's clear) don't need preserving.
  transition_image(cmd, swapchain_image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);

  // Blits rather than copies -- post_process_image_ is render_width_ x
  // render_height_ (render_scale_ of the swapchain's own width/height, see
  // set_render_scale()), so this is also what upscales the render back up
  // to the swapchain's actual size. A no-op-equivalent plain upscale at
  // render_scale_ == 1.0 (src/dst extents match, LINEAR degenerates to an
  // exact copy).
  VkImageBlit blit_region{};
  blit_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  blit_region.srcSubresource.layerCount = 1;
  blit_region.srcOffsets[1] = {static_cast<i32>(render_width_),
                              static_cast<i32>(render_height_), 1};
  blit_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  blit_region.dstSubresource.layerCount = 1;
  blit_region.dstOffsets[1] = {static_cast<i32>(width), static_cast<i32>(height),
                              1};

  // Copies from post_process_image_ now, not output_image_ directly -- the
  // post-process chain's final result.
  vkCmdBlitImage(cmd, post_process_image_.handle,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain_image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit_region,
                VK_FILTER_LINEAR);

  // Swapchain image -> colour attachment, ready for the UI renderpass that
  // runs right after this (see VulkanRendererBackend::end_frame()) to draw
  // on top of it. Not a direct transition to VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
  // here anymore -- that now happens at the end of the UI renderpass
  // instead (its has_next_pass=false), since it's the last thing to touch
  // this image before vkQueuePresentKHR.
  transition_image(cmd, swapchain_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

  write_timestamp(kTsPostComposite);
  ++timestamp_frame_;
}

void VulkanRaymarchShader::ensure_fixed_brick_pool() {
  if (fixed_brick_pool_ready_ || !valid_) {
    return;
  }
  // Every caller reaches here from rebake(), which has already idled the
  // device -- but this destroys a buffer descriptor sets still point at, so
  // it does not rely on that.
  vkDeviceWaitIdle(context_->device.logical_device);

  const u64 brick_pool_size =
      static_cast<u64>(kMaxBricks) * kBrickVoxelCount * sizeof(f32);
  brick_pool_buffer_.reset();
  brick_pool_buffer_.emplace(*context_, brick_pool_size,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!brick_pool_buffer_->is_valid()) {
    KERROR("Failed to grow the fixed-cube field's brick pool to {} bytes.",
          brick_pool_size);
    return;
  }
  fixed_brick_pool_ready_ = true;

  // Re-point every descriptor that referenced the placeholder: the
  // voxelize pass writes it (binding 1), the render pass samples it
  // (binding 2), and both probe-bake sets march against it (binding 1).
  VkDescriptorBufferInfo info{brick_pool_buffer_->handle(), 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet writes[4]{};
  const VkDescriptorSet sets[4] = {voxelize_set_, render_set_, probe_bake_set_,
                                   probe_bake_set_odd_};
  const u32 bindings[4] = {1, 2, 1, 1};
  for (u32 i = 0; i < 4; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = sets[i];
    writes[i].dstBinding = bindings[i];
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &info;
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 4, writes, 0, nullptr);
  KINFO("Fixed-cube brick pool grown to {} MB on first use.",
       brick_pool_size / (1024 * 1024));
}

void VulkanRaymarchShader::collect_frame_timings() {
  if (!timestamps_supported_) {
    return;
  }
  // The pool region written kTimestampFrames-1 frames ago -- old enough
  // that the GPU has certainly finished it, so this read never waits. Any
  // frame that isn't ready anyway (a resize, a skipped frame) is simply not
  // counted rather than stalled for.
  if (timestamp_frame_ < kTimestampFrames) {
    return;
  }
  const u32 read_frame = (timestamp_frame_ + 1) % kTimestampFrames;
  u64 stamps[kGraphicsTimestampCount]{};
  if (vkGetQueryPoolResults(context_->device.logical_device,
                            graphics_timestamp_pool_,
                            read_frame * kGraphicsTimestampCount,
                            kGraphicsTimestampCount, sizeof(stamps), stamps,
                            sizeof(u64), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    return;
  }
  for (u32 i = 1; i < kGraphicsTimestampCount; ++i) {
    if (stamps[i] <= stamps[i - 1]) {
      return; // a timestamp went backwards -- discard the whole frame
    }
  }
  for (u32 i = 1; i < kGraphicsTimestampCount; ++i) {
    graphics_pass_ms_[i] += static_cast<f64>(stamps[i] - stamps[i - 1]) *
                            static_cast<f64>(timestamp_period_ns_) * 1e-6;
  }
  ++timestamp_samples_;

  if (timestamp_samples_ < kTimestampReportInterval) {
    return;
  }
  static const char *const kPassNames[kGraphicsTimestampCount] = {
      "(frame begin)", "prepass: clears", "prepass: cull",
      "prepass: cascade", "prepass: shadow", "prepass: splat",
      "visibility/G-buffer",
      "ambient occlusion", "deferred shade", "TAA resolve", "bloom",
      "post composite"};
  const f64 inv = 1.0 / static_cast<f64>(timestamp_samples_);
  f64 total = 0.0;
  for (u32 i = 1; i < kGraphicsTimestampCount; ++i) {
    total += graphics_pass_ms_[i] * inv;
  }
  KINFO("GPU FRAME {:.3f} ms over {} frames:", total, timestamp_samples_);
  for (u32 i = 1; i < kGraphicsTimestampCount; ++i) {
    KINFO("  {:<20} {:7.3f} ms", kPassNames[i], graphics_pass_ms_[i] * inv);
  }
  // The bake runs on the async queue, so it overlaps the frame rather than
  // adding to it -- but a bake longer than the frame is what a hitch at a
  // chunk boundary or an LOD change is made of, which is why the peak
  // matters more than the mean here.
  if (async_bake_samples_ > 0) {
    // ELAPSED per submission, and deliberately labelled as such: the async
    // queue shares the graphics engine, so these figures include whatever
    // graphics work the GPU interleaved and are NOT a measure of a
    // submission's own cost. The old "ms/chunk" derived from them was
    // meaningless for the same reason (it read 12,312 ms/chunk on content
    // that bakes in a fraction of that) and is gone. What is worth watching
    // is chunks published per interval -- streaming throughput -- alongside
    // the peak, since an elapsed peak far above the rest still points at a
    // submission the GPU could not interleave away.
    KINFO("  chunk bake (async)   {:7.3f} ms mean, {:7.3f} ms PEAK elapsed over"
         " {} submissions ({:.0f} chunks published)",
         async_bake_ms_total_ / static_cast<f64>(async_bake_samples_),
         async_bake_ms_peak_, async_bake_samples_, async_bake_chunks_total_);
  } else {
    KINFO("  chunk bake (async)   none this interval");
  }
  // Candidate-list health. A non-zero fallback share means those chunks
  // folded the WHOLE scene at every voxel -- see voxelize_chunk_batch().
  if (candidate_chunks_total_ > 0) {
    KINFO("  candidate lists      {}/{} chunks fell back to whole-scene",
         candidate_chunks_fallback_, candidate_chunks_total_);
  }
  // Residency snapshot, per level: Ready/Baking/Evicting chunks, then
  // free + ring-delayed slots. The one line that separates the streaming
  // failure modes from each other -- "free stuck at 0" is a slot leak or
  // eviction starvation, "Baking piling up" is publishes not landing,
  // "Ready low with free high" is loads not being planned.
  if (!chunk_streaming_levels_.empty()) {
    std::string residency;
    for (u32 level = 0; level < chunk_streaming_levels_.size(); ++level) {
      const ChunkStreamingManager &streaming = chunk_streaming_levels_[level];
      u32 ready = 0, baking = 0, evicting = 0;
      for (const auto &[key, record] : streaming.resident_chunks()) {
        if (record.state == ChunkState::Ready) {
          ++ready;
        } else if (record.state == ChunkState::Baking) {
          ++baking;
        } else if (record.state == ChunkState::Evicting) {
          ++evicting;
        }
      }
      residency += "L" + std::to_string(level) + " " + std::to_string(ready) +
                   "R/" + std::to_string(baking) + "B/" +
                   std::to_string(evicting) + "E f" +
                   std::to_string(streaming.free_slot_count()) + "+" +
                   std::to_string(streaming.releasing_slot_count()) + "  ";
    }
    KINFO("  chunk residency      {}forced {}", residency,
         pending_forced_evictions_.size());
  }
  // Brick deduplication -- how much of the baked content was a copy of
  // something already baked rather than freshly evaluated.
  if (alias_cells_total_ > 0) {
    KINFO("  brick dedup          {:.1f}% of cells copied ({} of {}), {} from"
         " an earlier bake",
         100.0 * static_cast<f64>(alias_cells_aliased_) /
             static_cast<f64>(alias_cells_total_),
         alias_cells_aliased_, alias_cells_total_, alias_cells_cached_);
  }

  for (u32 i = 0; i < kGraphicsTimestampCount; ++i) {
    graphics_pass_ms_[i] = 0.0;
  }
  timestamp_samples_ = 0;
  async_bake_ms_total_ = 0.0;
  async_bake_ms_peak_ = 0.0;
  async_bake_samples_ = 0;
  async_bake_chunks_total_ = 0.0;
  candidate_chunks_total_ = 0;
  candidate_chunks_fallback_ = 0;
  alias_cells_total_ = 0;
  alias_cells_aliased_ = 0;
  alias_cells_cached_ = 0;
}
