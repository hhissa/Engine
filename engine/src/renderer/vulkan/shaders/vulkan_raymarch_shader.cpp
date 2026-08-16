#include "vulkan_raymarch_shader.h"
#include "../../../core/logger.h"
#include "../../../resources/expression.h"
#include "../../../systems/geometry_system.h"
#include "../../../systems/texture_system.h"
#include "../vulkan_commandbuffer.h"
#include "../vulkan_image.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <initializer_list>
#include <unordered_map>
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
constexpr std::string_view BUILTIN_SHADER_NAME_PROBE_BAKE =
    "Builtin.ProbeBake";
constexpr std::string_view BUILTIN_SHADER_NAME_RAYMARCH =
    "Builtin.RaymarchShader";
constexpr std::string_view BUILTIN_SHADER_NAME_BLOOM_BLUR_H =
    "Builtin.BloomBlurH";
constexpr std::string_view BUILTIN_SHADER_NAME_POST_COMPOSITE =
    "Builtin.PostComposite";

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
  i32 grid_enabled; // Nonzero to draw the reference grid (the subdivided
                   // ground plane) -- see VulkanRaymarchShader::
                   // set_grid_visible().
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
  i32 skybox_enabled; // Nonzero to sample skybox_texture for the background
                     // (a primary ray miss) instead of the flat two-colour
                     // gradient -- see set_skybox()/apply_skybox() in
                     // Builtin.RaymarchShader.comp.glsl.
  i32 chunked_field_enabled; // Phase 4: nonzero to sample the chunked/
                            // streamed field instead of the fixed-cube one
                            // -- see set_chunked_field_enabled(). Off by
                            // default.
  f32 gi_cascade_center_x;
  f32 gi_cascade_center_y;
  f32 gi_cascade_center_z; // Phase 5: gi_cascade_center_'s current value --
                          // see sample_gi_cascade() in Builtin.
                          // RaymarchShader.comp.glsl.
};
// 4 vec4s + 13 scalars = 116 bytes -- comfortably within Vulkan's guaranteed
// minimum push constant size of 128 bytes, so no device limit query is
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

// Matches Builtin.ChunkVoxelize.comp.glsl's push constant block.
struct ChunkVoxelizePushConstants {
  i32 layer_count;
  f32 max_smoothness;
  i32 chunk_slot;
  f32 chunk_world_min_x;
  f32 chunk_world_min_y;
  f32 chunk_world_min_z;
  f32 chunk_cell_size;
};

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
  i32 chunk_slot;
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
      probe_bake_stage_(context, BUILTIN_SHADER_NAME_PROBE_BAKE, "comp",
                       VK_SHADER_STAGE_COMPUTE_BIT),
      render_stage_(context, BUILTIN_SHADER_NAME_RAYMARCH, "comp",
                   VK_SHADER_STAGE_COMPUTE_BIT),
      bloom_blur_h_stage_(context, BUILTIN_SHADER_NAME_BLOOM_BLUR_H, "comp",
                        VK_SHADER_STAGE_COMPUTE_BIT),
      post_composite_stage_(context, BUILTIN_SHADER_NAME_POST_COMPOSITE, "comp",
                           VK_SHADER_STAGE_COMPUTE_BIT) {
  if (!voxelize_stage_.is_valid() || !chunk_voxelize_stage_.is_valid() ||
      !chunk_debug_query_stage_.is_valid() || !chunk_evict_stage_.is_valid() ||
      !chunk_probe_bake_stage_.is_valid() ||
      !probe_bake_stage_.is_valid() ||
      !render_stage_.is_valid() || !bloom_blur_h_stage_.is_valid() ||
      !post_composite_stage_.is_valid()) {
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
  brick_pool_buffer_.emplace(*context_, brick_pool_size,
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
  chunk_indirection_buffer_.emplace(*context_, chunk_indirection_size,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_brick_pool_buffer_.emplace(*context_, chunk_brick_pool_size,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_brick_primitive_buffer_.emplace(*context_, chunk_brick_primitive_size,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  chunk_brick_demand_buffer_.emplace(*context_, sizeof(u32),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  chunk_brick_free_list_buffer_.emplace(
      *context_, static_cast<u64>(kMaxChunkBricks) * sizeof(i32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  chunk_brick_free_list_top_buffer_.emplace(
      *context_, sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  chunk_debug_query_output_buffer_.emplace(
      *context_, sizeof(ChunkDebugQueryOutput),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // Phase 5: the chunked field's GI cascade -- same size/usage pattern as
  // probe_buffer_a_/probe_buffer_b_ above (device-local, buffer a gets
  // TRANSFER_DST since bake_gi_cascade() zeroes it via vkCmdFillBuffer
  // before bounce 0, exactly like bake_probes() does for probe_buffer_a_).
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
  // 7=layer_buffer (shared), 8=param_expr_buffer (shared)} -- must match
  // Builtin.ChunkVoxelize.comp.glsl's bindings exactly.
  VkDescriptorSetLayoutBinding chunk_voxelize_bindings[9]{};
  for (u32 i = 0; i < 9; ++i) {
    chunk_voxelize_bindings[i].binding = i;
    chunk_voxelize_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_voxelize_bindings[i].descriptorCount = 1;
    chunk_voxelize_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_voxelize_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_voxelize_layout_info.bindingCount = 9;
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
  // 2=brick_free_list_top} -- must match Builtin.ChunkEvict.comp.glsl's
  // bindings exactly.
  VkDescriptorSetLayoutBinding chunk_evict_bindings[3]{};
  for (u32 i = 0; i < 3; ++i) {
    chunk_evict_bindings[i].binding = i;
    chunk_evict_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_evict_bindings[i].descriptorCount = 1;
    chunk_evict_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_evict_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_evict_layout_info.bindingCount = 3;
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

  VkDescriptorPoolSize chunk_pool_size{};
  chunk_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  // chunk_voxelize_set_ + chunk_debug_query_set_ + chunk_evict_set_ +
  // chunk_probe_bake_set_/_odd_ (x2, like probe_bake_set_/_odd_ above).
  chunk_pool_size.descriptorCount = 9 + 5 + 3 + 8 * 2;
  VkDescriptorPoolCreateInfo chunk_pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  chunk_pool_info.poolSizeCount = 1;
  chunk_pool_info.pPoolSizes = &chunk_pool_size;
  chunk_pool_info.maxSets = 5;
  VK_CHECK(vkCreateDescriptorPool(context_->device.logical_device,
                                  &chunk_pool_info, context_->allocator,
                                  &chunk_descriptor_pool_));

  VkDescriptorSetLayout chunk_layouts[5] = {
      chunk_voxelize_set_layout_, chunk_debug_query_set_layout_,
      chunk_evict_set_layout_, chunk_probe_bake_set_layout_,
      chunk_probe_bake_set_layout_};
  VkDescriptorSet chunk_sets[5];
  VkDescriptorSetAllocateInfo chunk_alloc_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  chunk_alloc_info.descriptorPool = chunk_descriptor_pool_;
  chunk_alloc_info.descriptorSetCount = 5;
  chunk_alloc_info.pSetLayouts = chunk_layouts;
  VK_CHECK(vkAllocateDescriptorSets(context_->device.logical_device,
                                    &chunk_alloc_info, chunk_sets));
  chunk_voxelize_set_ = chunk_sets[0];
  chunk_debug_query_set_ = chunk_sets[1];
  chunk_evict_set_ = chunk_sets[2];
  chunk_probe_bake_set_ = chunk_sets[3];
  chunk_probe_bake_set_odd_ = chunk_sets[4];

  VkDescriptorBufferInfo chunk_voxelize_buffer_infos[9] = {
      {chunk_indirection_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_pool_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_demand_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_free_list_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_free_list_top_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {layer_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {param_expr_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_voxelize_writes[9]{};
  for (u32 i = 0; i < 9; ++i) {
    chunk_voxelize_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_voxelize_writes[i].dstSet = chunk_voxelize_set_;
    chunk_voxelize_writes[i].dstBinding = i;
    chunk_voxelize_writes[i].descriptorCount = 1;
    chunk_voxelize_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_voxelize_writes[i].pBufferInfo = &chunk_voxelize_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 9,
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

  VkDescriptorBufferInfo chunk_evict_buffer_infos[3] = {
      {chunk_indirection_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_free_list_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_free_list_top_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_evict_writes[3]{};
  for (u32 i = 0; i < 3; ++i) {
    chunk_evict_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_evict_writes[i].dstSet = chunk_evict_set_;
    chunk_evict_writes[i].dstBinding = i;
    chunk_evict_writes[i].descriptorCount = 1;
    chunk_evict_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_evict_writes[i].pBufferInfo = &chunk_evict_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 3, chunk_evict_writes,
                        0, nullptr);

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
  // always-bound-but-conditionally-sampled reasoning.
  VkDescriptorSetLayoutBinding render_bindings[20]{};
  for (u32 i = 0; i < 20; ++i) {
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

  VkDescriptorSetLayoutCreateInfo render_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  render_layout_info.bindingCount = 20;
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

  // One pool backing all six sets.
  VkDescriptorPoolSize pool_sizes[3]{};
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  // voxelize's 7 + render's 16 (11 + Phase 4's 4 chunked-field bindings +
  // Phase 5's 1 GI cascade probe binding) + probe bake's 7 x2
  // (probe_bake_set_ and probe_bake_set_odd_ -- see their declaration
  // comment).
  pool_sizes[0].descriptorCount = 37;
  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  pool_sizes[1].descriptorCount = 6; // render's 1 + bloom blur h's 2 + post composite's 3
  pool_sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  // scene_textures + scene_bump_textures + skybox_texture.
  pool_sizes[2].descriptorCount = kMaxScenePrimitives * 2 + 1;

  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = pool_sizes;
  pool_info.maxSets = 6;
  VK_CHECK(vkCreateDescriptorPool(context_->device.logical_device, &pool_info,
                                  context_->allocator, &descriptor_pool_));

  VkDescriptorSetLayout layouts[6] = {
      voxelize_set_layout_,     render_set_layout_,
      probe_bake_set_layout_,   bloom_blur_h_set_layout_,
      post_composite_set_layout_, probe_bake_set_layout_};
  VkDescriptorSet sets[6];
  VkDescriptorSetAllocateInfo alloc_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc_info.descriptorPool = descriptor_pool_;
  alloc_info.descriptorSetCount = 6;
  alloc_info.pSetLayouts = layouts;
  VK_CHECK(vkAllocateDescriptorSets(context_->device.logical_device,
                                    &alloc_info, sets));
  voxelize_set_ = sets[0];
  render_set_ = sets[1];
  probe_bake_set_ = sets[2];
  bloom_blur_h_set_ = sets[3];
  post_composite_set_ = sets[4];
  probe_bake_set_odd_ = sets[5];

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
  } render_buffer_bindings[16] = {
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
  };

  VkDescriptorBufferInfo render_buffer_infos[16];
  VkWriteDescriptorSet render_writes[17]{};
  render_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  render_writes[0].dstSet = render_set_;
  render_writes[0].dstBinding = 0;
  render_writes[0].descriptorCount = 1;
  render_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  render_writes[0].pImageInfo = &render_image_info;

  for (u32 i = 0; i < 16; ++i) {
    render_buffer_infos[i] = {render_buffer_bindings[i].buffer, 0,
                              VK_WHOLE_SIZE};
    render_writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    render_writes[i + 1].dstSet = render_set_;
    render_writes[i + 1].dstBinding = render_buffer_bindings[i].binding;
    render_writes[i + 1].descriptorCount = 1;
    render_writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    render_writes[i + 1].pBufferInfo = &render_buffer_infos[i];
  }

  vkUpdateDescriptorSets(context_->device.logical_device, 17, render_writes,
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
  VkDescriptorImageInfo bloom_blur_h_image_infos[2] = {
      {VK_NULL_HANDLE, output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
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
      {VK_NULL_HANDLE, output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
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
  render_pipeline_.emplace(*context_, render_stage_,
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

  if (!voxelize_pipeline_->is_valid() || !chunk_voxelize_pipeline_->is_valid() ||
      !chunk_debug_query_pipeline_->is_valid() || !chunk_evict_pipeline_->is_valid() ||
      !probe_bake_pipeline_->is_valid() || !chunk_probe_bake_pipeline_->is_valid() ||
      !render_pipeline_->is_valid() || !bloom_blur_h_pipeline_->is_valid() ||
      !post_composite_pipeline_->is_valid()) {
    KERROR("Failed to create compute pipeline(s) for the raymarch field.");
    return;
  }

  // One ChunkStreamingManager per clip level, each owning a disjoint
  // slot_offset range of the shared chunk_indirection_buffer_ -- see
  // chunk_streaming_levels_'s own comment for why this can't be a single
  // member-initializer-list construction.
  chunk_streaming_levels_.reserve(kNumLevels);
  for (u32 level = 0; level < kNumLevels; ++level) {
    chunk_streaming_levels_.emplace_back(kMaxResidentChunks, kStreamRadiusChunks,
                                         kFramesInFlightDelay,
                                         level * kMaxResidentChunks);
  }

  valid_ = true;

  // Upload the registered static scene and bake it, now that everything
  // above exists.
  rebuild_static_scene();

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

  chunk_debug_query_output_buffer_.reset();
  chunk_brick_free_list_top_buffer_.reset();
  chunk_brick_free_list_buffer_.reset();
  chunk_brick_demand_buffer_.reset();
  chunk_brick_primitive_buffer_.reset();
  chunk_brick_pool_buffer_.reset();
  chunk_indirection_buffer_.reset();
  chunk_table_buffer_.reset();

  vulkan_image_destroy(context_, &post_process_image_);
  vulkan_image_destroy(context_, &bloom_temp_image_);
  vulkan_image_destroy(context_, &output_image_);
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
  VkWriteDescriptorSet render_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  render_write.dstSet = render_set_;
  render_write.dstBinding = 0;
  render_write.descriptorCount = 1;
  render_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  render_write.pImageInfo = &render_image_info;
  vkUpdateDescriptorSets(context_->device.logical_device, 1, &render_write, 0,
                        nullptr);

  VkDescriptorImageInfo bloom_blur_h_image_infos[2] = {
      {VK_NULL_HANDLE, output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
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
      {VK_NULL_HANDLE, output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
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
  vkDeviceWaitIdle(context_->device.logical_device);
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

void VulkanRaymarchShader::reset_chunked_field() {
  // Every table entry (across all kNumLevels' worth) starts "no chunk
  // resident" -- voxelize_chunk() fills in exactly the entries it assigns
  // a slot to.
  const u64 table_entries =
      static_cast<u64>(kNumLevels) * kChunkTableDim * kChunkTableDim * kChunkTableDim;
  if (void *mapped = chunk_table_buffer_->lock(0, table_entries * sizeof(i32), 0)) {
    i32 *table = static_cast<i32 *>(mapped);
    std::fill(table, table + table_entries, -1);
    chunk_table_buffer_->unlock();
  }

  // Free list starts full: every brick index available, in order --
  // Builtin.ChunkVoxelize.comp.glsl pops from the top down (see its own
  // comment: allocation order never matters, only that every index is
  // handed out at most once between resets).
  if (void *mapped = chunk_brick_free_list_buffer_->lock(
          0, static_cast<u64>(kMaxChunkBricks) * sizeof(i32), 0)) {
    i32 *free_list = static_cast<i32 *>(mapped);
    for (u32 i = 0; i < kMaxChunkBricks; ++i) {
      free_list[i] = static_cast<i32>(i);
    }
    chunk_brick_free_list_buffer_->unlock();
  }
  if (void *mapped = chunk_brick_free_list_top_buffer_->lock(0, sizeof(u32), 0)) {
    *static_cast<u32 *>(mapped) = kMaxChunkBricks;
    chunk_brick_free_list_top_buffer_->unlock();
  }
}

void VulkanRaymarchShader::voxelize_chunk(VulkanCommandBuffer &cmd,
                                          glm::ivec3 world_chunk_coord, u32 level,
                                          u32 gpu_slot, u32 layer_count,
                                          f32 max_smoothness) {
  // Assign world_chunk_coord to gpu_slot in level's own toroidal table
  // range -- must compute the exact same wrap (and the same level offset)
  // Builtin.ChunkedFieldCommon.inc.glsl's sample_chunked_field_at_level()
  // does GPU-side, or a query would read a different slot than this bake
  // just wrote. ChunkStreamingManager owns collision handling (evicting
  // whatever chunk previously held this wrapped slot, if any, before
  // reassigning it) -- this method just performs the assignment it's told
  // to.
  auto floor_mod = [](i32 v, i32 m) { return ((v % m) + m) % m; };
  i32 table_dim = static_cast<i32>(kChunkTableDim);
  glm::ivec3 wrapped(floor_mod(world_chunk_coord.x, table_dim),
                     floor_mod(world_chunk_coord.y, table_dim),
                     floor_mod(world_chunk_coord.z, table_dim));
  i32 table_index = static_cast<i32>(level) * table_dim * table_dim * table_dim +
      wrapped.x + wrapped.y * table_dim + wrapped.z * table_dim * table_dim;
  if (void *mapped = chunk_table_buffer_->lock(
          static_cast<u64>(table_index) * sizeof(i32), sizeof(i32), 0)) {
    *static_cast<i32 *>(mapped) = static_cast<i32>(gpu_slot);
    chunk_table_buffer_->unlock();
  }

  // Zero this bake's demand counter -- mirrors voxelize()'s identical
  // vkCmdFillBuffer+barrier for brick_counter_buffer_ (see its comment).
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

  f32 level_world_size = chunk_level_world_size(level);
  f32 level_cell_size = level_world_size / static_cast<f32>(kChunkCoarseDim);
  glm::vec3 chunk_world_min = glm::vec3(world_chunk_coord) * level_world_size;
  ChunkVoxelizePushConstants push_constants{
      static_cast<i32>(layer_count), max_smoothness, static_cast<i32>(gpu_slot),
      chunk_world_min.x, chunk_world_min.y, chunk_world_min.z, level_cell_size};
  vkCmdPushConstants(cmd.handle(), chunk_voxelize_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(ChunkVoxelizePushConstants), &push_constants);

  constexpr u32 local_size = 4; // must match local_size_x/y/z in the shader
  u32 groups = (kChunkCoarseDim + local_size - 1) / local_size;
  vkCmdDispatch(cmd.handle(), groups, groups, groups);

  // Make this bake's writes visible to whatever reads the chunked field
  // next in the same command buffer (query_chunked_field(), or a later
  // phase's own dispatches) -- same reasoning as voxelize()'s identical
  // trailing barrier.
  record_compute_buffer_barriers(
      cmd.handle(), VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
      {chunk_indirection_buffer_->handle(), chunk_brick_pool_buffer_->handle(),
       chunk_brick_primitive_buffer_->handle()});
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
                                       u32 gpu_slot) {
  // Clear the table entry FIRST (before the GPU dispatch below even runs)
  // -- see this method's header comment for why leaving it stale, even
  // briefly, would let a query resolve through to gpu_slot's about-to-be-
  // freed (or already-reused) content instead of correctly seeing "no
  // chunk resident." Same level-offset table index as voxelize_chunk().
  auto floor_mod = [](i32 v, i32 m) { return ((v % m) + m) % m; };
  i32 table_dim = static_cast<i32>(kChunkTableDim);
  glm::ivec3 wrapped(floor_mod(world_chunk_coord.x, table_dim),
                     floor_mod(world_chunk_coord.y, table_dim),
                     floor_mod(world_chunk_coord.z, table_dim));
  i32 table_index = static_cast<i32>(level) * table_dim * table_dim * table_dim +
      wrapped.x + wrapped.y * table_dim + wrapped.z * table_dim * table_dim;
  if (void *mapped = chunk_table_buffer_->lock(
          static_cast<u64>(table_index) * sizeof(i32), sizeof(i32), 0)) {
    *static_cast<i32 *>(mapped) = -1;
    chunk_table_buffer_->unlock();
  }

  chunk_evict_pipeline_->bind(cmd);
  vkCmdBindDescriptorSets(cmd.handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         chunk_evict_pipeline_->layout(), 0, 1,
                         &chunk_evict_set_, 0, nullptr);

  ChunkEvictPushConstants push_constants{static_cast<i32>(gpu_slot)};
  vkCmdPushConstants(cmd.handle(), chunk_evict_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(ChunkEvictPushConstants), &push_constants);

  constexpr u32 local_size = 4; // must match local_size_x/y/z in the shader
  u32 groups = (kChunkCoarseDim + local_size - 1) / local_size;
  vkCmdDispatch(cmd.handle(), groups, groups, groups);

  // Make this dispatch's free-list push visible to whatever pops from it
  // next -- a later voxelize_chunk() this same frame (or a future one).
  record_compute_buffer_barriers(
      cmd.handle(), VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
      {chunk_indirection_buffer_->handle(),
       chunk_brick_free_list_buffer_->handle(),
       chunk_brick_free_list_top_buffer_->handle()});
}

void VulkanRaymarchShader::update_streaming(VulkanCommandBuffer &cmd,
                                            glm::vec3 camera_pos) {
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
  // Deliberately brute-force (every resident chunk, not just ones the
  // dirty primitive's own chunks_touched_by() would name) rather than
  // surgical: a REMOVED primitive is already gone from GeometrySystem by
  // the time this runs (release() already erased it), leaving no bounds to
  // compute which chunks it used to occupy, and a MOVED primitive's old
  // position is equally unrecoverable here. Re-baking every resident chunk
  // near the camera costs at most kMaxResidentChunks*kNumLevels evictions
  // spread over kMaxChunkBakesPerFrame-sized budgets across a handful of
  // frames -- nowhere near the cost of the old fixed-cube rebake() this
  // replaces.
  if (!context_->geometry_system->dirty_since_last_snapshot().empty()) {
    for (u32 level = 0; level < kNumLevels; ++level) {
      for (const auto &[key, record] : chunk_streaming_levels_[level].resident_chunks()) {
        if (record.state == ChunkState::Ready) {
          pending_forced_evictions_.emplace_back(level, key);
        }
      }
    }
    context_->geometry_system->clear_dirty();
  }

  // One independent load/evict pass per clip level -- see chunk_streaming_
  // levels_'s own comment. Each level has its own budget (kMaxChunkBakes
  // PerFrame), so total per-frame chunk work scales with kNumLevels; still
  // far cheaper than one full rebake either way.
  for (u32 level = 0; level < kNumLevels; ++level) {
    ChunkStreamingManager &streaming = chunk_streaming_levels_[level];
    f32 level_world_size = chunk_level_world_size(level);

    streaming.tick();
    ChunkStreamingManager::Plan plan = streaming.update(camera_pos, level_world_size);

    u32 evicted = 0;
    for (ChunkKey key : plan.to_evict) {
      if (evicted >= kMaxChunkBakesPerFrame) {
        break;
      }
      ChunkState state = streaming.state_of(key);
      if (state != ChunkState::Ready) {
        // Raced with tick()/update() in between planning and acting (e.g.
        // this key already flipped to Evicting via an earlier iteration
        // this same loop, or dropped out of resident_ entirely) --
        // update()'s own to_evict only ever includes Ready chunks at the
        // moment it ran, but nothing stops the SAME key appearing once;
        // this check is defensive, not expected to trigger in practice.
        continue;
      }
      u32 slot = streaming.resident_chunks().at(key).gpu_slot;
      glm::ivec3 chunk_coord(static_cast<i32>(key.cx), static_cast<i32>(key.cy),
                             static_cast<i32>(key.cz));
      evict_chunk(cmd, chunk_coord, level, slot);
      streaming.commit_evict(key);
      ++evicted;
    }

    // Drain this level's share of pending_forced_evictions_ with whatever
    // budget the boundary evictions above didn't use -- a dirty-triggered
    // eviction is exactly as urgent as a boundary one (both fix an
    // otherwise-permanently-wrong chunk), so they share one budget rather
    // than each getting their own kMaxChunkBakesPerFrame.
    for (auto it = pending_forced_evictions_.begin();
        it != pending_forced_evictions_.end() && evicted < kMaxChunkBakesPerFrame;) {
      if (it->first != level) {
        ++it;
        continue;
      }
      ChunkKey key = it->second;
      it = pending_forced_evictions_.erase(it);
      if (streaming.state_of(key) != ChunkState::Ready) {
        // Already evicted by the boundary pass above this same frame (it
        // can appear in both plan.to_evict and here), or otherwise no
        // longer resident -- nothing left to do for this entry.
        continue;
      }
      u32 slot = streaming.resident_chunks().at(key).gpu_slot;
      glm::ivec3 chunk_coord(static_cast<i32>(key.cx), static_cast<i32>(key.cy),
                             static_cast<i32>(key.cz));
      evict_chunk(cmd, chunk_coord, level, slot);
      streaming.commit_evict(key);
      ++evicted;
    }

    u32 loaded = 0;
    for (ChunkKey key : plan.to_load) {
      if (loaded >= kMaxChunkBakesPerFrame) {
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
      voxelize_chunk(cmd, chunk_coord, level, slot, static_cast<u32>(layer_count_),
                     max_smoothness_);
      streaming.commit_load(key, slot);
      ++loaded;
    }
  }
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

  auto simulate_frame = [&]() {
    auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
        *context_, context_->device.graphics_command_pool);
    update_streaming(*cmd, camera_pos);
    VulkanCommandBuffer::end_single_use(*context_,
                                        context_->device.graphics_command_pool,
                                        std::move(cmd), context_->device.graphics_queue);
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
  // B), not a second empty bake.
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

void VulkanRaymarchShader::bake_gi_cascade(VulkanCommandBuffer &cmd,
                                           u32 light_count) {
  // Zero chunk_gi_probe_buffer_a_ -- bounce 0's "previous bounce" input,
  // exactly mirroring bake_probes()'s identical zero-fill of probe_buffer_a_
  // above (see its comment for why bounce 0 must see an all-zero prev
  // buffer). chunk_gi_probe_buffer_b_ never needs zeroing -- always a write
  // target before it's ever read.
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

  ChunkProbeBakePushConstants push_constants{
      static_cast<i32>(light_count), ambient_, gi_cascade_center_.x,
      gi_cascade_center_.y,          gi_cascade_center_.z,
      last_camera_pos_.x,            last_camera_pos_.y, last_camera_pos_.z};
  constexpr u32 local_size = 4; // must match local_size_x/y/z in the shader
  u32 groups = (kGiProbeDim + local_size - 1) / local_size;

  // Same pipeline every bounce -- bound once rather than redundantly
  // re-binding it each iteration (mirrors bake_probes()).
  chunk_probe_bake_pipeline_->bind(cmd);

  for (u32 bounce = 0; bounce < kProbeBounceCount; ++bounce) {
    // Same alternation reasoning as bake_probes() -- chunk_probe_bake_set_
    // for even bounces (write chunk_gi_probe_buffer_b_), chunk_probe_bake_
    // set_odd_ for odd bounces, each a fixed descriptor set rather than one
    // rewritten every iteration (every bounce is recorded into the same
    // command buffer up front, so a rewrite would retroactively corrupt
    // earlier bounces' already-recorded binds).
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
}

void VulkanRaymarchShader::update_gi_cascade(VulkanCommandBuffer &cmd,
                                             glm::vec3 camera_pos) {
  last_camera_pos_ = camera_pos;
  if (glm::length(camera_pos - gi_cascade_center_) <=
      kGiCascadeRecenterThreshold) {
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
  if (new_center == gi_cascade_center_) {
    return;
  }
  gi_cascade_center_ = new_center;
  bake_gi_cascade(cmd, static_cast<u32>(light_count_));
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
  push_constants.grid_enabled = grid_visible_ ? 1 : 0;
  push_constants.layer_count = layer_count_;
  push_constants.volumetric_start = volumetric_start_;
  push_constants.volumetric_count = volumetric_count_;
  elapsed_time_ += delta_time;
  push_constants.time = elapsed_time_;
  push_constants.skybox_enabled = skybox_enabled_ ? 1 : 0;
  push_constants.chunked_field_enabled = chunked_field_enabled_ ? 1 : 0;
  push_constants.gi_cascade_center_x = gi_cascade_center_.x;
  push_constants.gi_cascade_center_y = gi_cascade_center_.y;
  push_constants.gi_cascade_center_z = gi_cascade_center_.z;

  VkCommandBuffer cmd = command_buffer.handle();

  constexpr u32 local_size = 16; // must match local_size_x/y in every pass 3/4 shader
  // Dispatches size off render_width_/render_height_ (output_image_/
  // bloom_temp_image_/post_process_image_'s actual size, per render_scale_
  // -- see set_render_scale()), not width/height (the swapchain's size,
  // used below only for the final upscale blit).
  u32 group_x = (render_width_ + local_size - 1) / local_size;
  u32 group_y = (render_height_ + local_size - 1) / local_size;

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

  render_pipeline_->bind(command_buffer);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                         render_pipeline_->layout(), 0, 1, &render_set_, 0,
                         nullptr);
  vkCmdPushConstants(cmd, render_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants),
                    &push_constants);
  vkCmdDispatch(cmd, group_x, group_y, 1);

  // --- Pass 4a: bloom bright-pass + horizontal blur, into
  // bloom_temp_image_ (half-resolution). ---
  // output_image_ stays in GENERAL (no layout change, just a memory
  // barrier): pass 4a reads it via imageLoad right after pass 3 wrote it,
  // so that write must be visible before this read.
  transition_image(cmd, output_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
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
}
