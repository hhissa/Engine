#include <cstdio>
#include <filesystem>
#include "vulkan_raymarch_shader.h"
#include "../../../core/logger.h"
#include "../../../resources/expression.h"
#include "../../../systems/geometry_system.h"
#include "../../../systems/texture_system.h"
#include "../vulkan_commandbuffer.h"
#include "../vulkan_image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <glm/gtc/quaternion.hpp>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
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
// M1a/M2a: see stamp_shape().
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_SCULPT_STAMP =
    "Builtin.SculptStamp";
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_SCULPT_SPLAT_REGEN =
    "Builtin.SculptSplatRegen";
// M2b: see undo_stamp().
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_SCULPT_UNDO_RESTORE =
    "Builtin.SculptUndoRestore";
// M3c: see spray_paint().
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_SCULPT_SPRAYPAINT =
    "Builtin.SculptSpraypaint";
// M4b: see restore_sculpt_checkpoint().
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_SCULPT_CHECKPOINT_RESTORE =
    "Builtin.SculptCheckpointRestore";
// M4 (Crop/Cutout): see crop_volume().
constexpr std::string_view BUILTIN_SHADER_NAME_CHUNK_SCULPT_CROP =
    "Builtin.SculptCrop";

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
  i32 flags; // kRenderFlag* bits below. Four independent booleans packed
            // into one scalar: this block sits close to Vulkan's guaranteed
            // 128-byte push-constant limit, and four whole ints for four
            // bits is the first thing that should give.
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
  // NOTE: this struct is uploaded via vkCmdPushConstants and read back by
  // Builtin.RaymarchShader.comp.glsl/Builtin.DeferredShade.comp.glsl's own
  // `push_constant` blocks (both declared separately, byte-for-byte
  // identical to this one and to each other) -- every field here must match
  // those two in name, order AND size exactly, with NO extra/missing
  // fields, or the two sides silently disagree about which bytes mean what
  // (caught the hard way once already: selected_primitive_index/
  // layer_count/volumetric_start/volumetric_count/dynamic_primitive used to
  // live here, left over from the CSG primitive system the M0 deletion
  // pass removed from both GLSL blocks but NOT from this struct -- every
  // field below `flags` was reading one slot off until they were deleted,
  // which is how push.flags ended up reading an always-zero slot and the
  // chunked field silently stopped being sampled at all. If a new field is
  // genuinely needed by only one of the two shaders, it still has to be
  // added to BOTH push_constant blocks to keep the layout in sync -- there
  // is no way to add a C++-only field here without breaking this).
};
// PushConstants::flags bits -- must match RENDER_FLAG_* in Builtin.
// RaymarchShader.comp.glsl and Builtin.DeferredShade.comp.glsl.
constexpr i32 kRenderFlagGrid = 1;
constexpr i32 kRenderFlagSkybox = 2;
constexpr i32 kRenderFlagChunkedField = 4;
constexpr i32 kRenderFlagIsm = 8;

// 4 vec4s + 11 scalars = 108 bytes -- comfortably under Vulkan's guaranteed
// minimum push constant size (128 bytes), so no device limit query is
// needed here. Anything new the shading pass needs has to ride in a buffer
// instead (see transmission_buffer_, whose tail range deliberately
// self-terminates rather than being given a count here), OR be added to
// this struct AND both GLSL push_constant blocks at once -- see this
// struct's own comment on why the three must stay byte-for-byte identical.
//
// Machine-checked because the comment above was wrong for a while: it said
// 16 scalars / 128 bytes, from when this struct still carried five fields
// (selected_primitive_index/layer_count/volumetric_start/volumetric_count/
// dynamic_primitive) the GLSL side no longer declared at all -- a silent
// overflow here is a validation error at best and four bytes of garbage in
// every field after the overflow at worst.
static_assert(sizeof(PushConstants) <= 128,
              "PushConstants must fit Vulkan's guaranteed 128-byte minimum -- "
              "put new per-frame data in a buffer instead.");

// Must match TRANSMISSIVE_MAX_COUNT in Builtin.DeferredShade.comp.glsl.
// Every transmissive primitive is evaluated analytically at every pixel
// the range is tested against, so this is a per-pixel loop bound rather
// than a storage limit -- which is why it is small, and why exceeding it
// is worth a warning (see rebuild_static_scene()).
constexpr i32 kMaxShadedTransmissivePrimitives = 16;

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
  i32 pass;         // 0 = bake representatives, 1 = copy aliases, 3 = gather
                   // a baked chunk into the cache staging buffer, 4 =
                   // restore one from it. See the shader.
  i32 target_slot;    // passes 3/4 -- see the shader's identical fields.
  i32 staging_offset;
  f32 target_world_x;
  f32 target_world_y;
  f32 target_world_z;
  f32 target_cell_size;
  i32 collect_stats; // See ChunkBrickDemandBuffer in the shader.
};

// How many u32s chunk_brick_demand_buffer_ holds: [0] is the brick demand
// the overflow check reads, [1..5] are the bake-cost counters, [6] is the
// aliased-voxel counter (see the shader's own comment), and [7]/[8] are the
// two POOL GAUGES copied in by record_pool_gauges(). Kept as one buffer
// rather than several because they share its "written on the GPU, read once
// by the CPU after the fence" access pattern exactly.
//
// Was 6 while the shader already wrote [6] -- an out-of-bounds SSBO store
// on every aliased cell, which robustBufferAccess discards on most drivers
// but is undefined behaviour and cost the alias counter its value.
constexpr u32 kChunkBakeStatCount = 11;
// Indices into that buffer for the two gauges, so the copy and the read
// cannot drift apart.
constexpr u32 kChunkStatBricksFreeWord = 7;
constexpr u32 kChunkStatClustersFreeWord = 8;
// How many times generate_splat_points() rolled a whole LOD level back
// because the cluster page pool would not give it another page. This is
// the direct measure of "the scene is being rendered at lower splat
// density than it was baked to want" -- the thing that reads on screen as
// sparse, streaky surfaces. MONOTONIC across the run (the per-batch fill
// only clears word 0), so the report prints the interval's delta.
constexpr u32 kChunkStatSplatRollbackWord = 9;
// The subset of those where the brick's FIRST page was refused, so it
// emitted no points at all and its surface is missing rather than coarse.
// Non-zero here means the cluster pool is genuinely out, not merely tight.
constexpr u32 kChunkStatSplatNoPointsWord = 10;

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

// Mirrors Builtin.SculptStamp.comp.glsl/Builtin.SculptSplatRegen.comp.glsl's
// identical StampCell struct exactly -- see their shared comment for why
// it's vec4-only (std430 array-stride safety). Uploaded host-side (chunk_
// sculpt_cell_buffer_ is host-visible) by stamp_shape() every call.
struct GpuSculptStampCell {
  glm::ivec4 slot_cell;    // x = chunk-table slot, y = local cell index.
  glm::vec4 cell_min_size; // xyz = cell world-space min corner, w = size.
};

// How many touched coarse cells one stamp_shape() call can record in
// chunk_sculpt_cell_buffer_ -- a sphere wanting more than this is
// truncated (with a warning) rather than growing the buffer, which is
// already generous headroom for M1a's single-sphere smoke test (a radius-1
// sphere at COARSE_CELL_SIZE=0.25 touches on the order of a few hundred
// cells at most, well under this).
constexpr u32 kMaxSculptStampCellsPerCall = 4096;

// M2b: how many of a stamp's touched cells Builtin.SculptStamp.comp.glsl
// captures pre-overwrite undo state for -- must match its own
// UNDO_CAPTURE_CAPACITY exactly (same manual-sync-point convention as every
// other constant shared between this file and the GLSL side). Deliberately
// much smaller than kMaxSculptStampCellsPerCall above: that cap exists to
// bound a single dispatch's cost and is sized for a worst-case huge brush;
// this one bounds a PERSISTENT per-cell capture (up to kChunkBrickVoxelCount
// floats each) that stamp_shape() reads back and keeps in CPU memory for
// every stamp made this session (see undo_stack_). Sizing it to
// kMaxSculptStampCellsPerCall's own 4096 would mean a ~91MB scratch buffer
// allocated once PLUS up to ~91MB retained per undoable stamp in the worst
// case -- wildly more than any realistic brush needs. 1024 is a generous
// multiple of the "a few hundred cells at most" a default-sized (radius-1)
// sphere touches (see sculpt_shape_bounding_radius()'s own comment) while
// staying a two-digit-MB one-time allocation (1024 * (16 + kChunkBrickVoxel
// Count*4) bytes =~ 22.8MB). A stamp whose cell_count exceeds this (an
// unusually large brush) still gets fully STAMPED -- only its UNDO capture
// is truncated, with a warning; undoing that one stamp will revert the
// first kMaxSculptUndoCellsPerStamp cells correctly and leave the remainder
// as stamped, the same class of documented, accepted degrade kMaxSculpt
// StampCellsPerCall's own truncation already is.
constexpr u32 kMaxSculptUndoCellsPerStamp = 1024;

// M2b: how many stamps back undo_stamp() can still revert -- see undo_
// stack_'s own comment (vulkan_raymarch_shader.h). 64 is generous for an
// interactive authoring session (the plan's own M2b section suggests
// 50-100) while keeping the worst-case CPU memory bounded: at
// kMaxSculptUndoCellsPerStamp MODIFIED cells each holding a full
// kChunkBrickVoxelCount-float snapshot, 64 stamps tops out around 64 * 1024
// * 5832 * 4 bytes =~ 1.46GB in the absolute worst case (every cell of
// every stamp a dense MODIFIED blend, which real sculpting sessions never
// sustain -- most cells of most stamps are either NEW_BRICK, which captures
// no voxel data at all, so the practical cost is far below this). Oldest
// entries are simply dropped once this is exceeded (see stamp_shape()).
constexpr size_t kMaxSculptUndoDepth = 64;

// Must match both Builtin.SculptStamp.comp.glsl's and Builtin.
// SculptSplatRegen.comp.glsl's local_size_x exactly -- one invocation per
// touched cell in each dispatch.
constexpr u32 kSculptLocalSize = 64;

// Matches Builtin.SculptStamp.comp.glsl's push constant block EXACTLY --
// member for member, same order, every member a plain 4-byte scalar (f32
// or i32, never a vec/ivec) so there is no std430 vec4-alignment padding
// for the two sides to silently disagree about. This is not cosmetic -- a
// prior milestone (M1a) had a real, hard-to-diagnose bug from exactly this
// kind of struct drifting out of byte-sync between C++ and GLSL (see the
// plan file's M1a postmortem, /home/h-i/.claude/plans/joyful-skipping-
// giraffe.md). Whatever changes here must change identically in the GLSL
// block.
struct SculptStampPushConstants {
  f32 center_x, center_y, center_z;
  f32 param0, param1, param2, param3; // See VulkanRaymarchShader::
                                      // stamp_shape()'s header comment for
                                      // what these four mean per shape.
  f32 smoothness;
  f32 voxel_size;
  i32 shape_type; // VulkanRaymarchShader::BrushShape's underlying value.
  i32 cell_count;
  i32 material_slot;
  i32 op; // VulkanRaymarchShader::BrushOp's underlying value.
  // M3 (Looseness): the per-stamp authored looseness value (SculptStamp::
  // looseness, 0 = uniform/clean to 1 = maximally loose), written alongside
  // material_slot into the new per-brick chunk_brick_looseness_buffer_ --
  // see Builtin.SculptStamp.comp.glsl's identical new `looseness` member
  // (same UNION-only-overwrites rule as material_slot's own write). Added
  // at the END of the struct deliberately -- every member above this one
  // already matches Builtin.SculptStamp.comp.glsl's PushConstants block
  // exactly; appending keeps every existing member's offset unchanged.
  f32 looseness;
};
// Byte-exact cross-check against Builtin.SculptStamp.comp.glsl's own
// PushConstants block (14 plain 4-byte scalars, no vec/ivec, so no std430
// alignment padding on either side) -- see this struct's own header
// comment and the plan file's M1a postmortem for why this one struct is
// worth a hard compile-time check instead of just a comment.
static_assert(sizeof(SculptStampPushConstants) == 14 * 4,
             "SculptStampPushConstants must stay byte-identical to Builtin."
             "SculptStamp.comp.glsl's PushConstants block");

// Matches Builtin.SculptSpraypaint.comp.glsl's push constant block EXACTLY
// -- same scalar-only/no-vec convention as SculptStampPushConstants above,
// and the same reasoning for why (see its own comment).
struct SculptSpraypaintPushConstants {
  f32 center_x, center_y, center_z;
  f32 param0, param1, param2, param3;
  f32 voxel_size;
  i32 shape_type;
  i32 cell_count;
  f32 opacity_per_pass;
};

// Matches Builtin.SculptSplatRegen.comp.glsl's push constant block.
struct SculptSplatRegenPushConstants {
  i32 cell_count;
  i32 stats;
  f32 chunk_cell_size;
  f32 voxel_size;
};

// Mirrors Builtin.SculptStamp.comp.glsl's/Builtin.SculptUndoRestore.comp.
// glsl's identical UndoCaptureMeta struct exactly -- written by the former,
// read by stamp_shape() (CPU readback) and the latter (GPU restore).
struct GpuUndoCaptureMeta {
  i32 change_type; // 0 = NOOP, 1 = NEW_BRICK, 2 = MODIFIED.
  i32 old_material;
  // M3 (Looseness): repurposed from a formerly-unused pad0 -- same
  // change_type==2-only meaning as old_material immediately above, captured
  // and restored alongside it (Builtin.SculptStamp.comp.glsl/Builtin.
  // SculptUndoRestore.comp.glsl's own identical struct). Also reused, same
  // "repurposed" convention as old_material's own "material to restore"
  // reuse below, by the checkpoint-restore path (restore_sculpt_
  // checkpoint()) to carry "the looseness to restore".
  f32 old_looseness;
  i32 pad1;
};

// Matches Builtin.SculptUndoRestore.comp.glsl's push constant block.
struct SculptUndoRestorePushConstants {
  i32 cell_count;
};

// Matches Builtin.SculptCrop.comp.glsl's push constant block EXACTLY -- same
// scalar-only convention as every other sculpt-family block above (see
// SculptStampPushConstants's own comment for why).
struct SculptCropPushConstants {
  f32 center_x, center_y, center_z;
  f32 param0, param1, param2, param3;
  f32 smoothness;
  i32 shape_type;
  i32 cell_count;
  i32 keep_outside;
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
  // Diagnostic: when >= 0, only this clip level is allowed to splat, so a
  // suspect artefact can be attributed to one level by isolating each in
  // turn. -1 (the default, and anything but KENGINE_SPLAT_ONLY_LEVEL being
  // set) is normal rendering. See splat_only_level_.
  i32 only_level;
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

// Start a COLD bake only every Nth update_streaming() call. Cache restores
// are exempt (see the pacing block in the to_load loop for the full
// rationale and why this only became safe once a non-resident chunk fell
// back to a coarser level instead of a hole). 1 restores the old
// every-frame behaviour.
constexpr u64 kChunkColdBakeFrameStride = 3;
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

// --- Disk-backed brick cache (strategy 1). Mirrors CHUNK_CACHE_* in
// Builtin.SdfFieldConfig.inc.glsl exactly -- see that block for the payload
// layout and the quantization's precision argument.
// Raised 512 -> 2048 on 2026-08-20, from measurement rather than taste: a
// live session baked 315 chunks holding 148,195 bricks between them, i.e.
// **470 bricks per chunk on average**, against a cap of 512. The cap sat
// almost exactly on the mean, so roughly half of all chunks were refused by
// the cache -- and being refused for DENSITY means it was always the
// expensive half. The cheap chunks cached; the ones actually worth caching
// re-baked from scratch on every single visit, which is why baking stayed
// prevalent no matter how warm the cache got.
//
// 2048 covers a chunk with half its 4096 cells on a surface. The cost is
// staging memory (~12MB per region, host-visible, so system RAM not VRAM),
// which is why kChunkCacheStagingRegions drops to 8 alongside this.
constexpr u32 kChunkCacheMaxBricks = 2048;
constexpr u32 kChunkCacheBrickWords = kChunkBrickVoxelCount / 4;
constexpr u32 kChunkCacheCellMapWord = 1;
constexpr u32 kChunkCachePrimitiveWord = kChunkCacheCellMapWord + kChunkCellCount;
constexpr u32 kChunkCacheVoxelWord =
    kChunkCachePrimitiveWord + kChunkCacheMaxBricks;
constexpr u32 kChunkCacheWords =
    kChunkCacheVoxelWord + kChunkCacheMaxBricks * kChunkCacheBrickWords;

// How many chunk payloads the staging buffer holds. Gathers and restores
// both draw from it, and a region stays claimed until the submission that
// touched it has signalled -- so this is also the cap on how many chunks
// can be cached or restored in one frame. ~2.9MB per region.
// Raised from 4 after a live run: at 4, only four chunks per frame could be
// gathered or restored, so most bakes were never cached and most cache hits
// never got a region to land in (measured 25 restores against 97 stores and
// several hundred chunks published). This is the cap on cache throughput in
// both directions, and at ~2.9MB per region it is cheap to widen.
constexpr u32 kChunkCacheStagingRegions = 8;
// Threads per workgroup in Builtin.ChunkVoxelize.comp.glsl (8 * 8 * 1), which
// the 1-D cache-pass dispatches below divide the cell count by. The shader
// flattens the same way in cache_cell_index(); the two MUST agree, and did
// not until 2026-08-20 -- see that function's comment.
constexpr u32 kChunkCacheThreadsPerGroup = 64;
// chunk_cache_region_busy_ (header) must have one entry per region; the
// header cannot see this constant, so the two are reconciled inside
// acquire_cache_region(), which can see both.

// On-disk format version. Bumped whenever the payload layout or the
// quantization changes -- an entry written by an older build must never be
// restored by a newer one, and the version is the only thing standing
// between "stale cache" and "silently wrong geometry".
//
// 2: CHUNK_CACHE_BAND_VOXELS 2 -> 8. Same layout, but every voxel byte
//    means four times the distance it used to, so a version-1 entry read
//    by this build would be geometry that is simply wrong rather than
//    stale. Also the version at which the key started covering primitive
//    indices and layer content (see chunk_content_hash()).
// 3: the gather that wrote every version-1 and version-2 entry only ever
//    visited 512 of a chunk's 4096 cells (see cache_cell_index() in the
//    shader), so every existing file on disk is a partial, wrong snapshot.
//    They must never be restored again.
// 4: CHUNK_CACHE_MAX_BRICKS 512 -> 2048, which moves the voxel word offset
//    within the payload. Layout change, so v3 entries are unreadable.
// 5: layer_fold_candidate() (Builtin.SdfSceneCommon.inc.glsl) now measures
//    a finite layer fold's instance id from the primitive's own position
//    instead of the world origin, so a layer authored away from the origin
//    bakes to a genuinely different field. Nothing in the KEY changed --
//    the key is hashed over the uploaded primitive/layer bytes, which are
//    identical either side of this -- so without a bump every chunk of
//    every repeated layer would restore its pre-fix, collapsed geometry
//    from disk and the fix would be invisible. THIS IS THE GENERAL RULE:
//    a change to what the voxelize shaders COMPUTE needs a version bump
//    just as much as a change to what the file HOLDS.
constexpr u32 kChunkCacheFormatVersion = 5;


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
  // displace_amplitude, w = displace_frequency. The bend's DIRECTION
  // (Geometry::bend_axis) does not fit here -- all four slots were already
  // spoken for -- and rides in layer_repeat_count[3] below instead.
  f32 deform[4];
  // The repetition of the LAYER this primitive belongs to, in the same
  // packing as repeat_mode_cell/repeat_count above: x = RepetitionMode as
  // a float, yzw = cell; then xyz = count (w unused). Folded in WORLD
  // space before the primitive's own rotation, so a whole layer repeats
  // as one arrangement -- see Geometry::layer_repetition_mode and
  // primitive_sdf() in Builtin.SdfSceneCommon.inc.glsl. The fold's own
  // pivot is position_type.xyz above (layer_fold_candidate()'s `pivot`),
  // so nothing beyond these two vec4s needs uploading for it.
  //
  // Carried per primitive rather than read from the Layer struct so that
  // every existing primitive_sdf() call site keeps working unchanged: the
  // bake's per-chunk candidate lists evaluate primitives one at a time,
  // outside any enclosing layer loop, and would otherwise each need to
  // pass a layer index down. Zero-init (None) is what a volumetric and a
  // primitive in an ordinary layer both get.
  f32 layer_repeat_mode_cell[4];
  // xyz = the layer's repetition count, per the comment above. w = this
  // primitive's BendAxis as a float (see Geometry::bend_axis) -- nothing to
  // do with layer repetition, just the last unused float in the struct, and
  // repurposed rather than growing every GpuPrimitive by another vec4, the
  // same call already made for repeat_count[3]'s bounding radius. Zero
  // (BendAxis::XToY, the classic bend) is what the value-initialised
  // default and every volumetric both leave here.
  f32 layer_repeat_count[4];
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

// M7 polish: matches `BrushPreviewBuffer` in Builtin.DeferredShade.comp.
// glsl field-for-field -- see set_brush_preview()'s own header comment
// (vulkan_raymarch_shader.h) for why this exists at all. Plain C arrays of
// f32/i32, like GpuLight/GpuLayer above, NOT glm::vec3/vec4 -- a bare vec3
// is exactly the kind of GLSL-vs-C++ alignment mismatch M1a cost real
// debugging time to root-cause (see the plan file's M1a section), so every
// field here is a full 16-byte vec4-equivalent, with any genuinely-unused
// component left as explicit padding rather than omitted.
struct GpuBrushPreview {
  f32 center_enabled[4]; // xyz = world-space center, w = 1.0f/0.0f enabled
                        // -- a float flag, not bool, matching every other
                        // packed boolean in this file (set_material()'s
                        // waxy flag, ScenePrimitiveMaterialScalars' exempt
                        // flag) for the same reason: bool's size/ABI is not
                        // guaranteed to match across the C++/GLSL boundary.
  f32 params[4];          // Same per-shape layout as stamp_shape()'s own
                        // `params` -- see Builtin.SculptStamp.comp.glsl's
                        // eval_brush_shape() table.
  i32 shape_op_slot[4];   // x = BrushShape, y = BrushOp, z = material slot,
                        // w = unused padding (kept explicit, not omitted,
                        // so sizeof() stays a whole number of vec4s without
                        // relying on the compiler to insert matching
                        // padding -- the exact class of silent mismatch
                        // M1a's root cause was).
};
static_assert(sizeof(GpuBrushPreview) == 48,
             "GpuBrushPreview must stay 3 vec4-sized members (48 bytes) -- "
             "see Builtin.DeferredShade.comp.glsl's BrushPreviewBuffer, "
             "which this struct's layout must match field-for-field.");

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

// expression_max_magnitude() -- the parametric-expression interval-analysis
// bound used only by rebuild_static_scene()'s primitive-bound refinement --
// was removed in the M0 CSG deletion pass along with that function and
// ExprOp/kMaxExprInstructions (engine/src/resources/expression.h, no
// longer included here).

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

// M4 (Cutout): a C++ port of Builtin.BrushShapes.inc.glsl's eval_brush_
// shape()/smooth_subtraction(), used ONLY by export_crop_outside() to
// compute the "outside" (removed) complement entirely on the CPU from a
// plain readback, with no second GPU dispatch needed for it (see that
// method's own comment). Kept in exact correspondence with the GLSL
// originals -- any shape formula changed there must change identically
// here, same manual-sync-point convention as every push-constant struct
// above. GLSL's sign() returns exactly 0.0 at 0.0 (unlike a naive
// branchless +-1); glsl_sign() below matches that, since two of the ported
// formulas (hex_prism_sdf/pyramid_sdf) depend on it.
namespace crop_cpu {

f32 glsl_sign(f32 x) {
  return (x > 0.0f) ? 1.0f : ((x < 0.0f) ? -1.0f : 0.0f);
}

f32 sphere_sdf(glm::vec3 p, f32 radius) { return glm::length(p) - radius; }

f32 box_sdf(glm::vec3 p, glm::vec3 half_extents) {
  glm::vec3 q = glm::abs(p) - half_extents;
  return glm::length(glm::max(q, glm::vec3(0.0f))) +
      std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
}

f32 plane_sdf(glm::vec3 p, f32 height) { return p.y - height; }

f32 torus_sdf(glm::vec3 p, f32 major_radius, f32 minor_radius) {
  glm::vec2 q(glm::length(glm::vec2(p.x, p.z)) - major_radius, p.y);
  return glm::length(q) - minor_radius;
}

f32 capped_cylinder_sdf(glm::vec3 p, f32 radius, f32 half_height) {
  glm::vec2 d = glm::abs(glm::vec2(glm::length(glm::vec2(p.x, p.z)), p.y)) -
      glm::vec2(radius, half_height);
  return std::min(std::max(d.x, d.y), 0.0f) +
      glm::length(glm::max(d, glm::vec2(0.0f)));
}

f32 capped_cone_sdf(glm::vec3 p, f32 half_height, f32 r1, f32 r2) {
  glm::vec2 q(glm::length(glm::vec2(p.x, p.z)), p.y);
  glm::vec2 k1(r2, half_height);
  glm::vec2 k2(r2 - r1, 2.0f * half_height);
  glm::vec2 ca(q.x - std::min(q.x, (q.y < 0.0f) ? r1 : r2),
              std::abs(q.y) - half_height);
  glm::vec2 cb = q - k1 +
      k2 * glm::clamp(glm::dot(k1 - q, k2) / glm::dot(k2, k2), 0.0f, 1.0f);
  f32 s = (cb.x < 0.0f && ca.y < 0.0f) ? -1.0f : 1.0f;
  return s * std::sqrt(std::min(glm::dot(ca, ca), glm::dot(cb, cb)));
}

f32 round_box_sdf(glm::vec3 p, glm::vec3 half_extents, f32 corner_radius) {
  glm::vec3 q = glm::abs(p) - half_extents + corner_radius;
  return glm::length(glm::max(q, glm::vec3(0.0f))) +
      std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f) - corner_radius;
}

f32 box_frame_sdf(glm::vec3 p, glm::vec3 half_extents, f32 edge_thickness) {
  glm::vec3 pp = glm::abs(p) - half_extents;
  glm::vec3 q = glm::abs(pp + edge_thickness) - edge_thickness;
  return std::min(std::min(
      glm::length(glm::max(glm::vec3(pp.x, q.y, q.z), glm::vec3(0.0f))) +
          std::min(std::max(pp.x, std::max(q.y, q.z)), 0.0f),
      glm::length(glm::max(glm::vec3(q.x, pp.y, q.z), glm::vec3(0.0f))) +
          std::min(std::max(q.x, std::max(pp.y, q.z)), 0.0f)),
      glm::length(glm::max(glm::vec3(q.x, q.y, pp.z), glm::vec3(0.0f))) +
          std::min(std::max(q.x, std::max(q.y, pp.z)), 0.0f));
}

f32 octahedron_sdf(glm::vec3 p, f32 s) {
  glm::vec3 q = glm::abs(p);
  f32 m = q.x + q.y + q.z - s;
  glm::vec3 r;
  if (3.0f * q.x < m) {
    r = q;
  } else if (3.0f * q.y < m) {
    r = glm::vec3(q.y, q.z, q.x);
  } else if (3.0f * q.z < m) {
    r = glm::vec3(q.z, q.x, q.y);
  } else {
    return m * 0.57735027f;
  }
  f32 k = glm::clamp(0.5f * (r.z - r.y + s), 0.0f, s);
  return glm::length(glm::vec3(r.x, r.y - s + k, r.z - k));
}

f32 pyramid_sdf(glm::vec3 p, f32 h, f32 base) {
  f32 b2 = base * base;
  f32 m2 = h * h + b2;
  f32 px = std::abs(p.x);
  f32 pz = std::abs(p.z);
  if (pz > px) {
    std::swap(px, pz);
  }
  px -= base;
  pz -= base;
  glm::vec3 q(pz, h * p.y - base * px, h * px + base * p.y);
  f32 s = std::max(-q.x, 0.0f);
  f32 t = glm::clamp((q.y - base * q.x) / (m2 + b2), 0.0f, 1.0f);
  // NOTE: the GLSL original reads `q.y - base * p.z` here, where p.z at
  // that point is q.x's own source value (p.xz was reassigned to (pz_final,
  // ...) only inside the vec3 q construction, never written back into p
  // itself) -- i.e. it is literally q.x's pre-construction value, `pz`
  // above. Substituted directly rather than left as a dangling `p.z`
  // reference, which would silently read the ORIGINAL (pre-abs/pre-swap)
  // p.z in a naive line-by-line port.
  f32 a = m2 * (q.x + s) * (q.x + s) + q.y * q.y;
  f32 bb = m2 * (q.x + base * t) * (q.x + base * t) +
      (q.y - m2 * t) * (q.y - m2 * t);
  f32 d2 = (std::min(q.y, -q.x * m2 - q.y * base) > 0.0f) ? 0.0f
                                                          : std::min(a, bb);
  return std::sqrt((d2 + q.z * q.z) / m2) * glsl_sign(std::max(q.z, -p.y));
}

f32 hex_prism_sdf(glm::vec3 p, f32 inradius, f32 half_height) {
  const glm::vec2 kxy(-0.8660254f, 0.5f);
  const f32 kz = 0.57735f;
  glm::vec3 q = glm::abs(p);
  glm::vec2 qxz(q.x, q.z);
  qxz -= 2.0f * std::min(glm::dot(kxy, qxz), 0.0f) * kxy;
  glm::vec2 d(
      glm::length(qxz - glm::vec2(glm::clamp(qxz.x, -kz * inradius, kz * inradius),
                                  inradius)) *
          glsl_sign(qxz.y - inradius),
      q.y - half_height);
  return std::min(std::max(d.x, d.y), 0.0f) +
      glm::length(glm::max(d, glm::vec2(0.0f)));
}

f32 round_cone_sdf(glm::vec3 p, f32 r1, f32 r2, f32 half_height) {
  p.y += half_height;
  f32 height = 2.0f * half_height;
  f32 b = (r1 - r2) / height;
  f32 a = std::sqrt(std::max(1.0f - b * b, 0.0f));
  glm::vec2 q(glm::length(glm::vec2(p.x, p.z)), p.y);
  f32 k = glm::dot(q, glm::vec2(-b, a));
  if (k < 0.0f) {
    return glm::length(q) - r1;
  }
  if (k > a * height) {
    return glm::length(q - glm::vec2(0.0f, height)) - r2;
  }
  return glm::dot(q, glm::vec2(a, b)) - r1;
}

f32 capsule_sdf(glm::vec3 p, f32 radius, f32 half_height) {
  p.y -= glm::clamp(p.y, -half_height, half_height);
  return glm::length(p) - radius;
}

f32 link_sdf(glm::vec3 p, f32 half_length, f32 r1, f32 r2) {
  glm::vec3 q(p.x, std::max(std::abs(p.y) - half_length, 0.0f), p.z);
  return glm::length(glm::vec2(glm::length(glm::vec2(q.x, q.y)) - r1, q.z)) - r2;
}

f32 ellipsoid_sdf(glm::vec3 p, glm::vec3 radii) {
  f32 k0 = glm::length(p / radii);
  f32 k1 = glm::length(p / (radii * radii));
  return k0 * (k0 - 1.0f) / k1;
}

f32 eval_brush_shape(glm::vec3 p, VulkanRaymarchShader::BrushShape shape_type,
                     glm::vec4 params) {
  using BrushShape = VulkanRaymarchShader::BrushShape;
  switch (shape_type) {
  case BrushShape::Sphere:
    return sphere_sdf(p, params.x);
  case BrushShape::Box:
    return box_sdf(p, glm::vec3(params));
  case BrushShape::Plane:
    return plane_sdf(p, params.x);
  case BrushShape::Torus:
    return torus_sdf(p, params.x, params.y);
  case BrushShape::CappedCylinder:
    return capped_cylinder_sdf(p, params.x, params.y);
  case BrushShape::CappedCone:
    return capped_cone_sdf(p, params.x, params.y, params.z);
  case BrushShape::RoundBox:
    return round_box_sdf(p, glm::vec3(params), params.w);
  case BrushShape::BoxFrame:
    return box_frame_sdf(p, glm::vec3(params), params.w);
  case BrushShape::Octahedron:
    return octahedron_sdf(p, params.x);
  case BrushShape::Pyramid:
    return pyramid_sdf(p, params.x, params.y);
  case BrushShape::HexPrism:
    return hex_prism_sdf(p, params.x, params.y);
  case BrushShape::RoundCone:
    return round_cone_sdf(p, params.x, params.y, params.z);
  case BrushShape::Capsule:
    return capsule_sdf(p, params.x, params.y);
  case BrushShape::Link:
    return link_sdf(p, params.x, params.y, params.z);
  case BrushShape::Ellipsoid:
    return ellipsoid_sdf(p, glm::vec3(params));
  default:
    return sphere_sdf(p, params.x);
  }
}

// Mirrors Builtin.BrushShapes.inc.glsl's smooth_subtraction() exactly (same
// cutter/base/k/out-h argument order and effect).
f32 smooth_subtraction(f32 cutter, f32 base, f32 k, f32 &h) {
  if (k <= 0.0001f || std::abs(base + cutter) >= k) {
    f32 neg_cutter = -cutter;
    h = (neg_cutter > base) ? 1.0f : 0.0f;
    return std::max(neg_cutter, base);
  }
  h = glm::clamp(0.5f - 0.5f * (base + cutter) / k, 0.0f, 1.0f);
  return glm::mix(base, -cutter, h) + k * h * (1.0f - h);
}

} // namespace crop_cpu

// M4 (Cutout): the on-disk snapshot format export_crop_outside()/load_
// crop_outside_file() share -- see either method's own header comment
// (vulkan_raymarch_shader.h) for why this is a brand-new, self-contained
// format rather than the tool's .stamps replayable-edit-list format. Plain
// binary, host-endian (this tool never moves a file between architectures),
// no compression -- an authoring-time export, not a shipped asset format.
constexpr u32 kCropOutsideMagic = 0x54435343u; // ASCII "CSCT".
constexpr u32 kCropOutsideVersion = 1;
struct CropOutsideFileHeader {
  u32 magic;
  u32 version;
  u32 brick_count;
};
struct CropOutsideBrickHeader {
  i32 chunk_x, chunk_y, chunk_z;
  i32 local_cell_index;
  i32 material;
};

} // namespace

VulkanRaymarchShader::VulkanRaymarchShader(VulkanContext &context)
    : context_(&context),
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
                        VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_sculpt_stamp_stage_(context, BUILTIN_SHADER_NAME_CHUNK_SCULPT_STAMP,
                               "comp", VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_sculpt_splat_regen_stage_(
          context, BUILTIN_SHADER_NAME_CHUNK_SCULPT_SPLAT_REGEN, "comp",
          VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_sculpt_undo_restore_stage_(
          context, BUILTIN_SHADER_NAME_CHUNK_SCULPT_UNDO_RESTORE, "comp",
          VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_sculpt_spraypaint_stage_(
          context, BUILTIN_SHADER_NAME_CHUNK_SCULPT_SPRAYPAINT, "comp",
          VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_sculpt_checkpoint_restore_stage_(
          context, BUILTIN_SHADER_NAME_CHUNK_SCULPT_CHECKPOINT_RESTORE, "comp",
          VK_SHADER_STAGE_COMPUTE_BIT),
      chunk_sculpt_crop_stage_(context, BUILTIN_SHADER_NAME_CHUNK_SCULPT_CROP,
                              "comp", VK_SHADER_STAGE_COMPUTE_BIT) {
  if (!chunk_debug_query_stage_.is_valid() || !chunk_evict_stage_.is_valid() ||
      !chunk_probe_bake_stage_.is_valid() ||
      !chunk_cluster_cull_stage_.is_valid() ||
      !chunk_shadow_splat_stage_.is_valid() ||
      !chunk_voxel_cascade_stage_.is_valid() ||
      !stochastic_ao_stage_.is_valid() ||
      !chunk_point_splat_stage_.is_valid() ||
      !probe_bake_stage_.is_valid() ||
      !render_stage_.is_valid() || !deferred_shade_stage_.is_valid() ||
      !bloom_blur_h_stage_.is_valid() ||
      !post_composite_stage_.is_valid() || !taa_resolve_stage_.is_valid() ||
      !chunk_sculpt_stamp_stage_.is_valid() ||
      !chunk_sculpt_splat_regen_stage_.is_valid() ||
      !chunk_sculpt_undo_restore_stage_.is_valid() ||
      !chunk_sculpt_spraypaint_stage_.is_valid() ||
      !chunk_sculpt_checkpoint_restore_stage_.is_valid() ||
      !chunk_sculpt_crop_stage_.is_valid()) {
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
  // TRANSFER_DST, on top of the STORAGE_BUFFER every other reader/writer
  // needs: stamp_shape() vkCmdFillBuffer()s a freshly-claimed
  // chunk's sub-block to -1 (see its own comment for why) before the
  // stamp shader writes real brick indices into the cells it actually
  // touches -- VUID-vkCmdFillBuffer-dstBuffer-00029 requires the
  // destination buffer to have been created with this bit. TRANSFER_SRC,
  // on top of that: M4b's VulkanRaymarchShader::capture_sculpt_checkpoint()
  // reads this buffer's level-0 sub-range back to the CPU (to learn which
  // cells hold a brick at all) via a device-local-to-staging vkCmdCopyBuffer
  // -- see its own comment.
  chunk_indirection_buffer_.emplace(*context_, chunk_indirection_size,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  // TRANSFER_SRC: M4b's capture_sculpt_checkpoint() reads every resident
  // brick's full voxel array back to the CPU (batched into one multi-region
  // copy -- see VulkanBuffer::copy_regions_to()) so a checkpoint can later
  // be replayed from regular process memory, exactly like undo_stack_
  // already keeps its own (smaller, per-stamp) snapshots there.
  chunk_brick_pool_buffer_.emplace(*context_, chunk_brick_pool_size,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  // TRANSFER_SRC: same reasoning as chunk_brick_pool_buffer_ above -- a
  // checkpoint also needs each resident brick's material id.
  chunk_brick_primitive_buffer_.emplace(*context_, chunk_brick_primitive_size,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  // M3 (Looseness): same size/usage as chunk_brick_primitive_buffer_
  // immediately above -- one f32 per brick slot, same TRANSFER_SRC for
  // capture_sculpt_checkpoint()'s own readback. See its own header comment
  // (vulkan_raymarch_shader.h) for the full data-path story.
  chunk_brick_looseness_buffer_.emplace(*context_, chunk_brick_primitive_size,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  // M3c (Spraypaint): same size as chunk_brick_pool_buffer_ above -- see
  // chunk_brick_paint_buffer_'s own comment, vulkan_raymarch_shader.h.
  // TRANSFER_DST so reset_chunked_field() can vkCmdFillBuffer() it to all-
  // zero once, the same way chunk_indirection_buffer_ is cleared, instead
  // of needing a ~1.9GB CPU-side zero array for upload_to_device_local().
  chunk_brick_paint_buffer_.emplace(*context_, chunk_brick_pool_size,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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
  // Disk-cache staging: host-visible so the CPU can read a gathered chunk
  // straight out of it and write it to disk, and upload a restored one back
  // the same way, with no extra copy.
  chunk_cache_staging_buffer_.emplace(
      *context_,
      static_cast<u64>(kChunkCacheWords) * kChunkCacheStagingRegions *
          sizeof(u32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  chunk_evict_batch_buffer_.emplace(
      *context_,
      static_cast<u64>(kMaxChunkBatchSize) * kAsyncRingDepth * sizeof(i32),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // M1a/M2a: stamp_shape()'s own per-cell scratch -- host-visible, like the
  // batch buffers above, since it's rewritten wholesale from the CPU every
  // call and read by both Builtin.SculptStamp.comp.glsl and Builtin.
  // SculptSplatRegen.comp.glsl.
  chunk_sculpt_cell_buffer_.emplace(
      *context_,
      static_cast<u64>(kMaxSculptStampCellsPerCall) * sizeof(GpuSculptStampCell),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // M2b: Builtin.SculptStamp.comp.glsl's undo-capture scratch -- host-
  // visible/host-coherent, same reasoning as chunk_sculpt_cell_buffer_ just
  // above (stamp_shape() locks() these straight back after its one
  // synchronous dispatch, no staging-buffer copy needed). See kMaxSculpt
  // UndoCellsPerStamp's own comment for why this is sized smaller than
  // kMaxSculptStampCellsPerCall rather than matching it.
  chunk_sculpt_undo_capture_meta_buffer_.emplace(
      *context_,
      static_cast<u64>(kMaxSculptUndoCellsPerStamp) * sizeof(GpuUndoCaptureMeta),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  chunk_sculpt_undo_capture_voxel_buffer_.emplace(
      *context_,
      static_cast<u64>(kMaxSculptUndoCellsPerStamp) * kChunkBrickVoxelCount *
          sizeof(f32),
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
      !chunk_brick_looseness_buffer_->is_valid() ||
      !chunk_brick_demand_buffer_->is_valid() ||
      !chunk_brick_free_list_buffer_->is_valid() ||
      !chunk_brick_free_list_top_buffer_->is_valid() ||
      !chunk_debug_query_output_buffer_->is_valid() ||
      !chunk_voxelize_batch_slot_buffer_->is_valid() ||
      !chunk_voxelize_batch_data_buffer_->is_valid() ||
      !chunk_cache_staging_buffer_->is_valid() ||
      !chunk_evict_batch_buffer_->is_valid() ||
      !chunk_sculpt_cell_buffer_->is_valid() ||
      !chunk_sculpt_undo_capture_meta_buffer_->is_valid() ||
      !chunk_sculpt_undo_capture_voxel_buffer_->is_valid() ||
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

  // One vec4 (pixelation_exempt, bump_strength, casts_shadow, _) per
  // primitive -- see material_scalar_buffer_'s own comment.
  const u64 material_scalar_buffer_size =
      static_cast<u64>(kMaxScenePrimitives) * sizeof(f32) * 4;
  material_scalar_buffer_.emplace(
      *context_, material_scalar_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // One vec4 (xyz = Beer-Lambert absorption coefficient, w = index of
  // refraction) per primitive -- see transmission_buffer_'s own comment.
  const u64 transmission_buffer_size =
      static_cast<u64>(kMaxScenePrimitives) * sizeof(f32) * 4;
  transmission_buffer_.emplace(
      *context_, transmission_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
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
      !probe_buffer_b_->is_valid() || !material_scalar_buffer_->is_valid() ||
      !transmission_buffer_->is_valid() ||
      !tex_transform_buffer_->is_valid()) {
    KERROR("Failed to create sparse voxel field buffers.");
    return;
  }

  // The voxelize_set_layout_/chunk_voxelize_set_layout_ descriptor set
  // layouts (pass 1's primitive-driven fixed-cube bake, and the chunked
  // field's primitive-scanning voxelize pass) were removed here (M0 CSG
  // deletion pass, see /home/h-i/.claude/plans/joyful-skipping-giraffe.md)
  // along with Builtin.RaymarchVoxelize.comp.glsl/Builtin.ChunkVoxelize.
  // comp.glsl themselves.

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
  // 5=cull_args, 6=chunk_table, 7=chunk_slot_published} -- must match
  // Builtin.ChunkPointSplat.comp.glsl's bindings exactly. Binding 3 is
  // re-pointed by rebind_render_target_descriptors() whenever
  // splat_visibility_buffer_ is recreated (resize/render-scale change).
  //
  // 6 and 7 exist so finest_level_at() can ask whether a clip level is
  // actually RESIDENT at a point, not merely in-window -- see its comment
  // in the shader for the holes that gating on the window alone punched.
  VkDescriptorSetLayoutBinding chunk_point_splat_bindings[8]{};
  for (u32 i = 0; i < 8; ++i) {
    chunk_point_splat_bindings[i].binding = i;
    chunk_point_splat_bindings[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_point_splat_bindings[i].descriptorCount = 1;
    chunk_point_splat_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_point_splat_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_point_splat_layout_info.bindingCount = 8;
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
  VkDescriptorSetLayoutBinding chunk_shadow_splat_bindings[8]{};
  for (u32 i = 0; i < 8; ++i) {
    chunk_shadow_splat_bindings[i].binding = i;
    chunk_shadow_splat_bindings[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_shadow_splat_bindings[i].descriptorCount = 1;
    chunk_shadow_splat_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_shadow_splat_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_shadow_splat_layout_info.bindingCount = 8;
  chunk_shadow_splat_layout_info.pBindings = chunk_shadow_splat_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_shadow_splat_layout_info,
                                       context_->allocator,
                                       &chunk_shadow_splat_set_layout_));

  // chunk_voxel_cascade_set_ -- see its writes below.
  VkDescriptorSetLayoutBinding chunk_voxel_cascade_bindings[7]{};
  for (u32 i = 0; i < 7; ++i) {
    chunk_voxel_cascade_bindings[i].binding = i;
    chunk_voxel_cascade_bindings[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_voxel_cascade_bindings[i].descriptorCount = 1;
    chunk_voxel_cascade_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_voxel_cascade_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_voxel_cascade_layout_info.bindingCount = 7;
  chunk_voxel_cascade_layout_info.pBindings = chunk_voxel_cascade_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_voxel_cascade_layout_info,
                                       context_->allocator,
                                       &chunk_voxel_cascade_set_layout_));

  // M1a/M2b/M3c/M3 (Looseness): chunk_sculpt_stamp_set_: {0=StampCellBuffer,
  // 1=chunk_indirection, 2=chunk_brick_pool, 3=chunk_brick_primitive,
  // 4=brick_free_list, 5=brick_free_list_top, 6=UndoCaptureMetaBuffer,
  // 7=UndoCaptureVoxelBuffer, 8=ChunkBrickPaintBuffer, 9=
  // ChunkBrickLoosenessBuffer} -- must match Builtin.SculptStamp.comp.glsl's
  // AND Builtin.SculptCheckpointRestore.comp.glsl's bindings exactly (both
  // reuse this one layout -- see chunk_sculpt_checkpoint_restore_pipeline_'s
  // own comment; 6/7 added in M2b for undo capture, 8 added in M3c purely so
  // a freshly allocated brick's paint voxels can be zeroed at alloc time,
  // 9 added for Looseness -- same per-brick write/restore shape as binding
  // 3's material, see that binding's own comment in each shader).
  VkDescriptorSetLayoutBinding chunk_sculpt_stamp_bindings[10]{};
  for (u32 i = 0; i < 10; ++i) {
    chunk_sculpt_stamp_bindings[i].binding = i;
    chunk_sculpt_stamp_bindings[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_sculpt_stamp_bindings[i].descriptorCount = 1;
    chunk_sculpt_stamp_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_sculpt_stamp_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_sculpt_stamp_layout_info.bindingCount = 10;
  chunk_sculpt_stamp_layout_info.pBindings = chunk_sculpt_stamp_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_sculpt_stamp_layout_info,
                                       context_->allocator,
                                       &chunk_sculpt_stamp_set_layout_));

  // M1a: chunk_sculpt_splat_regen_set_: {0=StampCellBuffer (same buffer as
  // above), 1=chunk_indirection, 2=chunk_brick_pool, 3=cluster_free_list,
  // 4=cluster_free_top, 5=cluster_points, 6=chunk_clusters,
  // 7=brick_clusters, 8=chunk_brick_demand} -- must match Builtin.
  // SculptSplatRegen.comp.glsl's bindings exactly.
  VkDescriptorSetLayoutBinding chunk_sculpt_splat_regen_bindings[9]{};
  for (u32 i = 0; i < 9; ++i) {
    chunk_sculpt_splat_regen_bindings[i].binding = i;
    chunk_sculpt_splat_regen_bindings[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_sculpt_splat_regen_bindings[i].descriptorCount = 1;
    chunk_sculpt_splat_regen_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_sculpt_splat_regen_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_sculpt_splat_regen_layout_info.bindingCount = 9;
  chunk_sculpt_splat_regen_layout_info.pBindings = chunk_sculpt_splat_regen_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_sculpt_splat_regen_layout_info,
                                       context_->allocator,
                                       &chunk_sculpt_splat_regen_set_layout_));

  // M2b/M3 (Looseness): chunk_sculpt_undo_restore_set_: {0=StampCellBuffer
  // (same buffer as the two sets above), 1=chunk_indirection,
  // 2=chunk_brick_pool, 3=chunk_brick_primitive, 4=brick_free_list,
  // 5=brick_free_list_top, 6=brick_clusters, 7=chunk_clusters,
  // 8=cluster_free_list, 9=cluster_free_top, 10=UndoCaptureMetaBuffer,
  // 11=UndoCaptureVoxelBuffer, 12=ChunkBrickLoosenessBuffer (NEW)} -- must
  // match Builtin.SculptUndoRestore.comp.glsl's bindings exactly.
  VkDescriptorSetLayoutBinding chunk_sculpt_undo_restore_bindings[13]{};
  for (u32 i = 0; i < 13; ++i) {
    chunk_sculpt_undo_restore_bindings[i].binding = i;
    chunk_sculpt_undo_restore_bindings[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_sculpt_undo_restore_bindings[i].descriptorCount = 1;
    chunk_sculpt_undo_restore_bindings[i].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_sculpt_undo_restore_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_sculpt_undo_restore_layout_info.bindingCount = 13;
  chunk_sculpt_undo_restore_layout_info.pBindings =
      chunk_sculpt_undo_restore_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_sculpt_undo_restore_layout_info,
                                       context_->allocator,
                                       &chunk_sculpt_undo_restore_set_layout_));

  // M1a's own small, dedicated pool -- deliberately NOT chunk_descriptor_
  // pool_ above (same "fully decoupled descriptor lifetime" reasoning its
  // own comment gives, and it avoids touching that pool's own fragile,
  // hand-added-up descriptorCount term -- see its comment on why a term
  // drifting from its set's real binding count silently under-sizes every
  // set allocated after it). M2b added chunk_sculpt_undo_restore_set_'s own
  // 12 bindings to the same pool; M3 (Looseness) added one more binding to
  // BOTH chunk_sculpt_stamp_set_ (9->10) and chunk_sculpt_undo_restore_set_
  // (12->13).
  VkDescriptorPoolSize sculpt_pool_size{};
  sculpt_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  sculpt_pool_size.descriptorCount =
      10 + 9 + 13; // chunk_sculpt_stamp_set_ (8 -> 9 in M3c -> 10 in M3) +
                  // chunk_sculpt_splat_regen_set_ +
                  // chunk_sculpt_undo_restore_set_ (12 -> 13 in M3)
  VkDescriptorPoolCreateInfo sculpt_pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  sculpt_pool_info.poolSizeCount = 1;
  sculpt_pool_info.pPoolSizes = &sculpt_pool_size;
  sculpt_pool_info.maxSets = 3;
  VK_CHECK(vkCreateDescriptorPool(context_->device.logical_device,
                                  &sculpt_pool_info, context_->allocator,
                                  &sculpt_descriptor_pool_));

  VkDescriptorSetLayout sculpt_layouts[3] = {
      chunk_sculpt_stamp_set_layout_, chunk_sculpt_splat_regen_set_layout_,
      chunk_sculpt_undo_restore_set_layout_};
  VkDescriptorSet sculpt_sets[3];
  VkDescriptorSetAllocateInfo sculpt_alloc_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  sculpt_alloc_info.descriptorPool = sculpt_descriptor_pool_;
  sculpt_alloc_info.descriptorSetCount = 3;
  sculpt_alloc_info.pSetLayouts = sculpt_layouts;
  VK_CHECK(vkAllocateDescriptorSets(context_->device.logical_device,
                                    &sculpt_alloc_info, sculpt_sets));
  chunk_sculpt_stamp_set_ = sculpt_sets[0];
  chunk_sculpt_splat_regen_set_ = sculpt_sets[1];
  chunk_sculpt_undo_restore_set_ = sculpt_sets[2];

  VkDescriptorBufferInfo chunk_sculpt_stamp_buffer_infos[10] = {
      {chunk_sculpt_cell_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_indirection_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_pool_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_free_list_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_free_list_top_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_sculpt_undo_capture_meta_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_sculpt_undo_capture_voxel_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_paint_buffer_->handle(), 0, VK_WHOLE_SIZE}, // M3c, binding 8.
      {chunk_brick_looseness_buffer_->handle(), 0, VK_WHOLE_SIZE}, // M3, binding 9.
  };
  VkWriteDescriptorSet chunk_sculpt_stamp_writes[10]{};
  for (u32 i = 0; i < 10; ++i) {
    chunk_sculpt_stamp_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_sculpt_stamp_writes[i].dstSet = chunk_sculpt_stamp_set_;
    chunk_sculpt_stamp_writes[i].dstBinding = i;
    chunk_sculpt_stamp_writes[i].descriptorCount = 1;
    chunk_sculpt_stamp_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_sculpt_stamp_writes[i].pBufferInfo = &chunk_sculpt_stamp_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 10,
                        chunk_sculpt_stamp_writes, 0, nullptr);

  VkDescriptorBufferInfo chunk_sculpt_splat_regen_buffer_infos[9] = {
      {chunk_sculpt_cell_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_indirection_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_pool_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_free_list_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_free_top_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_point_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_demand_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_sculpt_splat_regen_writes[9]{};
  for (u32 i = 0; i < 9; ++i) {
    chunk_sculpt_splat_regen_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_sculpt_splat_regen_writes[i].dstSet = chunk_sculpt_splat_regen_set_;
    chunk_sculpt_splat_regen_writes[i].dstBinding = i;
    chunk_sculpt_splat_regen_writes[i].descriptorCount = 1;
    chunk_sculpt_splat_regen_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_sculpt_splat_regen_writes[i].pBufferInfo = &chunk_sculpt_splat_regen_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 9,
                        chunk_sculpt_splat_regen_writes, 0, nullptr);

  VkDescriptorBufferInfo chunk_sculpt_undo_restore_buffer_infos[13] = {
      {chunk_sculpt_cell_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_indirection_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_pool_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_free_list_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_free_list_top_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_free_list_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_free_top_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_sculpt_undo_capture_meta_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_sculpt_undo_capture_voxel_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_looseness_buffer_->handle(), 0, VK_WHOLE_SIZE}, // M3, binding 12.
  };
  VkWriteDescriptorSet chunk_sculpt_undo_restore_writes[13]{};
  for (u32 i = 0; i < 13; ++i) {
    chunk_sculpt_undo_restore_writes[i].sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_sculpt_undo_restore_writes[i].dstSet = chunk_sculpt_undo_restore_set_;
    chunk_sculpt_undo_restore_writes[i].dstBinding = i;
    chunk_sculpt_undo_restore_writes[i].descriptorCount = 1;
    chunk_sculpt_undo_restore_writes[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_sculpt_undo_restore_writes[i].pBufferInfo =
        &chunk_sculpt_undo_restore_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 13,
                        chunk_sculpt_undo_restore_writes, 0, nullptr);

  // M3c: chunk_sculpt_spraypaint_set_: {0=StampCellBuffer (same buffer as
  // every sculpt set above), 1=chunk_indirection (read-only -- spray_paint()
  // never allocates a brick, only checks whether one already exists),
  // 2=ChunkBrickPaintBuffer (NEW, read-write)} -- must match Builtin.
  // SculptSpraypaint.comp.glsl's bindings exactly.
  VkDescriptorSetLayoutBinding chunk_sculpt_spraypaint_bindings[3]{};
  for (u32 i = 0; i < 3; ++i) {
    chunk_sculpt_spraypaint_bindings[i].binding = i;
    chunk_sculpt_spraypaint_bindings[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_sculpt_spraypaint_bindings[i].descriptorCount = 1;
    chunk_sculpt_spraypaint_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo chunk_sculpt_spraypaint_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  chunk_sculpt_spraypaint_layout_info.bindingCount = 3;
  chunk_sculpt_spraypaint_layout_info.pBindings = chunk_sculpt_spraypaint_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &chunk_sculpt_spraypaint_layout_info,
                                       context_->allocator,
                                       &chunk_sculpt_spraypaint_set_layout_));

  // M7 polish: brush_preview_buffer_ -- the single small UBO behind
  // set_brush_preview() (see its own header comment, vulkan_raymarch_
  // shader.h). Host-visible/host-coherent, like primitive_colour_buffer_
  // etc. above: it is rewritten by a direct CPU load_data() every frame
  // the brush-cursor preview is visible, never touched by the GPU. Created
  // here, right before paint_read_set_layout_ below, since binding 1 of
  // that set is the only thing that ever reads it.
  brush_preview_buffer_.emplace(
      *context_, sizeof(GpuBrushPreview), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  {
    // Default-safe contents until the first real set_brush_preview() call:
    // enabled = 0.0f, so Builtin.DeferredShade.comp.glsl's early-out skips
    // the preview trace entirely rather than reading whatever uninitialized
    // bytes a fresh host-visible allocation happens to hold.
    const GpuBrushPreview disabled_preview{};
    brush_preview_buffer_->load_data(0, sizeof(GpuBrushPreview), 0,
                                     &disabled_preview);
  }

  // M3c: paint_read_set_: {0=ChunkBrickPaintBuffer (the SAME buffer as
  // above, read-only here)} -- the tiny, SEPARATE set (bound as set = 1)
  // Builtin.DeferredShade.comp.glsl's pipeline binds alongside render_set_
  // (set = 0) to read the paint accumulator -- see this file's deferred_
  // shade_pipeline_ construction and ChunkBrickPaintBuffer's own comment in
  // the shader for why this, not widening render_set_, is how the data
  // gets there. render_pipeline_ (the visibility pass) never binds this --
  // its pipeline layout stays exactly {render_set_layout_}, unchanged.
  // M7 polish ADDS binding 1 = BrushPreviewBuffer (brush_preview_buffer_
  // above) to this SAME set, rather than a third one -- see paint_read_
  // set_layout_'s own header comment (vulkan_raymarch_shader.h) for why.
  // M7 polish's TAA-ghosting FIX further ADDS binding 2 =
  // preview_overlay_image_ (writeonly -- see its own declaration comment)
  // to this same set, same reasoning again: this is deferred_shade_
  // pipeline_'s existing additive set, and new shading-only output belongs
  // here, not a fourth set. M3 (Looseness) ADDS binding 3 =
  // chunk_brick_looseness_buffer_ (read-only here -- the write side is
  // Builtin.SculptStamp.comp.glsl/Builtin.SculptCheckpointRestore.comp.
  // glsl/Builtin.SculptUndoRestore.comp.glsl's own chunk_sculpt_stamp_set_/
  // chunk_sculpt_undo_restore_set_ bindings) -- render_set_ is confirmed
  // already fully packed at 29/29 bindings, so this per-brick buffer goes
  // on this set instead, same precedent as the three additions above it.
  VkDescriptorSetLayoutBinding paint_read_bindings[4]{};
  paint_read_bindings[0].binding = 0;
  paint_read_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  paint_read_bindings[0].descriptorCount = 1;
  paint_read_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  paint_read_bindings[1].binding = 1;
  paint_read_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  paint_read_bindings[1].descriptorCount = 1;
  paint_read_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  paint_read_bindings[2].binding = 2;
  paint_read_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  paint_read_bindings[2].descriptorCount = 1;
  paint_read_bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  paint_read_bindings[3].binding = 3;
  paint_read_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  paint_read_bindings[3].descriptorCount = 1;
  paint_read_bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo paint_read_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  paint_read_layout_info.bindingCount = 4;
  paint_read_layout_info.pBindings = paint_read_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &paint_read_layout_info,
                                       context_->allocator,
                                       &paint_read_set_layout_));

  // M3c's own small, dedicated pool -- same "fully decoupled descriptor
  // lifetime" reasoning sculpt_descriptor_pool_'s own comment gives, kept
  // separate from it too since paint_read_set_ is consumed by a completely
  // different pipeline (deferred_shade_pipeline_, an existing per-frame
  // shader) than every set in sculpt_descriptor_pool_ (authoring-time-only
  // dispatches) -- mixing the two pools' lifetimes/semantics would be
  // confusing even though Vulkan itself has no objection. M7 polish adds a
  // SECOND pool-size entry: brush_preview_buffer_'s binding is a UNIFORM_
  // BUFFER, a different VkDescriptorType than every STORAGE_BUFFER binding
  // already in this pool, and Vulkan requires one VkDescriptorPoolSize
  // entry per distinct type. The TAA-ghosting fix adds a THIRD: binding 2
  // (preview_overlay_image_) is a STORAGE_IMAGE, a third distinct type.
  VkDescriptorPoolSize paint_pool_sizes[3]{};
  paint_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  paint_pool_sizes[0].descriptorCount =
      3 + 1 + 1; // chunk_sculpt_spraypaint_set_ + paint_read_set_'s
                // binding 0 + binding 3 (M3 Looseness, NEW)
  paint_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  paint_pool_sizes[1].descriptorCount = 1; // paint_read_set_'s binding 1
                                          // (BrushPreviewBuffer)
  paint_pool_sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  paint_pool_sizes[2].descriptorCount = 1; // paint_read_set_'s binding 2
                                          // (preview_overlay_image_)
  VkDescriptorPoolCreateInfo paint_pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  paint_pool_info.poolSizeCount = 3;
  paint_pool_info.pPoolSizes = paint_pool_sizes;
  paint_pool_info.maxSets = 2;
  VK_CHECK(vkCreateDescriptorPool(context_->device.logical_device,
                                  &paint_pool_info, context_->allocator,
                                  &paint_descriptor_pool_));

  VkDescriptorSetLayout paint_layouts[2] = {chunk_sculpt_spraypaint_set_layout_,
                                            paint_read_set_layout_};
  VkDescriptorSet paint_sets[2];
  VkDescriptorSetAllocateInfo paint_alloc_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  paint_alloc_info.descriptorPool = paint_descriptor_pool_;
  paint_alloc_info.descriptorSetCount = 2;
  paint_alloc_info.pSetLayouts = paint_layouts;
  VK_CHECK(vkAllocateDescriptorSets(context_->device.logical_device,
                                    &paint_alloc_info, paint_sets));
  chunk_sculpt_spraypaint_set_ = paint_sets[0];
  paint_read_set_ = paint_sets[1];

  VkDescriptorBufferInfo chunk_sculpt_spraypaint_buffer_infos[3] = {
      {chunk_sculpt_cell_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_indirection_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_paint_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_sculpt_spraypaint_writes[3]{};
  for (u32 i = 0; i < 3; ++i) {
    chunk_sculpt_spraypaint_writes[i].sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_sculpt_spraypaint_writes[i].dstSet = chunk_sculpt_spraypaint_set_;
    chunk_sculpt_spraypaint_writes[i].dstBinding = i;
    chunk_sculpt_spraypaint_writes[i].descriptorCount = 1;
    chunk_sculpt_spraypaint_writes[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_sculpt_spraypaint_writes[i].pBufferInfo =
        &chunk_sculpt_spraypaint_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 3,
                        chunk_sculpt_spraypaint_writes, 0, nullptr);

  VkDescriptorBufferInfo paint_read_buffer_infos[3] = {
      {chunk_brick_paint_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {brush_preview_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_looseness_buffer_->handle(), 0, VK_WHOLE_SIZE}, // M3, binding 3.
  };
  VkDescriptorImageInfo paint_read_preview_image_info{
      VK_NULL_HANDLE, preview_overlay_image_.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet paint_read_writes[4]{};
  paint_read_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  paint_read_writes[0].dstSet = paint_read_set_;
  paint_read_writes[0].dstBinding = 0;
  paint_read_writes[0].descriptorCount = 1;
  paint_read_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  paint_read_writes[0].pBufferInfo = &paint_read_buffer_infos[0];
  // M7 polish: binding 1 = BrushPreviewBuffer, the UNIFORM_BUFFER-typed
  // addition -- see paint_read_bindings[1] above for why this is a
  // different descriptor type than binding 0.
  paint_read_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  paint_read_writes[1].dstSet = paint_read_set_;
  paint_read_writes[1].dstBinding = 1;
  paint_read_writes[1].descriptorCount = 1;
  paint_read_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  paint_read_writes[1].pBufferInfo = &paint_read_buffer_infos[1];
  // TAA-ghosting fix: binding 2 = preview_overlay_image_, the STORAGE_IMAGE
  // addition -- also re-pointed by rebind_render_target_descriptors()
  // whenever this image is recreated on resize (it's a render target).
  paint_read_writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  paint_read_writes[2].dstSet = paint_read_set_;
  paint_read_writes[2].dstBinding = 2;
  paint_read_writes[2].descriptorCount = 1;
  paint_read_writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  paint_read_writes[2].pImageInfo = &paint_read_preview_image_info;
  // M3 (Looseness): binding 3 = chunk_brick_looseness_buffer_, read-only
  // here -- same STORAGE_BUFFER type as binding 0, different buffer.
  paint_read_writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  paint_read_writes[3].dstSet = paint_read_set_;
  paint_read_writes[3].dstBinding = 3;
  paint_read_writes[3].descriptorCount = 1;
  paint_read_writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  paint_read_writes[3].pBufferInfo = &paint_read_buffer_infos[2];
  vkUpdateDescriptorSets(context_->device.logical_device, 4, paint_read_writes,
                        0, nullptr);

  VkDescriptorPoolSize chunk_pool_size{};
  chunk_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  // chunk_debug_query_set_ + chunk_evict_set_ + chunk_probe_bake_set_/_odd_
  // (x2, like probe_bake_set_/_odd_ above) + chunk_point_splat_set_.
  // chunk_voxelize_set_'s own term (21) was removed along with that set
  // (M0 CSG deletion pass -- see /home/h-i/.claude/plans/
  // joyful-skipping-giraffe.md).
  // One term per set, in the order chunk_layouts[] allocates them, and each
  // term MUST equal that set's binding count -- a set whose bindings grow
  // without its term growing here under-sizes the pool, and the sets
  // allocated after it are the ones that come up short. (The point-splat
  // term went 6 -> 8 when finest_level_at() gained the chunk table and the
  // published flags.)
  chunk_pool_size.descriptorCount = 5 + 8 + 8 * 2 + 8 + 9 + 8 + 7;
  VkDescriptorPoolCreateInfo chunk_pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  chunk_pool_info.poolSizeCount = 1;
  chunk_pool_info.pPoolSizes = &chunk_pool_size;
  chunk_pool_info.maxSets = 8;
  VK_CHECK(vkCreateDescriptorPool(context_->device.logical_device,
                                  &chunk_pool_info, context_->allocator,
                                  &chunk_descriptor_pool_));

  VkDescriptorSetLayout chunk_layouts[8] = {
      chunk_debug_query_set_layout_,
      chunk_evict_set_layout_,         chunk_probe_bake_set_layout_,
      chunk_probe_bake_set_layout_,    chunk_point_splat_set_layout_,
      chunk_cluster_cull_set_layout_,  chunk_shadow_splat_set_layout_,
      chunk_voxel_cascade_set_layout_};
  VkDescriptorSet chunk_sets[8];
  VkDescriptorSetAllocateInfo chunk_alloc_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  chunk_alloc_info.descriptorPool = chunk_descriptor_pool_;
  chunk_alloc_info.descriptorSetCount = 8;
  chunk_alloc_info.pSetLayouts = chunk_layouts;
  VK_CHECK(vkAllocateDescriptorSets(context_->device.logical_device,
                                    &chunk_alloc_info, chunk_sets));
  chunk_debug_query_set_ = chunk_sets[0];
  chunk_evict_set_ = chunk_sets[1];
  chunk_probe_bake_set_ = chunk_sets[2];
  chunk_probe_bake_set_odd_ = chunk_sets[3];
  chunk_point_splat_set_ = chunk_sets[4];
  chunk_cluster_cull_set_ = chunk_sets[5];
  chunk_shadow_splat_set_ = chunk_sets[6];
  chunk_voxel_cascade_set_ = chunk_sets[7];

  // The chunk_voxelize_set_'s own buffer-info/write-descriptor population
  // (21 bindings: chunk_indirection/chunk_brick_pool/.../chunk_cache_
  // staging) was removed here along with that set -- every one of those
  // buffers that the KEPT pipelines (evict/cluster-cull/point-splat/etc.)
  // still read/write gets its own descriptor write at ITS set's own
  // population site elsewhere in this constructor; nothing here needs to
  // populate a set that no longer exists.

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
  VkDescriptorBufferInfo chunk_point_splat_buffer_infos[8] = {
      {chunk_cluster_point_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {splat_visibility_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {splat_tile_bound_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_visible_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cull_args_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_table_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_slot_published_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_point_splat_writes[8]{};
  for (u32 i = 0; i < 8; ++i) {
    chunk_point_splat_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_point_splat_writes[i].dstSet = chunk_point_splat_set_;
    chunk_point_splat_writes[i].dstBinding = i;
    chunk_point_splat_writes[i].descriptorCount = 1;
    chunk_point_splat_writes[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_point_splat_writes[i].pBufferInfo = &chunk_point_splat_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 8,
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
  // 2=resident_clusters, 3=resident_args, 4=voxel_cascades,
  // 5=chunk_brick_primitive, 6=material_scalars} -- must match
  // Builtin.ChunkVoxelCascade.comp.glsl. The last two are only there for
  // Material::casts_shadow: a non-casting primitive is left out of the
  // cascades entirely, which is what stops it occluding ambient light --
  // same pair, for the same reason, as chunk_shadow_splat_set_'s 6/7.
  VkDescriptorBufferInfo chunk_voxel_cascade_buffer_infos[7] = {
      {chunk_cluster_point_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_resident_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_resident_args_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {voxel_cascade_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {material_scalar_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_voxel_cascade_writes[7]{};
  for (u32 i = 0; i < 7; ++i) {
    chunk_voxel_cascade_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_voxel_cascade_writes[i].dstSet = chunk_voxel_cascade_set_;
    chunk_voxel_cascade_writes[i].dstBinding = i;
    chunk_voxel_cascade_writes[i].descriptorCount = 1;
    chunk_voxel_cascade_writes[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_voxel_cascade_writes[i].pBufferInfo =
        &chunk_voxel_cascade_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 7,
                        chunk_voxel_cascade_writes, 0, nullptr);

  // chunk_shadow_splat_set_: {0=chunk_cluster_point, 1=chunk_cluster,
  // 2=light_buffer, 3=shadow_atlas, 4=shadow_pairs, 5=shadow_args,
  // 6=chunk_brick_primitive, 7=material_scalars} -- must match
  // Builtin.ChunkShadowSplat.comp.glsl.
  VkDescriptorBufferInfo chunk_shadow_splat_buffer_infos[8] = {
      {chunk_cluster_point_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_cluster_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {light_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {shadow_atlas_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_shadow_pair_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_shadow_args_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {chunk_brick_primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {material_scalar_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet chunk_shadow_splat_writes[8]{};
  for (u32 i = 0; i < 8; ++i) {
    chunk_shadow_splat_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunk_shadow_splat_writes[i].dstSet = chunk_shadow_splat_set_;
    chunk_shadow_splat_writes[i].dstBinding = i;
    chunk_shadow_splat_writes[i].descriptorCount = 1;
    chunk_shadow_splat_writes[i].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunk_shadow_splat_writes[i].pBufferInfo =
        &chunk_shadow_splat_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 8,
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
  VkDescriptorSetLayoutBinding render_bindings[29]{};
  for (u32 i = 0; i < 29; ++i) {
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
  // 28 = per-primitive transmission (absorption + IOR), a plain storage
  // buffer like 11/13 -- see transmission_buffer_.

  VkDescriptorSetLayoutCreateInfo render_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  render_layout_info.bindingCount = 29;
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
  // output_image_ and bloom_temp_image_, writes post_process_image_, and
  // (M7 polish TAA-ghosting fix) reads preview_overlay_image_ -- see
  // Builtin.PostComposite.comp.glsl.
  VkDescriptorSetLayoutBinding post_composite_bindings[4]{};
  for (u32 i = 0; i < 4; ++i) {
    post_composite_bindings[i].binding = i;
    post_composite_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    post_composite_bindings[i].descriptorCount = 1;
    post_composite_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }

  VkDescriptorSetLayoutCreateInfo post_composite_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  post_composite_layout_info.bindingCount = 4;
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
  // 2=ao_image (write), 3=voxel_cascades, 4=material_scalars} -- must match
  // Builtin.StochasticAo.comp.glsl. The three images are render targets, so
  // this set is re-pointed by rebind_render_target_descriptors() like every
  // other set that references one. Binding 4 is there only for
  // Material::casts_shadow: the screen-space stage reads it to march past a
  // non-casting occluder instead of darkening ambient light with it.
  VkDescriptorSetLayoutBinding stochastic_ao_bindings[5]{};
  for (u32 i = 0; i < 5; ++i) {
    stochastic_ao_bindings[i].binding = i;
    stochastic_ao_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    stochastic_ao_bindings[i].descriptorCount = 1;
    stochastic_ao_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  stochastic_ao_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  stochastic_ao_bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  VkDescriptorSetLayoutCreateInfo stochastic_ao_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  stochastic_ao_layout_info.bindingCount = 5;
  stochastic_ao_layout_info.pBindings = stochastic_ao_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &stochastic_ao_layout_info,
                                       context_->allocator,
                                       &stochastic_ao_set_layout_));

  // One pool backing all eight sets. (A ninth, voxelize_set_, was removed
  // along with Builtin.RaymarchVoxelize.comp.glsl -- M0 CSG deletion pass,
  // see /home/h-i/.claude/plans/joyful-skipping-giraffe.md.)
  VkDescriptorPoolSize pool_sizes[3]{};
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  // render's 20 (11 + Phase 4's 4 chunked-field bindings + Phase 5's 1 GI
  // cascade probe binding + splatting's 4: visibility buffer, point pool,
  // point meta, per-tile near bound) + probe bake's 7 x2 (probe_bake_set_
  // and probe_bake_set_odd_ -- see their declaration comment).
  // ...plus the AO pass's two storage buffers (the voxel cascades, and the
  // material scalars its screen-space stage tests casts_shadow with).
  pool_sizes[0].descriptorCount = 44 - 7;
  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  // render's 2 (output + depth) + bloom blur h's 2 + post composite's 3 +
  // TAA resolve's 5 x2 sets.
  // render's 3 (colour + the G-buffer's two halves) + bloom blur h's 2 +
  // post composite's 3 + TAA resolve's 5 x2 sets.
  // render's 4 (colour, the G-buffer's two halves, the AO estimate) + bloom
  // blur h's 2 + post composite's 4 (M7 polish TAA-ghosting fix added its
  // preview_overlay_image_ read) + TAA resolve's 5 x2 sets + the AO pass's
  // own 3 images.
  pool_sizes[1].descriptorCount = 23;
  pool_sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  // scene_textures + scene_bump_textures + skybox_texture.
  pool_sizes[2].descriptorCount = kMaxScenePrimitives * 2 + 1;

  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = pool_sizes;
  pool_info.maxSets = 8;
  VK_CHECK(vkCreateDescriptorPool(context_->device.logical_device, &pool_info,
                                  context_->allocator, &descriptor_pool_));

  VkDescriptorSetLayout layouts[8] = {
      render_set_layout_,
      probe_bake_set_layout_,     bloom_blur_h_set_layout_,
      post_composite_set_layout_, probe_bake_set_layout_,
      taa_resolve_set_layout_,    taa_resolve_set_layout_,
      stochastic_ao_set_layout_};
  VkDescriptorSet sets[8];
  VkDescriptorSetAllocateInfo alloc_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc_info.descriptorPool = descriptor_pool_;
  alloc_info.descriptorSetCount = 8;
  alloc_info.pSetLayouts = layouts;
  VK_CHECK(vkAllocateDescriptorSets(context_->device.logical_device,
                                    &alloc_info, sets));
  render_set_ = sets[0];
  probe_bake_set_ = sets[1];
  bloom_blur_h_set_ = sets[2];
  post_composite_set_ = sets[3];
  probe_bake_set_odd_ = sets[4];
  taa_resolve_sets_[0] = sets[5];
  taa_resolve_sets_[1] = sets[6];
  stochastic_ao_set_ = sets[7];

  // voxelize_set_'s own population (indirection/brick_pool/brick_counter/
  // primitive_buffer/brick_primitive_buffer/layer_buffer/param_expr_buffer)
  // was removed along with that set.

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
  // kProbeBounceCount is compiled in -- 11 = the per-primitive material
  // scalars (pixelation-exempt/bump strength/shadow casting -- see
  // material_scalar_buffer_), and 13 = per-primitive texture offset/
  // rotation.
  struct RenderBufferBinding {
    u32 binding;
    VkBuffer buffer;
  } render_buffer_bindings[22] = {
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
      {11, material_scalar_buffer_->handle()},
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
      // 28 = per-primitive absorption + IOR, read only by the transmissive
      // shading block in Builtin.DeferredShade.comp.glsl.
      {28, transmission_buffer_->handle()},
  };

  VkDescriptorBufferInfo render_buffer_infos[22];
  VkWriteDescriptorSet render_writes[23]{};
  render_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  render_writes[0].dstSet = render_set_;
  render_writes[0].dstBinding = 0;
  render_writes[0].descriptorCount = 1;
  render_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  render_writes[0].pImageInfo = &render_image_info;

  for (u32 i = 0; i < 22; ++i) {
    render_buffer_infos[i] = {render_buffer_bindings[i].buffer, 0,
                              VK_WHOLE_SIZE};
    render_writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    render_writes[i + 1].dstSet = render_set_;
    render_writes[i + 1].dstBinding = render_buffer_bindings[i].binding;
    render_writes[i + 1].descriptorCount = 1;
    render_writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    render_writes[i + 1].pBufferInfo = &render_buffer_infos[i];
  }

  vkUpdateDescriptorSets(context_->device.logical_device, 23, render_writes,
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
  // bloom_temp_image_ (read), 2 = post_process_image_ (write), 3 =
  // preview_overlay_image_ (read -- M7 polish TAA-ghosting fix).
  VkDescriptorImageInfo post_composite_image_infos[4] = {
      {VK_NULL_HANDLE, taa_output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, bloom_temp_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, post_process_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, preview_overlay_image_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet post_composite_writes[4]{};
  for (u32 i = 0; i < 4; ++i) {
    post_composite_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    post_composite_writes[i].dstSet = post_composite_set_;
    post_composite_writes[i].dstBinding = i;
    post_composite_writes[i].descriptorCount = 1;
    post_composite_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    post_composite_writes[i].pImageInfo = &post_composite_image_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 4,
                        post_composite_writes, 0, nullptr);

  // Everything that points at a render-target image (render_set_'s 0/24,
  // both TAA parities, bloom/post's inputs) is written by one function, so
  // the initial population and the post-resize repoint can never drift
  // apart -- the sets above only cover bindings that outlive a resize.
  rebind_render_target_descriptors();

  // voxelize_pipeline_/chunk_voxelize_pipeline_ (Builtin.RaymarchVoxelize.
  // comp.glsl/Builtin.ChunkVoxelize.comp.glsl) were removed here -- M0 CSG
  // deletion pass, see /home/h-i/.claude/plans/joyful-skipping-giraffe.md.
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
  chunk_sculpt_stamp_pipeline_.emplace(
      *context_, chunk_sculpt_stamp_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_sculpt_stamp_set_layout_},
      sizeof(SculptStampPushConstants));
  chunk_sculpt_splat_regen_pipeline_.emplace(
      *context_, chunk_sculpt_splat_regen_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_sculpt_splat_regen_set_layout_},
      sizeof(SculptSplatRegenPushConstants));
  chunk_sculpt_undo_restore_pipeline_.emplace(
      *context_, chunk_sculpt_undo_restore_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_sculpt_undo_restore_set_layout_},
      sizeof(SculptUndoRestorePushConstants));
  // M3c: see spray_paint().
  chunk_sculpt_spraypaint_pipeline_.emplace(
      *context_, chunk_sculpt_spraypaint_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_sculpt_spraypaint_set_layout_},
      sizeof(SculptSpraypaintPushConstants));
  // M4b: reuses chunk_sculpt_stamp_set_layout_ wholesale (same binding list
  // -- see chunk_sculpt_checkpoint_restore_pipeline_'s own header comment)
  // and SculptUndoRestorePushConstants (identical single-int push-constant
  // shape to what Builtin.SculptCheckpointRestore.comp.glsl declares).
  chunk_sculpt_checkpoint_restore_pipeline_.emplace(
      *context_, chunk_sculpt_checkpoint_restore_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_sculpt_stamp_set_layout_},
      sizeof(SculptUndoRestorePushConstants));
  // M4 (Crop/Cutout): reuses chunk_sculpt_undo_restore_set_layout_ wholesale
  // -- see chunk_sculpt_crop_pipeline_'s own header comment (vulkan_
  // raymarch_shader.h) for why that set's binding list already covers
  // everything Builtin.SculptCrop.comp.glsl needs.
  chunk_sculpt_crop_pipeline_.emplace(
      *context_, chunk_sculpt_crop_stage_,
      std::vector<VkDescriptorSetLayout>{chunk_sculpt_undo_restore_set_layout_},
      sizeof(SculptCropPushConstants));
  render_pipeline_.emplace(*context_, render_stage_,
                           std::vector<VkDescriptorSetLayout>{render_set_layout_},
                           sizeof(PushConstants));
  // Same main descriptor set layout and same push constants as the
  // visibility pass -- see deferred_shade_stage_'s comment for why they
  // share. M3c adds a SECOND set (paint_read_set_layout_, set = 1) to THIS
  // pipeline only -- render_pipeline_ above stays single-set, completely
  // unaffected, since the visibility pass never needs paint data. See
  // ChunkBrickPaintBuffer's own comment in Builtin.DeferredShade.comp.glsl
  // for why a second set, not a 30th binding on render_set_layout_ itself.
  deferred_shade_pipeline_.emplace(
      *context_, deferred_shade_stage_,
      std::vector<VkDescriptorSetLayout>{render_set_layout_,
                                         paint_read_set_layout_},
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

  if (!chunk_debug_query_pipeline_->is_valid() || !chunk_evict_pipeline_->is_valid() ||
      !probe_bake_pipeline_->is_valid() || !chunk_probe_bake_pipeline_->is_valid() ||
      !chunk_cluster_cull_pipeline_->is_valid() ||
      !chunk_shadow_splat_pipeline_->is_valid() ||
      !chunk_voxel_cascade_pipeline_->is_valid() ||
      !stochastic_ao_pipeline_->is_valid() ||
      !chunk_point_splat_pipeline_->is_valid() ||
      !render_pipeline_->is_valid() || !deferred_shade_pipeline_->is_valid() ||
      !bloom_blur_h_pipeline_->is_valid() ||
      !post_composite_pipeline_->is_valid() ||
      !taa_resolve_pipeline_->is_valid() ||
      !chunk_sculpt_stamp_pipeline_->is_valid() ||
      !chunk_sculpt_splat_regen_pipeline_->is_valid() ||
      !chunk_sculpt_undo_restore_pipeline_->is_valid() ||
      !chunk_sculpt_spraypaint_pipeline_->is_valid()) {
    KERROR("Failed to create compute pipeline(s) for the raymarch field.");
    return;
  }

  // The per-level ChunkStreamingManager construction loop that used to run
  // here was removed along with that class (M0 CSG deletion pass, see
  // /home/h-i/.claude/plans/joyful-skipping-giraffe.md).
  // One generation stamp per global slot -- see the alias cache.
  slot_content_generation_.assign(
      static_cast<size_t>(kNumLevels) * kMaxResidentChunks, 0);

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

  // The rebuild_static_scene() call that used to upload the registered
  // CSG scene and bake it was removed here (M0 CSG deletion pass) -- there
  // is no scene left to upload.

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
  // The disk-backed brick cache's env-var setup (KENGINE_CHUNK_CACHE_DIR
  // -> chunk_cache_dir_/chunk_cache_enabled_) was removed along with that
  // cache (M0 CSG deletion pass) -- it only ever cached primitive-driven
  // bakes.
  // Diagnostic: isolate one clip level in the splat pass. Run the editor
  // with KENGINE_SPLAT_ONLY_LEVEL=0 (then 1, 2, ...) to see exactly which
  // level a suspect artefact comes from -- the fastest way to tell a real
  // bug apart from the hard LOD cutoff at a level boundary.
  if (const char *only = std::getenv("KENGINE_SPLAT_ONLY_LEVEL")) {
    splat_only_level_ = std::atoi(only);
    KINFO("Splatting ONLY clip level {} (KENGINE_SPLAT_ONLY_LEVEL) -- "
         "diagnostic mode, every other level is suppressed.",
         splat_only_level_);
  }
  // Bake the whole scene into the disk cache at load, instead of warming it
  // by touring the scene. Only meaningful with KENGINE_CHUNK_CACHE_DIR set.
  // ON BY DEFAULT, opt out with KENGINE_NO_PREWARM_CACHE -- the same
  // inverted-flag shape brick deduplication ended up with (see
  // KENGINE_NO_CHUNK_DEDUP), and for the same reason: it shipped behind an
  // opt-in flag until it had been watched running against real content, and
  // has now done so. It only does anything at all when a cache directory is
  // configured, so a run without KENGINE_CHUNK_CACHE_DIR is unaffected --
  // silently, rather than warning about a default nobody asked for.
  // No disk cache exists to pre-warm any more (see above), so
  // prewarm_requested_ just stays false.
  prewarm_requested_ = false;
  chunk_dedup_enabled_ = std::getenv("KENGINE_NO_CHUNK_DEDUP") == nullptr;
  if (!chunk_dedup_enabled_) {
    KINFO("Chunk brick deduplication DISABLED (KENGINE_NO_CHUNK_DEDUP).");
  }

  reset_chunked_field();

  // M1a: so the very first stamp_shape() call has a valid material to
  // reference immediately, with no per-call readiness check needed.
  ensure_default_sculpt_material();
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
  chunk_sculpt_spraypaint_pipeline_.reset();
  chunk_sculpt_crop_pipeline_.reset();
  chunk_sculpt_checkpoint_restore_pipeline_.reset();
  chunk_sculpt_undo_restore_pipeline_.reset();
  chunk_sculpt_splat_regen_pipeline_.reset();
  chunk_sculpt_stamp_pipeline_.reset();
  post_composite_pipeline_.reset();
  bloom_blur_h_pipeline_.reset();
  render_pipeline_.reset();
  probe_bake_pipeline_.reset();
  chunk_point_splat_pipeline_.reset();
  chunk_probe_bake_pipeline_.reset();
  chunk_evict_pipeline_.reset();
  chunk_debug_query_pipeline_.reset();

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
  if (sculpt_descriptor_pool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(context_->device.logical_device,
                            sculpt_descriptor_pool_, context_->allocator);
    sculpt_descriptor_pool_ = VK_NULL_HANDLE;
  }
  if (paint_descriptor_pool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(context_->device.logical_device,
                            paint_descriptor_pool_, context_->allocator);
    paint_descriptor_pool_ = VK_NULL_HANDLE;
  }
  if (paint_read_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 paint_read_set_layout_, context_->allocator);
    paint_read_set_layout_ = VK_NULL_HANDLE;
  }
  if (chunk_sculpt_spraypaint_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 chunk_sculpt_spraypaint_set_layout_,
                                 context_->allocator);
    chunk_sculpt_spraypaint_set_layout_ = VK_NULL_HANDLE;
  }
  if (chunk_sculpt_undo_restore_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 chunk_sculpt_undo_restore_set_layout_,
                                 context_->allocator);
    chunk_sculpt_undo_restore_set_layout_ = VK_NULL_HANDLE;
  }
  if (chunk_sculpt_splat_regen_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 chunk_sculpt_splat_regen_set_layout_,
                                 context_->allocator);
    chunk_sculpt_splat_regen_set_layout_ = VK_NULL_HANDLE;
  }
  if (chunk_sculpt_stamp_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 chunk_sculpt_stamp_set_layout_,
                                 context_->allocator);
    chunk_sculpt_stamp_set_layout_ = VK_NULL_HANDLE;
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
  layer_buffer_.reset();
  light_buffer_.reset();
  param_expr_buffer_.reset();
  material_scalar_buffer_.reset();
  transmission_buffer_.reset();
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
  chunk_cache_staging_buffer_.reset();
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
  chunk_sculpt_cell_buffer_.reset();
  chunk_voxelize_batch_data_buffer_.reset();
  chunk_voxelize_batch_slot_buffer_.reset();
  chunk_slot_published_buffer_.reset();
  chunk_brick_free_list_top_buffer_.reset();
  chunk_brick_free_list_buffer_.reset();
  chunk_brick_demand_buffer_.reset();
  chunk_brick_paint_buffer_.reset();
  brush_preview_buffer_.reset();
  chunk_brick_primitive_buffer_.reset();
  chunk_brick_looseness_buffer_.reset();
  chunk_brick_pool_buffer_.reset();
  chunk_indirection_buffer_.reset();
  chunk_table_buffer_.reset();

  vulkan_image_destroy(context_, &taa_output_image_);
  vulkan_image_destroy(context_, &history_images_[1]);
  vulkan_image_destroy(context_, &history_images_[0]);
  vulkan_image_destroy(context_, &ao_image_);
  vulkan_image_destroy(context_, &preview_overlay_image_);
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
  // M7 polish (TAA-ghosting fix): the brush preview's shaded colour+alpha,
  // written by Builtin.DeferredShade.comp.glsl (paint_read_set_ binding 2)
  // and read back by Builtin.PostComposite.comp.glsl (post_composite_set_
  // binding 3) -- see preview_overlay_image_'s own header comment. Same
  // format as output_image_/taa_output_image_/post_process_image_ (the
  // swapchain's own rgba8-class format, matching every other image these
  // two shaders already blend together) and same render resolution/
  // lifetime as the rest of this function's images.
  vulkan_image_destroy(context_, &preview_overlay_image_);
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, render_width_, render_height_,
                      context_->swapchain.image_format.format,
                      VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &preview_overlay_image_);
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

  // The AO pass's own set: two G-buffer reads, the AO write, the voxel
  // cascades it traces against, and the material scalars its screen-space
  // stage tests Material::casts_shadow with (both buffers, not render
  // targets, but written here so the whole set is populated in one place).
  VkDescriptorImageInfo ao_image_infos[3] = {
      {VK_NULL_HANDLE, depth_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, normal_material_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, ao_image_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkDescriptorBufferInfo ao_buffer_infos[2] = {
      {voxel_cascade_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {material_scalar_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };
  VkWriteDescriptorSet ao_writes[5]{};
  for (u32 i = 0; i < 3; ++i) {
    ao_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ao_writes[i].dstSet = stochastic_ao_set_;
    ao_writes[i].dstBinding = i;
    ao_writes[i].descriptorCount = 1;
    ao_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ao_writes[i].pImageInfo = &ao_image_infos[i];
  }
  for (u32 i = 3; i < 5; ++i) {
    ao_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ao_writes[i].dstSet = stochastic_ao_set_;
    ao_writes[i].dstBinding = i;
    ao_writes[i].descriptorCount = 1;
    ao_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ao_writes[i].pBufferInfo = &ao_buffer_infos[i - 3];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 5, ao_writes, 0,
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

  VkDescriptorImageInfo post_composite_image_infos[4] = {
      {VK_NULL_HANDLE, taa_output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, bloom_temp_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, post_process_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, preview_overlay_image_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet post_composite_writes[4]{};
  for (u32 i = 0; i < 4; ++i) {
    post_composite_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    post_composite_writes[i].dstSet = post_composite_set_;
    post_composite_writes[i].dstBinding = i;
    post_composite_writes[i].descriptorCount = 1;
    post_composite_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    post_composite_writes[i].pImageInfo = &post_composite_image_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 4,
                        post_composite_writes, 0, nullptr);

  // M7 polish (TAA-ghosting fix): paint_read_set_'s binding 2 --
  // preview_overlay_image_ -- also needs re-pointing here: it is recreated
  // alongside depth_image_/ao_image_ etc above, same as every other
  // render-target image this function repoints.
  VkDescriptorImageInfo paint_read_preview_image_info{
      VK_NULL_HANDLE, preview_overlay_image_.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet paint_read_preview_write{
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  paint_read_preview_write.dstSet = paint_read_set_;
  paint_read_preview_write.dstBinding = 2;
  paint_read_preview_write.descriptorCount = 1;
  paint_read_preview_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  paint_read_preview_write.pImageInfo = &paint_read_preview_image_info;
  vkUpdateDescriptorSets(context_->device.logical_device, 1,
                        &paint_read_preview_write, 0, nullptr);
}

// Used to deep-copy GeometrySystem's registered CSG primitives into the
// scene buffers and rebuild the baked field from them (rebuild_static_
// scene()) -- removed in the M0 CSG deletion pass (see /home/h-i/.claude/
// plans/joyful-skipping-giraffe.md) along with that whole primitive-driven
// pipeline. Kept as a no-op, rather than deleted outright, because
// VulkanRendererBackend::maybe_recenter_origin() (floating origin, a
// CSG-independent feature) still defers to this via scene_dirty_/
// begin_frame() after shifting every render-space position -- see that
// function's own comment. There is currently nothing left for a rebake to
// actually do (no primitive list to re-voxelize), so this intentionally
// does nothing; a future milestone's SculptVolume is expected to give this
// a real body again.
void VulkanRaymarchShader::rebake() {
  if (!valid_) {
    KWARN("VulkanRaymarchShader::rebake called on an invalid shader.");
    return;
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
  // Already showing exactly this image -- nothing to do, and it matters
  // that this is free rather than merely idempotent. The wait below drains
  // the whole device, and this is now called from load_scene()/
  // reconcile_scene() with whatever SdfScene::skybox names: the editor
  // reconciles on EVERY edit, so without this, nudging any spinbox would
  // stall the device to re-acquire a texture it already has bound.
  if (skybox_enabled_ && skybox_texture_name_ == texture_name) {
    return;
  }

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

// Resets the chunked field's own table/free-lists/pools to the empty state
// (called once from the constructor, and could be called again by a future
// "clear the sculpture" feature) -- generic chunk-table bookkeeping with no
// dependency on the CSG primitive list, so this survived the M0 CSG
// deletion pass intact (see /home/h-i/.claude/plans/
// joyful-skipping-giraffe.md).
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

  // M3c (Spraypaint): every voxel starts unpainted. A plain vkCmdFillBuffer
  // of a single, self-contained command buffer -- not upload_to_device_
  // local(), which would need a ~1.9GB CPU-side zero array for a buffer
  // this size -- the fill value 0u's bit pattern is exactly the f32 0.0f
  // this buffer stores, so a raw dword fill is correct here, same as every
  // other vkCmdFillBuffer-based zero-clear in this file.
  {
    auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
        *context_, context_->device.graphics_command_pool);
    vkCmdFillBuffer(cmd->handle(), chunk_brick_paint_buffer_->handle(), 0,
                    VK_WHOLE_SIZE, 0u);
    VulkanCommandBuffer::end_single_use(*context_,
                                        context_->device.graphics_command_pool,
                                        std::move(cmd),
                                        context_->device.graphics_queue);
  }

  // M1a: nothing has stamped into any level-0 chunk yet -- see
  // acquire_sculpt_chunk_slot().
  sculpt_chunk_slot_used_.assign(kMaxResidentChunks, false);
  sculpt_chunk_slot_by_coord_.clear();

  // M2b: every captured undo entry refers to chunk slots/brick indices that
  // just got wiped above -- none of them mean anything post-reset.
  undo_stack_.clear();
}

void VulkanRaymarchShader::ensure_default_sculpt_material() {
  if (default_sculpt_material_ready_) {
    return;
  }
  default_sculpt_material_ready_ = true;

  // Index 0 of every per-primitive material array: a plain white,
  // default-roughness (0.5), opaque, shadow-casting, non-metallic,
  // non-pixelation-exempt material -- the one material every M1a sphere
  // stamp uses (see ScenePrimitiveColours/ScenePrimitiveMaterialScalars/
  // ScenePrimitiveTransmission's own packing comments in Builtin.
  // DeferredShade.comp.glsl). All four buffers are host-visible
  // (material_scalar_buffer_'s own creation comment), so a direct
  // load_data() is enough -- no staging copy needed.
  const glm::vec4 diffuse(1.0f, 1.0f, 1.0f, 1.0f); // white tint, texture_scale=1
  primitive_colour_buffer_->load_data(0, sizeof(glm::vec4), 0, &diffuse);

  const glm::vec4 scalars(0.0f, 1.0f, 1.0f, 0.5f); // not exempt/metallic, bump=1, casts shadow, roughness=0.5
  material_scalar_buffer_->load_data(0, sizeof(glm::vec4), 0, &scalars);

  const glm::vec4 transmission(0.0f, 0.0f, 0.0f, 0.0f); // opaque (ior 0)
  transmission_buffer_->load_data(0, sizeof(glm::vec4), 0, &transmission);

  const glm::vec4 tex_transform(0.0f, 0.0f, 0.0f, 0.0f); // no uv offset/rotation
  tex_transform_buffer_->load_data(0, sizeof(glm::vec4), 0, &tex_transform);

  // M3c (Spraypaint): index 1 of the SAME per-primitive colour array --
  // see Builtin.DeferredShade.comp.glsl's PAINT_MATERIAL_SLOT, which reads
  // scene_diffuse_colours[1].rgb directly as "the paint colour" when
  // blending in accumulated spray opacity. A fixed, clearly-visible bright
  // red/magenta -- simplest-reasonable per the plan's own M3c scope (no
  // colour-picker UI exists yet), and visually unmistakable in a
  // screenshot against most scene backgrounds/geometry colours. This slot
  // is never used as an actual hit_material (nothing ever stamps geometry
  // with material_slot=1 -- place_stamp() in tools/modelling_software_v2
  // always passes 0), so material_scalars/transmission/tex_transform for
  // it are left at the same harmless defaults index 0 already got (never
  // read for slot 1 specifically, but initialized anyway so nothing ever
  // reads uninitialized memory if that ever changes).
  const glm::vec4 paint_diffuse(1.0f, 0.05f, 0.05f, 1.0f); // bright red, texture_scale=1
  primitive_colour_buffer_->load_data(sizeof(glm::vec4), sizeof(glm::vec4), 0,
                                      &paint_diffuse);
  material_scalar_buffer_->load_data(sizeof(glm::vec4), sizeof(glm::vec4), 0,
                                     &scalars);
  transmission_buffer_->load_data(sizeof(glm::vec4), sizeof(glm::vec4), 0,
                                  &transmission);
  tex_transform_buffer_->load_data(sizeof(glm::vec4), sizeof(glm::vec4), 0,
                                   &tex_transform);

  // render_set_'s binding 6 (scene_textures[kMaxScenePrimitives]) / binding
  // 14 (scene_bump_textures[kMaxScenePrimitives]) -- see their own comment
  // in Builtin.DeferredShade.comp.glsl: "Vulkan requires every element of a
  // bound sampler array to reference a real image regardless of whether the
  // shader ever reads that index." rebuild_static_scene() used to populate
  // these for every registered CSG primitive; it was deleted in the M0
  // pass and NOTHING has written either array since -- confirmed by
  // grepping this file for texture_system->default_texture() call sites,
  // of which write_skybox_binding()'s is now the only one. Without this,
  // sampling scene_textures[0] for the stamped sphere's hit_material (0)
  // would read an entirely unbound descriptor the very first frame
  // anything renders through the chunked field. Every slot gets the same
  // filler (not just index 0): leaving slots [1, kMaxScenePrimitives)
  // unwritten would still violate the same Vulkan requirement the instant
  // this array is bound to a pipeline that statically indexes it by a
  // non-constant expression, which Builtin.DeferredShade.comp.glsl's
  // scene_textures[hit_material] always does.
  VkDescriptorImageInfo diffuse_info{};
  diffuse_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  diffuse_info.imageView = context_->texture_system->default_texture().view();
  diffuse_info.sampler = context_->texture_system->default_texture().sampler();
  VkDescriptorImageInfo bump_info{};
  bump_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  bump_info.imageView = context_->texture_system->flat_texture().view();
  bump_info.sampler = context_->texture_system->flat_texture().sampler();

  std::vector<VkDescriptorImageInfo> diffuse_infos(kMaxScenePrimitives, diffuse_info);
  std::vector<VkDescriptorImageInfo> bump_infos(kMaxScenePrimitives, bump_info);

  VkWriteDescriptorSet writes[2]{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = render_set_;
  writes[0].dstBinding = 6;
  writes[0].descriptorCount = kMaxScenePrimitives;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[0].pImageInfo = diffuse_infos.data();
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = render_set_;
  writes[1].dstBinding = 14;
  writes[1].descriptorCount = kMaxScenePrimitives;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[1].pImageInfo = bump_infos.data();
  vkUpdateDescriptorSets(context_->device.logical_device, 2, writes, 0, nullptr);
}

void VulkanRaymarchShader::set_material(i32 slot, glm::vec3 colour,
                                        f32 roughness, f32 metallic,
                                        bool waxy, i32 fleck_pattern,
                                        f32 fleck_strength, f32 fleck_scale) {
  // Slots 0/1 are reserved (ensure_default_sculpt_material()'s plain-white
  // default and Spraypaint's fixed paint colour) -- overwriting either
  // would silently change the look of every stamp/spray already relying
  // on them. slot must also fit the fixed-size arrays every one of these
  // buffers was allocated at (kMaxScenePrimitives) -- there is no resize
  // path.
  if (slot < 2 || slot >= static_cast<i32>(kMaxScenePrimitives)) {
    KWARN("VulkanRaymarchShader::set_material: slot {} out of range "
         "[2, {}) -- ignoring.",
         slot, kMaxScenePrimitives);
    return;
  }
  if (!primitive_colour_buffer_ || !material_scalar_buffer_ ||
      !transmission_buffer_) {
    KWARN("VulkanRaymarchShader::set_material: called before the sculpt "
         "material buffers exist -- ignoring.");
    return;
  }

  const u64 offset = static_cast<u64>(slot) * sizeof(glm::vec4);
  const f32 clamped_roughness = glm::clamp(roughness, 0.0f, 1.0f);
  const f32 clamped_metallic = glm::clamp(metallic, 0.0f, 1.0f);

  // Same packing ensure_default_sculpt_material() uses for slots 0/1 --
  // see this method's own header comment (vulkan_raymarch_shader.h) for
  // the full table. texture_scale=1 in the colour buffer's alpha, same as
  // every sculpt material so far (no per-material texture tiling control
  // exists in this tool yet).
  const glm::vec4 diffuse(colour, 1.0f);
  primitive_colour_buffer_->load_data(offset, sizeof(glm::vec4), 0, &diffuse);

  // x: exempt flag (always 0 for an authored Finish) + metallic*0.5 -- see
  // ScenePrimitiveMaterialScalars' own packing comment in Builtin.
  // DeferredShade.comp.glsl. y: ordinary bump strength (no bump map is
  // ever bound, so inert either way -- kept at 1.0 for consistency with
  // slots 0/1). z: casts_shadow. w: roughness.
  const glm::vec4 scalars(clamped_metallic * 0.5f, 1.0f, 1.0f,
                          clamped_roughness);
  material_scalar_buffer_->load_data(offset, sizeof(glm::vec4), 0, &scalars);

  // x: the "waxy" second-specular-lobe flag (M5). y/z/w: M6 (Flecks)'s
  // pattern id (0-18, rounded)/strength/scale -- see this method's own
  // header comment (vulkan_raymarch_shader.h) for the full packing table
  // and why transmission_buffer_, not tex_transform_buffer_, is where
  // these live. A caller that wants no Fleck at all is expected to pass
  // (0, 0.0f, 1.0f) -- strength 0 is what Builtin.FleckPattern.inc.glsl's
  // entry points gate on, so fleck_pattern/fleck_scale's exact values
  // don't matter in that case, but they are still stored verbatim (not
  // forced to 0) so a future "re-enable the last Fleck" UI action has
  // something to read back.
  const f32 clamped_fleck_strength = glm::clamp(fleck_strength, 0.0f, 1.0f);
  const glm::vec4 transmission(waxy ? 1.0f : 0.0f,
                               static_cast<f32>(fleck_pattern),
                               clamped_fleck_strength, fleck_scale);
  transmission_buffer_->load_data(offset, sizeof(glm::vec4), 0, &transmission);

  // No uv offset/rotation control exists for sculpt materials yet -- same
  // all-zero default ensure_default_sculpt_material() uses for slots 0/1.
  // M6 investigated repurposing this buffer's apparently-free components
  // for Fleck params instead of transmission_buffer_'s and found it NOT
  // safe -- see set_material()'s own header comment (vulkan_raymarch_
  // shader.h) for why: at the time M6 landed, every sculpt material's
  // scene_textures[slot] was bound to a visible procedural checkerboard
  // (texture_system->default_texture()), and this buffer's xyz/w feed that
  // checkerboard's triplanar UV directly in Builtin.DeferredShade.comp.glsl
  // -- a non-zero write here would have visibly shifted it regardless of
  // Fleck settings. A later M5 visual-quality pass rebound every authored
  // Finish's scene_textures[slot] to flat_texture() instead (see below --
  // tex_colour is now spatially uniform for every slot this function
  // touches), but the reasoning for leaving this buffer untouched still
  // holds for the same reason it always would for a flat texture: a UV
  // offset/rotation has no visible effect on a texture with no spatial
  // variation to offset or rotate, so there is nothing to gain by writing
  // anything other than the all-zero default here.
  const glm::vec4 tex_transform(0.0f, 0.0f, 0.0f, 0.0f);
  tex_transform_buffer_->load_data(offset, sizeof(glm::vec4), 0, &tex_transform);

  // M5 (Finishes) visual-quality fix: scene_textures[slot] started out
  // bound to texture_system->default_texture() -- a loud, high-contrast
  // procedural black/white checkerboard (ensure_default_sculpt_material()'s
  // blanket fill across the FULL [0, kMaxScenePrimitives) range, meant as a
  // "missing texture" placeholder, the same convention most engines use).
  // That is exactly right for slot 0 (the bare M1a stamp this tool/testbed
  // fall back to when no Finish has ever been applied -- it's deliberately
  // visible so a forgotten texture assignment is obvious), but every
  // authored Finish reaching this function has no texture of its own to
  // assign either, and was inheriting the SAME checkerboard by the same
  // fallback. The checkerboard's own contrast (near-black to near-white
  // cells) swamped the specular differences the whole Finishes feature
  // exists to show -- a "Shiny" and a "Rough" sphere both read as "the
  // black-and-white checker sphere, but with a highlight somewhere," and a
  // "Metal" sphere's specular (tinted by albedo = tex_colour * tint, see
  // Builtin.DeferredShade.comp.glsl's final combine) went dark over every
  // black checker cell instead of reading as a smooth coloured metal. Once
  // Finish presets select by colour, there is no sense in which a checker
  // pattern was ever "the material's actual look" for them -- it was purely
  // this binding never having been pointed anywhere else. Rebinding this
  // one slot to flat_texture() (plain, spatially-uniform white -- the same
  // texture scene_bump_textures[slot] already defaults to for "no bump
  // map", see its own comment above) makes tex_colour == (1,1,1) everywhere
  // for an authored Finish, so albedo reduces to exactly `tint` (the
  // Material Colour the user picked) and the GGX specular/Fresnel/metal-tint
  // math already in the shader is what actually determines the look,
  // undisguised. Slot 0/1 are never passed through this function (guarded
  // above), so the bare-stamp/Spraypaint checkerboard baseline is completely
  // unaffected -- this only ever rebinds a slot a Finish was just assigned.
  VkDescriptorImageInfo diffuse_info{};
  diffuse_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  diffuse_info.imageView = context_->texture_system->flat_texture().view();
  diffuse_info.sampler = context_->texture_system->flat_texture().sampler();

  VkWriteDescriptorSet diffuse_write{};
  diffuse_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  diffuse_write.dstSet = render_set_;
  diffuse_write.dstBinding = 6;
  diffuse_write.dstArrayElement = static_cast<u32>(slot);
  diffuse_write.descriptorCount = 1;
  diffuse_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  diffuse_write.pImageInfo = &diffuse_info;
  vkUpdateDescriptorSets(context_->device.logical_device, 1, &diffuse_write, 0,
                        nullptr);

  // scene_bump_textures[slot] was already bound to the same flat filler
  // every other slot got in ensure_default_sculpt_material() -- no bump map
  // control exists for sculpt materials yet, so nothing further needs
  // binding there for a brand-new slot to render correctly.
}

// M7 polish -- see this method's own header comment (vulkan_raymarch_
// shader.h) for what this is and why it exists. A plain direct load_data()
// write into brush_preview_buffer_, exactly set_material()'s own idiom
// just above -- no staging buffer, no device-idle wait, safe to call every
// frame. `enabled=false` still writes the full struct (not a 4-byte early
// write of just the flag): the shader reads center_enabled.w before
// touching anything else in the buffer, so a stale center/shape/slot left
// over from the last ENABLED call can never leak through while disabled,
// and a single load_data() call is simpler than reasoning about which
// partial writes are safe to skip.
void VulkanRaymarchShader::set_brush_preview(bool enabled, glm::vec3 center,
                                             BrushShape shape_type,
                                             glm::vec4 params, BrushOp op,
                                             i32 material_slot) {
  if (!brush_preview_buffer_) {
    KWARN("VulkanRaymarchShader::set_brush_preview: called before "
         "brush_preview_buffer_ exists -- ignoring.");
    return;
  }
  GpuBrushPreview preview{};
  preview.center_enabled[0] = center.x;
  preview.center_enabled[1] = center.y;
  preview.center_enabled[2] = center.z;
  preview.center_enabled[3] = enabled ? 1.0f : 0.0f;
  preview.params[0] = params.x;
  preview.params[1] = params.y;
  preview.params[2] = params.z;
  preview.params[3] = params.w;
  preview.shape_op_slot[0] = static_cast<i32>(shape_type);
  preview.shape_op_slot[1] = static_cast<i32>(op);
  preview.shape_op_slot[2] = material_slot;
  preview.shape_op_slot[3] = 0;
  brush_preview_buffer_->load_data(0, sizeof(GpuBrushPreview), 0, &preview);
}

i32 VulkanRaymarchShader::add_light_internal(f32 light_type, glm::vec3 vector,
                                             glm::vec3 colour,
                                             f32 intensity) {
  if (!light_buffer_) {
    KWARN("VulkanRaymarchShader::add_light_internal: called before "
         "light_buffer_ exists -- ignoring.");
    return -1;
  }
  if (light_count_ < 0 ||
      static_cast<u32>(light_count_) >= kMaxLights) {
    KWARN("VulkanRaymarchShader::add_light_internal: kMaxLights ({}) "
         "already reached -- ignoring.",
         kMaxLights);
    return -1;
  }

  const i32 slot = light_count_;
  const GpuLight gpu_light{
      .vector_type = {vector.x, vector.y, vector.z, light_type},
      .colour_intensity = {colour.x, colour.y, colour.z, intensity},
      // -1: no associated primitive -- see GpuLight::source_primitive's
      // own comment (this struct's declaration above). Every light added
      // through this path is a standing light, not synthesized from an
      // emissive primitive (that synthesis logic was rebuild_static_
      // scene()'s, deleted in M0, and never applied to standalone lights
      // anyway).
      .source_primitive = {-1.0f, 0.0f, 0.0f, 0.0f},
  };
  light_buffer_->load_data(static_cast<u64>(slot) * sizeof(GpuLight),
                           sizeof(GpuLight), 0, &gpu_light);
  light_count_ = slot + 1;
  return slot;
}

i32 VulkanRaymarchShader::add_directional_light(glm::vec3 direction,
                                                 glm::vec3 colour,
                                                 f32 intensity) {
  // 0.0f matches GpuLight::vector_type.w's directional convention -- see
  // Builtin.DeferredShade.comp.glsl's light loop (light_type == 1 is Point,
  // anything else is treated as Directional). direction need not be
  // pre-normalized -- the shader normalizes it itself.
  return add_light_internal(0.0f, direction, colour, intensity);
}

i32 VulkanRaymarchShader::add_point_light(glm::vec3 position, glm::vec3 colour,
                                          f32 intensity) {
  return add_light_internal(1.0f, position, colour, intensity);
}

i32 VulkanRaymarchShader::acquire_sculpt_chunk_slot(glm::ivec3 chunk_coord,
                                                     bool &out_is_new) {
  ChunkKey key{/*level=*/0, chunk_coord.x, chunk_coord.y, chunk_coord.z};
  auto it = sculpt_chunk_slot_by_coord_.find(key);
  if (it != sculpt_chunk_slot_by_coord_.end()) {
    out_is_new = false;
    return static_cast<i32>(it->second);
  }

  u32 slot = kInvalidChunkSlot;
  for (u32 candidate = 0; candidate < kMaxResidentChunks; ++candidate) {
    if (!sculpt_chunk_slot_used_[candidate]) {
      slot = candidate;
      break;
    }
  }
  if (slot == kInvalidChunkSlot) {
    out_is_new = false;
    return -1; // Every level-0 slot taken -- M1a has no eviction path.
  }

  sculpt_chunk_slot_used_[slot] = true;
  sculpt_chunk_slot_by_coord_[key] = slot;

  // Level 0's own [0, kChunkTableDim^3) range of chunk_table_buffer_ (see
  // Builtin.ChunkedFieldCommon.inc.glsl's chunk_table_slot_at() -- this
  // must wrap identically, or the render pass would resolve a different
  // slot than the one this stamp just published into).
  auto wrap16 = [](i64 v) -> i64 {
    return ((v % static_cast<i64>(kChunkTableDim)) + kChunkTableDim) %
        static_cast<i64>(kChunkTableDim);
  };
  const i64 wx = wrap16(key.cx);
  const i64 wy = wrap16(key.cy);
  const i64 wz = wrap16(key.cz);
  const u64 table_index = static_cast<u64>(
      wx + wy * kChunkTableDim + wz * kChunkTableDim * kChunkTableDim);

  if (void *mapped =
          chunk_table_buffer_->lock(table_index * sizeof(i32), sizeof(i32), 0)) {
    *static_cast<i32 *>(mapped) = static_cast<i32>(slot);
    chunk_table_buffer_->unlock();
  }
  if (void *mapped = chunk_slot_published_buffer_->lock(
          static_cast<u64>(slot) * sizeof(u32), sizeof(u32), 0)) {
    *static_cast<u32 *>(mapped) = 1u;
    chunk_slot_published_buffer_->unlock();
  }

  out_is_new = true;
  return static_cast<i32>(slot);
}

// M2a: a conservative (not tight) bounding radius for a brush shape's own
// local-space extent, used ONLY to decide which coarse cells stamp_shape()'s
// AABB enumeration touches -- NOT fed into the actual SDF evaluation (that
// happens per-voxel, inside Builtin.SculptStamp.comp.glsl's eval_brush_
// shape(), using the real params). Deliberately a single uniform scalar
// applied equally to all 3 axes (exactly like M1a's sphere radius was),
// rather than a tight per-shape/per-axis AABB -- brush rotation is out of
// scope for M2a (every shape stays axis-aligned), and the plan explicitly
// allows "doesn't need to be tight, just not wrong" here.
//
// Per-shape, not a single blanket formula: an earlier version of this
// function just summed all four params unconditionally, which is SAFE
// (never smaller than the true bound) but badly over-estimates any shape
// whose params are independent, orthogonal half-extents rather than
// radially-additive along one axis -- e.g. Box's bound was hx+hy+hz (3x a
// cube's actual half-extent, since hx/hy/hz each already bound their OWN
// axis and don't stack). That over-estimate is not just wasteful: a
// default size=1 Box (half_extents=(1,1,1)) produced bounding_radius=3.0,
// whose ~15,625-coarse-cell enumeration blew straight through
// kMaxSculptStampCellsPerCall (4096) and silently truncated away the
// entire visible near-camera slab of the stamp -- a real, observed bug
// (confirmed empirically while verifying this milestone's Box stamp
// renders), not a hypothetical one. The fix is to actually distinguish the
// two families of shape instead of pretending one formula fits both:
//   - Orthogonal half-extents (Box, RoundBox, BoxFrame, Ellipsoid,
//     Octahedron's single `s`): take the MAX of the relevant params, since
//     each axis's own extent is independent and the uniform radius only
//     needs to cover the LARGEST one.
//   - Radially-additive-along-one-axis shapes (Torus's major+minor;
//     Capsule/RoundCone/Link, whose end caps extend their radius beyond
//     their half-length along the shape's own axis): SUM the params that
//     stack, since the true extent along that axis really is their total.
// `smoothness` is added on top in every case: a smooth blend's influence
// can reach slightly past the shape's own hard surface (see smooth_union()/
// smooth_subtraction()'s own comments on the blend band), so cells just
// outside the hard shape still need to be touched for the blend to read
// correct neighbouring values.
static f32 sculpt_shape_bounding_radius(VulkanRaymarchShader::BrushShape shape_type,
                                        const glm::vec4 &params,
                                        f32 smoothness) {
  using BrushShape = VulkanRaymarchShader::BrushShape;
  const f32 p0 = std::abs(params.x);
  const f32 p1 = std::abs(params.y);
  const f32 p2 = std::abs(params.z);
  const f32 p3 = std::abs(params.w);

  f32 extent;
  switch (shape_type) {
  case BrushShape::Sphere:          // radius
    extent = p0;
    break;
  case BrushShape::Box:             // half_extents
    extent = std::max(p0, std::max(p1, p2));
    break;
  case BrushShape::Plane:           // height (offset) -- see eval_brush_
                                    // shape()'s comment: bounded to a finite
                                    // patch for stamping purposes, not a
                                    // true infinite half-space.
    extent = std::max(p0, 0.01f);
    break;
  case BrushShape::Torus:           // major_radius + minor_radius (radial
                                    // reach is additive)
    extent = p0 + p1;
    break;
  case BrushShape::CappedCylinder:  // radius, half_height -- independent
                                    // axes (flat caps add no extra radial
                                    // reach along the height axis)
    extent = std::max(p0, p1);
    break;
  case BrushShape::CappedCone:      // half_height, r1, r2 -- independent
                                    // axes
    extent = std::max(p0, std::max(p1, p2));
    break;
  case BrushShape::RoundBox:        // half_extents, corner_radius -- the
                                    // IQ formula's half_extents already
                                    // include the rounding (it carves
                                    // corners INWARD), so corner_radius
                                    // doesn't extend the bound further.
    extent = std::max(p0, std::max(p1, p2));
    break;
  case BrushShape::BoxFrame:        // half_extents, edge_thickness -- same
                                    // reasoning as RoundBox.
    extent = std::max(p0, std::max(p1, p2));
    break;
  case BrushShape::Octahedron:      // s
    extent = p0;
    break;
  case BrushShape::Pyramid:         // h, base -- independent axes (height
                                    // vs. base half-extent)
    extent = std::max(p0, p1);
    break;
  case BrushShape::HexPrism:        // inradius, half_height -- independent
                                    // axes; *1.2 pads inradius up toward
                                    // the hexagon's circumradius (1/cos(30
                                    // deg) =~ 1.1547) so the bound covers
                                    // the hex's corners, not just its flats.
    extent = std::max(p0 * 1.2f, p1);
    break;
  case BrushShape::RoundCone:       // r1, r2, half_height -- independent
                                    // axes
    extent = std::max(p0, std::max(p1, p2));
    break;
  case BrushShape::Capsule:         // radius, half_height -- the
                                    // hemispherical caps extend `radius`
                                    // past half_height along the capsule's
                                    // own axis, so that axis's true extent
                                    // is additive.
    extent = p0 + p1;
    break;
  case BrushShape::Link:            // half_length, r1, r2 -- same additive
                                    // reasoning as Capsule, plus a second
                                    // stacked radius.
    extent = p0 + p1 + p2;
    break;
  case BrushShape::Ellipsoid:       // radii -- independent axes
    extent = std::max(p0, std::max(p1, p2));
    break;
  default:
    extent = std::max(p0, std::max(p1, std::max(p2, p3)));
    break;
  }
  return extent + std::max(smoothness, 0.0f);
}

i32 VulkanRaymarchShader::stamp_shape(glm::vec3 center, BrushShape shape_type,
                                      glm::vec4 params, f32 smoothness,
                                      BrushOp op, i32 material_slot,
                                      f32 looseness) {
  if (!valid_) {
    return 0;
  }
  const f32 bounding_radius =
      sculpt_shape_bounding_radius(shape_type, params, smoothness);
  if (!(bounding_radius > 0.0f)) { // also rejects NaN
    KWARN("VulkanRaymarchShader::stamp_shape() ignored a shape whose "
         "bounding radius was non-positive ({:.4f}).",
         bounding_radius);
    return 0;
  }

  ensure_default_sculpt_material();

  // M7 (Smear-latency investigation): wall-clock the whole synchronous
  // critical section below (vkDeviceWaitIdle through end_single_use's own
  // fence wait) -- KTRACE is compiled out entirely in a release build
  // (see core/logger.h), so this costs nothing there; in a build where
  // it's live, it gives a real per-stamp number "for free" the next time
  // anyone drags a Smear/Clone stroke in the tool, without needing a
  // bespoke timing harness. See this method's own doc comment for the
  // empirical finding this was added to let future sessions reconfirm.
  const auto stamp_timing_start = std::chrono::steady_clock::now();

  // Device-wide wait first -- same reasoning as debug_probe_field()/
  // rebake()/set_skybox(): about to rewrite chunk_table_buffer_/chunk_
  // slot_published_buffer_ directly from the CPU and record GPU work that
  // writes chunk_indirection_buffer_/chunk_brick_pool_buffer_/chunk_brick_
  // primitive_buffer_/the brick free list/the cluster pool, all of which a
  // still-in-flight render_to() dispatch could be reading. M1a only ever
  // calls this before the render loop starts, so this is conservative, not
  // (yet) load-bearing.
  vkDeviceWaitIdle(context_->device.logical_device);

  const f32 cell_size = kCoarseCellSize; // 0.25 -- shared with the fixed-cube field
  const f32 voxel_size = cell_size / static_cast<f32>(kChunkBrickDim);

  const glm::ivec3 cell_min_idx(
      glm::floor((center - glm::vec3(bounding_radius)) / cell_size));
  const glm::ivec3 cell_max_idx(
      glm::floor((center + glm::vec3(bounding_radius)) / cell_size));

  // Floor-divide, not a plain `/` -- a negative global cell index must map
  // to a negative chunk coordinate with a POSITIVE local cell (e.g. global
  // cell -1 at kChunkCoarseDim=16 is chunk -1, local cell 15), not truncate
  // toward zero the way C++'s `/`/`%` do for negative operands. Same
  // reasoning as Builtin.ChunkedFieldCommon.inc.glsl's floor_mod3(), just
  // for division instead of modulo.
  auto floor_div = [](i32 a, i32 b) {
    return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b);
  };

  std::vector<GpuSculptStampCell> cells;
  std::vector<u32> new_slots;
  cells.reserve(std::min<size_t>(
      kMaxSculptStampCellsPerCall,
      static_cast<size_t>(cell_max_idx.x - cell_min_idx.x + 1) *
          static_cast<size_t>(cell_max_idx.y - cell_min_idx.y + 1) *
          static_cast<size_t>(cell_max_idx.z - cell_min_idx.z + 1)));

  bool truncated = false;
  for (i32 gz = cell_min_idx.z; gz <= cell_max_idx.z && !truncated; ++gz) {
    for (i32 gy = cell_min_idx.y; gy <= cell_max_idx.y && !truncated; ++gy) {
      for (i32 gx = cell_min_idx.x; gx <= cell_max_idx.x; ++gx) {
        if (cells.size() >= kMaxSculptStampCellsPerCall) {
          truncated = true;
          break;
        }

        const glm::ivec3 chunk_coord(floor_div(gx, static_cast<i32>(kChunkCoarseDim)),
                                     floor_div(gy, static_cast<i32>(kChunkCoarseDim)),
                                     floor_div(gz, static_cast<i32>(kChunkCoarseDim)));
        const glm::ivec3 local_cell(gx - chunk_coord.x * static_cast<i32>(kChunkCoarseDim),
                                    gy - chunk_coord.y * static_cast<i32>(kChunkCoarseDim),
                                    gz - chunk_coord.z * static_cast<i32>(kChunkCoarseDim));

        bool is_new_slot = false;
        const i32 slot = acquire_sculpt_chunk_slot(chunk_coord, is_new_slot);
        if (slot < 0) {
          KWARN("Sculpt stamp ran out of level-0 chunk-table slots "
               "(kMaxResidentChunks exceeded) -- part of this shape will "
               "not be stamped.");
          continue;
        }
        if (is_new_slot) {
          new_slots.push_back(static_cast<u32>(slot));
        }

        const i32 local_cell_index =
            local_cell.x + local_cell.y * static_cast<i32>(kChunkCoarseDim) +
            local_cell.z * static_cast<i32>(kChunkCoarseDim) *
                static_cast<i32>(kChunkCoarseDim);

        GpuSculptStampCell cell{};
        cell.slot_cell = glm::ivec4(slot, local_cell_index, 0, 0);
        const glm::vec3 cell_min(static_cast<f32>(gx) * cell_size,
                                 static_cast<f32>(gy) * cell_size,
                                 static_cast<f32>(gz) * cell_size);
        cell.cell_min_size = glm::vec4(cell_min, cell_size);
        cells.push_back(cell);
      }
    }
  }
  if (truncated) {
    KWARN("Sculpt stamp at ({:.3f}, {:.3f}, {:.3f}) bounding_r={:.3f} touches "
         "more than kMaxSculptStampCellsPerCall ({}) coarse cells -- "
         "truncating.",
         center.x, center.y, center.z, bounding_radius,
         kMaxSculptStampCellsPerCall);
  }

  if (cells.empty()) {
    return 0;
  }

  chunk_sculpt_cell_buffer_->load_data(
      0, cells.size() * sizeof(GpuSculptStampCell), 0, cells.data());

  auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);

  // Brand-new chunk slots start with whatever garbage device memory last
  // held in chunk_indirection_buffer_'s sub-block -- harmless as long as
  // the chunk's table entry still reads -1 (nothing samples it), but that
  // is exactly what is about to change. Every cell in a newly-claimed
  // chunk EXCEPT the ones this stamp fills below must read -1 ("no
  // brick"), or a sibling cell's leftover memory could be misread as a
  // valid brick index the instant the table says "resident".
  for (u32 slot : new_slots) {
    const VkDeviceSize offset =
        static_cast<VkDeviceSize>(slot) * kChunkCellCount * sizeof(i32);
    const VkDeviceSize size = static_cast<VkDeviceSize>(kChunkCellCount) * sizeof(i32);
    vkCmdFillBuffer(cmd->handle(), chunk_indirection_buffer_->handle(), offset,
                    size, 0xFFFFFFFFu);
  }
  if (!new_slots.empty()) {
    VkBufferMemoryBarrier fill_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    fill_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fill_barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    fill_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fill_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fill_barrier.buffer = chunk_indirection_buffer_->handle();
    fill_barrier.offset = 0;
    fill_barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd->handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                        &fill_barrier, 0, nullptr);
  }

  const u32 groups =
      (static_cast<u32>(cells.size()) + kSculptLocalSize - 1) / kSculptLocalSize;

  chunk_sculpt_stamp_pipeline_->bind(*cmd);
  vkCmdBindDescriptorSets(cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         chunk_sculpt_stamp_pipeline_->layout(), 0, 1,
                         &chunk_sculpt_stamp_set_, 0, nullptr);
  SculptStampPushConstants stamp_push{
      center.x,       center.y, center.z, params.x,
      params.y,       params.z, params.w, smoothness,
      voxel_size,     static_cast<i32>(shape_type),
      static_cast<i32>(cells.size()), material_slot,
      static_cast<i32>(op), looseness};
  vkCmdPushConstants(cmd->handle(), chunk_sculpt_stamp_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(stamp_push),
                    &stamp_push);
  vkCmdDispatch(cmd->handle(), groups, 1, 1);

  // The splat-regen pass below reads chunk_indirection_buffer_/chunk_brick_
  // pool_buffer_ (to resolve each cell's brick index and its voxels), both
  // just written above.
  VkBufferMemoryBarrier stamp_barriers[2]{};
  const VkBuffer stamp_written[2] = {chunk_indirection_buffer_->handle(),
                                     chunk_brick_pool_buffer_->handle()};
  for (u32 i = 0; i < 2; ++i) {
    stamp_barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    stamp_barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    stamp_barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    stamp_barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    stamp_barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    stamp_barriers[i].buffer = stamp_written[i];
    stamp_barriers[i].offset = 0;
    stamp_barriers[i].size = VK_WHOLE_SIZE;
  }
  vkCmdPipelineBarrier(cmd->handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2,
                      stamp_barriers, 0, nullptr);

  chunk_sculpt_splat_regen_pipeline_->bind(*cmd);
  vkCmdBindDescriptorSets(cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         chunk_sculpt_splat_regen_pipeline_->layout(), 0, 1,
                         &chunk_sculpt_splat_regen_set_, 0, nullptr);
  SculptSplatRegenPushConstants splat_push{static_cast<i32>(cells.size()),
                                           bake_stats_enabled_ ? 1 : 0,
                                           cell_size, voxel_size};
  vkCmdPushConstants(cmd->handle(), chunk_sculpt_splat_regen_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(splat_push),
                    &splat_push);
  vkCmdDispatch(cmd->handle(), groups, 1, 1);

  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(cmd),
                                      context_->device.graphics_queue);

  // M2b: end_single_use() above already fence-waited for the dispatch to
  // finish, and chunk_sculpt_undo_capture_meta_buffer_/chunk_sculpt_undo_
  // capture_voxel_buffer_ are host-visible/host-coherent -- so the capture
  // the stamp shader just wrote can be mapped and read directly here, no
  // separate device-local-to-staging copy needed (see those buffers' own
  // comment, vulkan_raymarch_shader.h).
  {
    const u32 capture_count =
        std::min<u32>(static_cast<u32>(cells.size()), kMaxSculptUndoCellsPerStamp);
    if (cells.size() > kMaxSculptUndoCellsPerStamp) {
      KWARN("Sculpt stamp at ({:.3f}, {:.3f}, {:.3f}) touched {} cells, more "
           "than kMaxSculptUndoCellsPerStamp ({}) -- undo for this stamp "
           "will only revert the first {} cells; the remainder will stay "
           "stamped if this stamp is later undone.",
           center.x, center.y, center.z, cells.size(),
           kMaxSculptUndoCellsPerStamp, kMaxSculptUndoCellsPerStamp);
    }

    SculptUndoEntry entry;
    entry.cells.resize(capture_count);

    std::vector<GpuUndoCaptureMeta> metas(capture_count);
    if (capture_count > 0) {
      if (void *mapped = chunk_sculpt_undo_capture_meta_buffer_->lock(
              0, static_cast<u64>(capture_count) * sizeof(GpuUndoCaptureMeta),
              0)) {
        std::memcpy(metas.data(), mapped,
                   static_cast<u64>(capture_count) * sizeof(GpuUndoCaptureMeta));
        chunk_sculpt_undo_capture_meta_buffer_->unlock();
      }
    }

    // How many of the capture_count cells actually need their voxel data
    // (change_type == MODIFIED) -- locked and copied once, in one
    // contiguous range, rather than once per cell.
    u32 modified_count = 0;
    for (u32 i = 0; i < capture_count; ++i) {
      if (metas[i].change_type == 2) {
        ++modified_count;
      }
    }
    const f32 *voxel_data = nullptr;
    std::vector<f32> voxel_scratch;
    if (modified_count > 0) {
      const u64 voxel_bytes =
          static_cast<u64>(capture_count) * kChunkBrickVoxelCount * sizeof(f32);
      if (void *mapped =
              chunk_sculpt_undo_capture_voxel_buffer_->lock(0, voxel_bytes, 0)) {
        voxel_scratch.resize(static_cast<size_t>(capture_count) * kChunkBrickVoxelCount);
        std::memcpy(voxel_scratch.data(), mapped, voxel_bytes);
        chunk_sculpt_undo_capture_voxel_buffer_->unlock();
        voxel_data = voxel_scratch.data();
      }
    }

    for (u32 i = 0; i < capture_count; ++i) {
      SculptUndoCell &out = entry.cells[i];
      out.chunk_slot = cells[i].slot_cell.x;
      out.local_cell_index = cells[i].slot_cell.y;
      out.cell_min = glm::vec3(cells[i].cell_min_size);
      out.change_type = metas[i].change_type;
      out.old_material = metas[i].old_material;
      out.old_looseness = metas[i].old_looseness;
      if (out.change_type == 2 && voxel_data != nullptr) {
        out.old_voxels.assign(voxel_data + static_cast<u64>(i) * kChunkBrickVoxelCount,
                              voxel_data + static_cast<u64>(i + 1) * kChunkBrickVoxelCount);
      }
    }

    undo_stack_.push_back(std::move(entry));
    if (undo_stack_.size() > kMaxSculptUndoDepth) {
      // Oldest entry drops off -- see undo_stack_'s own comment.
      undo_stack_.erase(undo_stack_.begin());
    }
  }

  // New geometry just appeared -- any accumulated TAA history predates it.
  invalidate_temporal_history();

  const auto stamp_timing_end = std::chrono::steady_clock::now();
  const f64 stamp_ms =
      std::chrono::duration<f64, std::milli>(stamp_timing_end -
                                             stamp_timing_start)
          .count();
  KTRACE("VulkanRaymarchShader::stamp_shape() touched {} cell(s) in {:.3f} "
        "ms (vkDeviceWaitIdle + dispatch + fence wait + undo capture).",
        cells.size(), stamp_ms);

  return static_cast<i32>(cells.size());
}

i32 VulkanRaymarchShader::spray_paint(glm::vec3 center, BrushShape shape_type,
                                      glm::vec4 params, f32 opacity_per_pass) {
  if (!valid_) {
    return 0;
  }
  // No `smoothness` concept for a paint-only brush (see this method's own
  // header comment) -- 0.0f here only widens stamp_shape()'s shared bounding-
  // radius helper by nothing, which is exactly the hard-edged brush extent
  // Spraypaint actually wants (paint reaches exactly as far as the shape
  // itself, see PAINT_THRESHOLD in Builtin.SculptSpraypaint.comp.glsl).
  const f32 bounding_radius =
      sculpt_shape_bounding_radius(shape_type, params, 0.0f);
  if (!(bounding_radius > 0.0f)) { // also rejects NaN
    KWARN("VulkanRaymarchShader::spray_paint() ignored a shape whose "
         "bounding radius was non-positive ({:.4f}).",
         bounding_radius);
    return 0;
  }

  // Same conservative-wait reasoning as stamp_shape()'s own comment -- about
  // to record GPU work that writes chunk_brick_paint_buffer_, which a still-
  // in-flight render_to() dispatch (specifically Builtin.DeferredShade.
  // comp.glsl's own read of it) could be reading.
  vkDeviceWaitIdle(context_->device.logical_device);

  const f32 cell_size = kCoarseCellSize;
  const f32 voxel_size = cell_size / static_cast<f32>(kChunkBrickDim);

  const glm::ivec3 cell_min_idx(
      glm::floor((center - glm::vec3(bounding_radius)) / cell_size));
  const glm::ivec3 cell_max_idx(
      glm::floor((center + glm::vec3(bounding_radius)) / cell_size));

  // Same floor-divide as stamp_shape() -- see its own comment.
  auto floor_div = [](i32 a, i32 b) {
    return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b);
  };

  std::vector<GpuSculptStampCell> cells;
  cells.reserve(std::min<size_t>(
      kMaxSculptStampCellsPerCall,
      static_cast<size_t>(cell_max_idx.x - cell_min_idx.x + 1) *
          static_cast<size_t>(cell_max_idx.y - cell_min_idx.y + 1) *
          static_cast<size_t>(cell_max_idx.z - cell_min_idx.z + 1)));

  bool truncated = false;
  for (i32 gz = cell_min_idx.z; gz <= cell_max_idx.z && !truncated; ++gz) {
    for (i32 gy = cell_min_idx.y; gy <= cell_max_idx.y && !truncated; ++gy) {
      for (i32 gx = cell_min_idx.x; gx <= cell_max_idx.x; ++gx) {
        if (cells.size() >= kMaxSculptStampCellsPerCall) {
          truncated = true;
          break;
        }

        const glm::ivec3 chunk_coord(floor_div(gx, static_cast<i32>(kChunkCoarseDim)),
                                     floor_div(gy, static_cast<i32>(kChunkCoarseDim)),
                                     floor_div(gz, static_cast<i32>(kChunkCoarseDim)));
        const glm::ivec3 local_cell(gx - chunk_coord.x * static_cast<i32>(kChunkCoarseDim),
                                    gy - chunk_coord.y * static_cast<i32>(kChunkCoarseDim),
                                    gz - chunk_coord.z * static_cast<i32>(kChunkCoarseDim));

        // LOOKUP ONLY -- unlike stamp_shape()'s acquire_sculpt_chunk_slot(),
        // this never allocates a fresh chunk-table slot. A chunk nothing
        // has ever stamped into has never had any bricks allocated in it
        // either, so there is categorically nothing in it for a spray to
        // paint -- skip the whole chunk rather than manufacturing a
        // resident-but-empty table entry for it (see spray_paint()'s own
        // header comment).
        const ChunkKey key{/*level=*/0, chunk_coord.x, chunk_coord.y,
                           chunk_coord.z};
        const auto it = sculpt_chunk_slot_by_coord_.find(key);
        if (it == sculpt_chunk_slot_by_coord_.end()) {
          continue;
        }
        const i32 slot = static_cast<i32>(it->second);

        const i32 local_cell_index =
            local_cell.x + local_cell.y * static_cast<i32>(kChunkCoarseDim) +
            local_cell.z * static_cast<i32>(kChunkCoarseDim) *
                static_cast<i32>(kChunkCoarseDim);

        GpuSculptStampCell cell{};
        cell.slot_cell = glm::ivec4(slot, local_cell_index, 0, 0);
        const glm::vec3 cell_min(static_cast<f32>(gx) * cell_size,
                                 static_cast<f32>(gy) * cell_size,
                                 static_cast<f32>(gz) * cell_size);
        cell.cell_min_size = glm::vec4(cell_min, cell_size);
        cells.push_back(cell);
      }
    }
  }
  if (truncated) {
    KWARN("Spraypaint at ({:.3f}, {:.3f}, {:.3f}) bounding_r={:.3f} touches "
         "more than kMaxSculptStampCellsPerCall ({}) coarse cells -- "
         "truncating.",
         center.x, center.y, center.z, bounding_radius,
         kMaxSculptStampCellsPerCall);
  }

  if (cells.empty()) {
    // Either every touched chunk has never been stamped into (spraying over
    // empty space -- correctly a no-op, see this method's header comment),
    // or the shape's bounding radius was somehow degenerate.
    return 0;
  }

  chunk_sculpt_cell_buffer_->load_data(
      0, cells.size() * sizeof(GpuSculptStampCell), 0, cells.data());

  auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);

  const u32 groups =
      (static_cast<u32>(cells.size()) + kSculptLocalSize - 1) / kSculptLocalSize;

  chunk_sculpt_spraypaint_pipeline_->bind(*cmd);
  vkCmdBindDescriptorSets(cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         chunk_sculpt_spraypaint_pipeline_->layout(), 0, 1,
                         &chunk_sculpt_spraypaint_set_, 0, nullptr);
  SculptSpraypaintPushConstants spray_push{
      center.x,   center.y, center.z, params.x,
      params.y,   params.z, params.w, voxel_size,
      static_cast<i32>(shape_type), static_cast<i32>(cells.size()),
      opacity_per_pass};
  vkCmdPushConstants(cmd->handle(), chunk_sculpt_spraypaint_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(spray_push),
                    &spray_push);
  vkCmdDispatch(cmd->handle(), groups, 1, 1);

  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(cmd),
                                      context_->device.graphics_queue);

  // No splat-regen tail (unlike stamp_shape()) -- painting changes no
  // distance/point-cloud data, only a tint Builtin.DeferredShade.comp.glsl
  // reads directly, so there is nothing for the splat pipeline to
  // regenerate. TAA history IS invalidated, though (same call stamp_shape()
  // makes): the shaded COLOUR at these pixels just changed even though the
  // underlying surface didn't, and letting TAA keep blending several old,
  // pre-spray frames in would read as the new paint fading in over the
  // next few frames instead of appearing immediately.
  invalidate_temporal_history();
  return static_cast<i32>(cells.size());
}

bool VulkanRaymarchShader::undo_stamp() {
  if (!valid_ || undo_stack_.empty()) {
    return false;
  }

  SculptUndoEntry entry = std::move(undo_stack_.back());
  undo_stack_.pop_back();
  if (entry.cells.empty()) {
    // A stamp that touched zero cells (e.g. the pool was exhausted for
    // every single one) -- nothing to actually undo, but still a
    // successful pop as far as the caller's bookkeeping is concerned.
    return true;
  }

  // Same device-wide wait stamp_shape() itself takes -- about to rewrite
  // chunk_indirection_buffer_/chunk_brick_pool_buffer_/chunk_brick_
  // primitive_buffer_/the brick free list/the cluster pool directly, all of
  // which a still-in-flight render_to() dispatch could be reading.
  vkDeviceWaitIdle(context_->device.logical_device);

  const u32 cell_count = static_cast<u32>(entry.cells.size());

  // Re-upload the ORIGINAL stamp's cell list -- chunk_sculpt_cell_buffer_ is
  // shared by the restore pass and the splat-regen pass below, exactly like
  // stamp_shape()'s own single upload serves both of ITS dispatches.
  std::vector<GpuSculptStampCell> gpu_cells(cell_count);
  std::vector<GpuUndoCaptureMeta> gpu_metas(cell_count);
  u32 modified_count = 0;
  for (u32 i = 0; i < cell_count; ++i) {
    const SculptUndoCell &cell = entry.cells[i];
    gpu_cells[i].slot_cell = glm::ivec4(cell.chunk_slot, cell.local_cell_index, 0, 0);
    gpu_cells[i].cell_min_size = glm::vec4(cell.cell_min, kCoarseCellSize);
    gpu_metas[i].change_type = cell.change_type;
    gpu_metas[i].old_material = cell.old_material;
    gpu_metas[i].old_looseness = cell.old_looseness;
    gpu_metas[i].pad1 = 0;
    if (cell.change_type == 2) {
      ++modified_count;
    }
  }
  chunk_sculpt_cell_buffer_->load_data(
      0, static_cast<u64>(cell_count) * sizeof(GpuSculptStampCell), 0,
      gpu_cells.data());
  chunk_sculpt_undo_capture_meta_buffer_->load_data(
      0, static_cast<u64>(cell_count) * sizeof(GpuUndoCaptureMeta), 0,
      gpu_metas.data());
  if (modified_count > 0) {
    // Flat voxel scratch matching UndoCaptureVoxelBuffer's own layout
    // (stride kChunkBrickVoxelCount per cell slot, valid only for
    // change_type == 2 slots) -- cells with no captured voxels leave their
    // slice as whatever this scratch happens to hold (zero-initialized),
    // which is harmless: Builtin.SculptUndoRestore.comp.glsl never reads a
    // NOOP/NEW_BRICK cell's slice.
    std::vector<f32> voxel_upload(
        static_cast<size_t>(cell_count) * kChunkBrickVoxelCount, 0.0f);
    for (u32 i = 0; i < cell_count; ++i) {
      const SculptUndoCell &cell = entry.cells[i];
      if (cell.change_type == 2 && cell.old_voxels.size() == kChunkBrickVoxelCount) {
        std::memcpy(voxel_upload.data() + static_cast<u64>(i) * kChunkBrickVoxelCount,
                   cell.old_voxels.data(), kChunkBrickVoxelCount * sizeof(f32));
      }
    }
    chunk_sculpt_undo_capture_voxel_buffer_->load_data(
        0, voxel_upload.size() * sizeof(f32), 0, voxel_upload.data());
  }

  auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);

  const u32 groups = (cell_count + kSculptLocalSize - 1) / kSculptLocalSize;

  chunk_sculpt_undo_restore_pipeline_->bind(*cmd);
  vkCmdBindDescriptorSets(cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         chunk_sculpt_undo_restore_pipeline_->layout(), 0, 1,
                         &chunk_sculpt_undo_restore_set_, 0, nullptr);
  SculptUndoRestorePushConstants restore_push{static_cast<i32>(cell_count)};
  vkCmdPushConstants(cmd->handle(), chunk_sculpt_undo_restore_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(restore_push),
                    &restore_push);
  vkCmdDispatch(cmd->handle(), groups, 1, 1);

  // The splat-regen pass below reads chunk_indirection_buffer_/chunk_brick_
  // pool_buffer_, both just written (or cleared) above.
  VkBufferMemoryBarrier restore_barriers[2]{};
  const VkBuffer restore_written[2] = {chunk_indirection_buffer_->handle(),
                                       chunk_brick_pool_buffer_->handle()};
  for (u32 i = 0; i < 2; ++i) {
    restore_barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    restore_barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    restore_barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    restore_barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restore_barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restore_barriers[i].buffer = restore_written[i];
    restore_barriers[i].offset = 0;
    restore_barriers[i].size = VK_WHOLE_SIZE;
  }
  vkCmdPipelineBarrier(cmd->handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2,
                      restore_barriers, 0, nullptr);

  // Same splat-regen pass/set stamp_shape() uses, completely unmodified --
  // see Builtin.SculptUndoRestore.comp.glsl's own header comment for why
  // its defensive brick_index<0 check already makes this correct for every
  // change_type this dispatch could have produced.
  chunk_sculpt_splat_regen_pipeline_->bind(*cmd);
  vkCmdBindDescriptorSets(cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         chunk_sculpt_splat_regen_pipeline_->layout(), 0, 1,
                         &chunk_sculpt_splat_regen_set_, 0, nullptr);
  const f32 voxel_size = kCoarseCellSize / static_cast<f32>(kChunkBrickDim);
  SculptSplatRegenPushConstants splat_push{static_cast<i32>(cell_count),
                                           bake_stats_enabled_ ? 1 : 0,
                                           kCoarseCellSize, voxel_size};
  vkCmdPushConstants(cmd->handle(), chunk_sculpt_splat_regen_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(splat_push),
                    &splat_push);
  vkCmdDispatch(cmd->handle(), groups, 1, 1);

  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(cmd),
                                      context_->device.graphics_queue);

  // Geometry just changed back -- any accumulated TAA history predates it.
  invalidate_temporal_history();

  return true;
}

void VulkanRaymarchShader::capture_sculpt_checkpoint(i32 after_edit_index) {
  if (!valid_) {
    return;
  }
  vkDeviceWaitIdle(context_->device.logical_device);

  SculptCheckpoint checkpoint;
  checkpoint.after_edit_index = after_edit_index;

  auto push_checkpoint = [this](SculptCheckpoint &&cp) {
    sculpt_checkpoints_.push_back(std::move(cp));
    if (sculpt_checkpoints_.size() > kMaxSculptCheckpoints) {
      // Oldest drops off -- see sculpt_checkpoints_'s own comment.
      sculpt_checkpoints_.erase(sculpt_checkpoints_.begin());
    }
  };

  if (sculpt_chunk_slot_by_coord_.empty()) {
    // Nothing resident -- a valid ("empty") checkpoint, same as what
    // restore_sculpt_checkpoint_before() falls back to when none exists at
    // all. Still worth recording: a later delete_edit() targeting an index
    // shortly after this one can restore straight to empty instead of
    // replaying everything from scratch.
    push_checkpoint(std::move(checkpoint));
    return;
  }

  // 1. One bulk copy of level 0's ENTIRE indirection sub-range (the only
  // range sculpting ever touches -- see the plan's "level 0 only"
  // architecture note). Cheap (kMaxResidentChunks * kChunkCellCount i32s =
  // 1MB) even though most of it reads -1 (unused) -- a single flat scan is
  // simpler and just as fast as restricting the copy to only the slots
  // sculpt_chunk_slot_by_coord_ already knows about.
  const u64 level0_entries =
      static_cast<u64>(kMaxResidentChunks) * kChunkCellCount;
  const u64 level0_bytes = level0_entries * sizeof(i32);
  std::vector<i32> indirection(static_cast<size_t>(level0_entries));
  {
    VulkanBuffer staging(*context_, level0_bytes,
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!staging.is_valid()) {
      KERROR("capture_sculpt_checkpoint(): failed to allocate the "
            "indirection staging buffer -- checkpoint capture aborted for "
            "edit index {}.",
            after_edit_index);
      return;
    }
    chunk_indirection_buffer_->copy_to(staging, 0, 0, level0_bytes,
                                      context_->device.graphics_queue,
                                      context_->device.graphics_command_pool);
    if (void *mapped = staging.lock(0, level0_bytes, 0)) {
      std::memcpy(indirection.data(), mapped, level0_bytes);
      staging.unlock();
    }
  }

  // 2. Scan: for every chunk this session has a slot for, walk its
  // CHUNK_CELL_COUNT local cells and collect every occupied one's brick
  // index, grouped per chunk so step 4 below can rebuild SculptCheckpoint
  // Chunk entries directly.
  struct PendingCellRef {
    glm::ivec3 chunk_coord;
    i32 local_cell_index;
    i32 brick_index;
  };
  std::vector<PendingCellRef> pending;
  for (const auto &entry : sculpt_chunk_slot_by_coord_) {
    const ChunkKey &key = entry.first;
    const u32 slot = entry.second;
    const u64 base = static_cast<u64>(slot) * kChunkCellCount;
    for (u32 local = 0; local < kChunkCellCount; ++local) {
      const i32 brick_index = indirection[static_cast<size_t>(base + local)];
      if (brick_index < 0) {
        continue;
      }
      pending.push_back(PendingCellRef{
          glm::ivec3(static_cast<i32>(key.cx), static_cast<i32>(key.cy),
                    static_cast<i32>(key.cz)),
          static_cast<i32>(local), brick_index});
    }
  }

  if (pending.empty()) {
    // Every known chunk slot is actually empty (shouldn't normally happen --
    // acquire_sculpt_chunk_slot() is only ever called right before a cell
    // gets a brick -- but harmless either way: an empty checkpoint is still
    // a correct, if trivial, restore target).
    push_checkpoint(std::move(checkpoint));
    return;
  }

  // 3. One multi-region copy PER SOURCE BUFFER, covering every pending
  // cell's own (scattered) brick offset -- never one round trip per brick.
  // See VulkanBuffer::copy_regions_to()'s own comment.
  const u64 material_bytes = static_cast<u64>(pending.size()) * sizeof(i32);
  const u64 voxel_bytes = static_cast<u64>(pending.size()) *
      kChunkBrickVoxelCount * sizeof(f32);

  // M3 (Looseness): same size/layout as material_bytes above -- one f32 per
  // pending brick.
  const u64 looseness_bytes = static_cast<u64>(pending.size()) * sizeof(f32);

  VulkanBuffer material_staging(*context_, material_bytes,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VulkanBuffer voxel_staging(*context_, voxel_bytes,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VulkanBuffer looseness_staging(*context_, looseness_bytes,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!material_staging.is_valid() || !voxel_staging.is_valid() ||
      !looseness_staging.is_valid()) {
    KERROR("capture_sculpt_checkpoint(): failed to allocate brick staging "
          "buffers for {} bricks -- checkpoint capture aborted for edit "
          "index {}.",
          pending.size(), after_edit_index);
    return;
  }

  std::vector<VkBufferCopy> material_regions(pending.size());
  std::vector<VkBufferCopy> voxel_regions(pending.size());
  std::vector<VkBufferCopy> looseness_regions(pending.size());
  for (size_t i = 0; i < pending.size(); ++i) {
    const u64 brick_index = static_cast<u64>(pending[i].brick_index);
    material_regions[i].srcOffset = brick_index * sizeof(i32);
    material_regions[i].dstOffset = static_cast<u64>(i) * sizeof(i32);
    material_regions[i].size = sizeof(i32);

    voxel_regions[i].srcOffset =
        brick_index * kChunkBrickVoxelCount * sizeof(f32);
    voxel_regions[i].dstOffset =
        static_cast<u64>(i) * kChunkBrickVoxelCount * sizeof(f32);
    voxel_regions[i].size = kChunkBrickVoxelCount * sizeof(f32);

    looseness_regions[i].srcOffset = brick_index * sizeof(f32);
    looseness_regions[i].dstOffset = static_cast<u64>(i) * sizeof(f32);
    looseness_regions[i].size = sizeof(f32);
  }
  chunk_brick_primitive_buffer_->copy_regions_to(
      material_staging, material_regions, context_->device.graphics_queue,
      context_->device.graphics_command_pool);
  chunk_brick_pool_buffer_->copy_regions_to(
      voxel_staging, voxel_regions, context_->device.graphics_queue,
      context_->device.graphics_command_pool);
  chunk_brick_looseness_buffer_->copy_regions_to(
      looseness_staging, looseness_regions, context_->device.graphics_queue,
      context_->device.graphics_command_pool);

  std::vector<i32> materials(pending.size());
  std::vector<f32> voxels(pending.size() * kChunkBrickVoxelCount);
  std::vector<f32> loosenesses(pending.size());
  if (void *mapped = material_staging.lock(0, material_bytes, 0)) {
    std::memcpy(materials.data(), mapped, material_bytes);
    material_staging.unlock();
  }
  if (void *mapped = voxel_staging.lock(0, voxel_bytes, 0)) {
    std::memcpy(voxels.data(), mapped, voxel_bytes);
    voxel_staging.unlock();
  }
  if (void *mapped = looseness_staging.lock(0, looseness_bytes, 0)) {
    std::memcpy(loosenesses.data(), mapped, looseness_bytes);
    looseness_staging.unlock();
  }

  // 4. Group pending[]/materials[]/voxels[] (all three share pending's own
  // index order) back into per-chunk SculptCheckpointChunk entries.
  std::unordered_map<ChunkKey, size_t, ChunkKeyHash> chunk_index_of;
  for (size_t i = 0; i < pending.size(); ++i) {
    const PendingCellRef &ref = pending[i];
    const ChunkKey key{/*level=*/0, ref.chunk_coord.x, ref.chunk_coord.y,
                      ref.chunk_coord.z};
    auto it = chunk_index_of.find(key);
    size_t chunk_idx;
    if (it == chunk_index_of.end()) {
      chunk_idx = checkpoint.chunks.size();
      checkpoint.chunks.push_back(
          SculptCheckpointChunk{ref.chunk_coord, {}});
      chunk_index_of.emplace(key, chunk_idx);
    } else {
      chunk_idx = it->second;
    }

    SculptCheckpointBrick brick;
    brick.local_cell_index = ref.local_cell_index;
    brick.material = materials[i];
    brick.looseness = loosenesses[i];
    brick.voxels.assign(voxels.begin() +
                           static_cast<std::ptrdiff_t>(i * kChunkBrickVoxelCount),
                       voxels.begin() + static_cast<std::ptrdiff_t>(
                                            (i + 1) * kChunkBrickVoxelCount));
    checkpoint.chunks[chunk_idx].bricks.push_back(std::move(brick));
  }

  push_checkpoint(std::move(checkpoint));
}

i32 VulkanRaymarchShader::restore_sculpt_checkpoint_before(i32 edit_index) {
  if (!valid_) {
    return -1;
  }
  // Conservative device-wide wait, same reasoning as stamp_shape()/undo_
  // stamp()'s own -- about to rewrite every chunk/brick/cluster buffer from
  // underneath whatever a still-in-flight render_to() dispatch might be
  // reading. Everything restore_sculpt_checkpoint()/reset_chunked_field()
  // do below is itself synchronous and self-fenced, so one wait here is
  // enough -- nothing after this point needs to wait again.
  vkDeviceWaitIdle(context_->device.logical_device);

  const SculptCheckpoint *best = nullptr;
  for (const SculptCheckpoint &cp : sculpt_checkpoints_) {
    if (cp.after_edit_index < edit_index &&
        (best == nullptr || cp.after_edit_index > best->after_edit_index)) {
      best = &cp;
    }
  }

  if (best == nullptr) {
    // No checkpoint old enough to help -- "state before edit_index" is the
    // empty volume (either edit_index is small, or this session simply
    // hasn't captured one yet -- see kMaxSculptCheckpoints' cadence).
    reset_chunked_field();
    return -1;
  }

  // Copied out rather than kept as a dangling reference across
  // restore_sculpt_checkpoint() -- that call's own reset_chunked_field()
  // doesn't touch sculpt_checkpoints_ today, but copying this small struct
  // is cheap insurance against a future change silently invalidating `best`
  // out from under this function.
  const SculptCheckpoint snapshot = *best;
  const i32 restored_index = snapshot.after_edit_index;
  restore_sculpt_checkpoint(snapshot);
  return restored_index;
}

void VulkanRaymarchShader::invalidate_sculpt_checkpoints_from(i32 edit_index) {
  sculpt_checkpoints_.erase(
      std::remove_if(sculpt_checkpoints_.begin(), sculpt_checkpoints_.end(),
                    [edit_index](const SculptCheckpoint &cp) {
                      return cp.after_edit_index >= edit_index;
                    }),
      sculpt_checkpoints_.end());
}

void VulkanRaymarchShader::restore_sculpt_checkpoint(
    const SculptCheckpoint &checkpoint) {
  // Clean slate first -- also clears undo_stack_ (every entry in it refers
  // to chunk slots/brick indices this is about to reshuffle) and the M1a/
  // M2a sculpt-chunk-slot bookkeeping (sculpt_chunk_slot_used_/sculpt_
  // chunk_slot_by_coord_), exactly like a fresh session start.
  reset_chunked_field();

  if (checkpoint.chunks.empty()) {
    return; // Truly empty state -- nothing further to populate.
  }

  struct PendingCell {
    i32 slot = 0;
    i32 local_cell_index = 0;
    glm::vec3 cell_min{0.0f};
    i32 material = 0;
    f32 looseness = 0.0f; // M3 (Looseness): captured alongside material.
    const std::vector<f32> *voxels = nullptr;
  };
  std::vector<PendingCell> pending;
  std::vector<u32> new_slots;

  for (const SculptCheckpointChunk &chunk : checkpoint.chunks) {
    bool is_new = false;
    const i32 slot = acquire_sculpt_chunk_slot(chunk.chunk_coord, is_new);
    if (slot < 0) {
      KWARN("restore_sculpt_checkpoint(): ran out of level-0 chunk-table "
           "slots while restoring a checkpoint -- some geometry will be "
           "missing from the restored state.");
      continue;
    }
    if (is_new) {
      new_slots.push_back(static_cast<u32>(slot));
    }
    for (const SculptCheckpointBrick &brick : chunk.bricks) {
      // Decode local_cell_index back into (lx, ly, lz) -- exact inverse of
      // stamp_shape()'s own local_cell.x + local_cell.y*DIM +
      // local_cell.z*DIM*DIM packing.
      const i32 dim = static_cast<i32>(kChunkCoarseDim);
      const i32 lx = brick.local_cell_index % dim;
      const i32 ly = (brick.local_cell_index / dim) % dim;
      const i32 lz = brick.local_cell_index / (dim * dim);
      const glm::vec3 cell_min(
          static_cast<f32>(chunk.chunk_coord.x * dim + lx) * kCoarseCellSize,
          static_cast<f32>(chunk.chunk_coord.y * dim + ly) * kCoarseCellSize,
          static_cast<f32>(chunk.chunk_coord.z * dim + lz) * kCoarseCellSize);
      pending.push_back(PendingCell{slot, brick.local_cell_index, cell_min,
                                    brick.material, brick.looseness,
                                    &brick.voxels});
    }
  }

  if (pending.empty()) {
    return;
  }

  // Fill every newly-claimed slot's indirection sub-block to -1 FIRST, in
  // one command buffer, before any batch below writes a real brick index
  // into it -- exact same reasoning as stamp_shape()'s own new_slots
  // handling.
  if (!new_slots.empty()) {
    auto fill_cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
        *context_, context_->device.graphics_command_pool);
    for (u32 slot : new_slots) {
      const VkDeviceSize offset =
          static_cast<VkDeviceSize>(slot) * kChunkCellCount * sizeof(i32);
      const VkDeviceSize size =
          static_cast<VkDeviceSize>(kChunkCellCount) * sizeof(i32);
      vkCmdFillBuffer(fill_cmd->handle(), chunk_indirection_buffer_->handle(),
                      offset, size, 0xFFFFFFFFu);
    }
    VulkanCommandBuffer::end_single_use(*context_,
                                        context_->device.graphics_command_pool,
                                        std::move(fill_cmd),
                                        context_->device.graphics_queue);
  }

  // Batched at kMaxSculptUndoCellsPerStamp entries per dispatch -- reuses
  // chunk_sculpt_cell_buffer_/chunk_sculpt_undo_capture_meta_buffer_/
  // chunk_sculpt_undo_capture_voxel_buffer_'s EXISTING fixed-size scratch
  // rather than allocating new, checkpoint-sized buffers: a checkpoint can
  // easily cover far more cells in total than either of those buffers'
  // single-call capacity, so this loops batches the same way a stamp
  // touching an unusually large area would have to, rather than truncating.
  const f32 voxel_size = kCoarseCellSize / static_cast<f32>(kChunkBrickDim);
  const size_t batch_cap = kMaxSculptUndoCellsPerStamp;
  for (size_t start = 0; start < pending.size(); start += batch_cap) {
    const size_t count = std::min(batch_cap, pending.size() - start);

    std::vector<GpuSculptStampCell> gpu_cells(count);
    std::vector<GpuUndoCaptureMeta> gpu_meta(count);
    std::vector<f32> voxel_upload(count * kChunkBrickVoxelCount);
    for (size_t i = 0; i < count; ++i) {
      const PendingCell &cell = pending[start + i];
      gpu_cells[i].slot_cell = glm::ivec4(cell.slot, cell.local_cell_index, 0, 0);
      gpu_cells[i].cell_min_size = glm::vec4(cell.cell_min, kCoarseCellSize);
      gpu_meta[i].change_type = 0; // Unused by the checkpoint-restore shader.
      gpu_meta[i].old_material = cell.material; // Repurposed as "material
                                                // to restore" -- see the
                                                // shader's own comment.
      // M3 (Looseness): old_looseness is likewise repurposed here as
      // "looseness to restore" -- same convention as old_material
      // immediately above.
      gpu_meta[i].old_looseness = cell.looseness;
      gpu_meta[i].pad1 = 0;
      std::memcpy(voxel_upload.data() + i * kChunkBrickVoxelCount,
                 cell.voxels->data(), kChunkBrickVoxelCount * sizeof(f32));
    }

    chunk_sculpt_cell_buffer_->load_data(
        0, count * sizeof(GpuSculptStampCell), 0, gpu_cells.data());
    chunk_sculpt_undo_capture_meta_buffer_->load_data(
        0, count * sizeof(GpuUndoCaptureMeta), 0, gpu_meta.data());
    chunk_sculpt_undo_capture_voxel_buffer_->load_data(
        0, voxel_upload.size() * sizeof(f32), 0, voxel_upload.data());

    auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
        *context_, context_->device.graphics_command_pool);
    const u32 groups =
        (static_cast<u32>(count) + kSculptLocalSize - 1) / kSculptLocalSize;

    chunk_sculpt_checkpoint_restore_pipeline_->bind(*cmd);
    vkCmdBindDescriptorSets(
        cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
        chunk_sculpt_checkpoint_restore_pipeline_->layout(), 0, 1,
        &chunk_sculpt_stamp_set_, 0, nullptr);
    SculptUndoRestorePushConstants restore_push{static_cast<i32>(count)};
    vkCmdPushConstants(cmd->handle(),
                      chunk_sculpt_checkpoint_restore_pipeline_->layout(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(restore_push),
                      &restore_push);
    vkCmdDispatch(cmd->handle(), groups, 1, 1);

    // The splat-regen pass below reads chunk_indirection_buffer_/chunk_
    // brick_pool_buffer_, both just written above.
    VkBufferMemoryBarrier barriers[2]{};
    const VkBuffer written[2] = {chunk_indirection_buffer_->handle(),
                                 chunk_brick_pool_buffer_->handle()};
    for (u32 i = 0; i < 2; ++i) {
      barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[i].buffer = written[i];
      barriers[i].offset = 0;
      barriers[i].size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(cmd->handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2,
                        barriers, 0, nullptr);

    chunk_sculpt_splat_regen_pipeline_->bind(*cmd);
    vkCmdBindDescriptorSets(cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                           chunk_sculpt_splat_regen_pipeline_->layout(), 0, 1,
                           &chunk_sculpt_splat_regen_set_, 0, nullptr);
    SculptSplatRegenPushConstants splat_push{static_cast<i32>(count),
                                             bake_stats_enabled_ ? 1 : 0,
                                             kCoarseCellSize, voxel_size};
    vkCmdPushConstants(cmd->handle(), chunk_sculpt_splat_regen_pipeline_->layout(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(splat_push),
                      &splat_push);
    vkCmdDispatch(cmd->handle(), groups, 1, 1);

    VulkanCommandBuffer::end_single_use(*context_,
                                        context_->device.graphics_command_pool,
                                        std::move(cmd),
                                        context_->device.graphics_queue);
  }

  invalidate_temporal_history();
}

i32 VulkanRaymarchShader::crop_volume(glm::vec3 center, BrushShape shape_type,
                                      glm::vec4 params, f32 smoothness,
                                      bool keep_outside) {
  if (!valid_) {
    return 0;
  }
  const f32 bounding_radius =
      sculpt_shape_bounding_radius(shape_type, params, smoothness);
  if (!(bounding_radius > 0.0f)) { // also rejects NaN
    KWARN("VulkanRaymarchShader::crop_volume() ignored a shape whose "
         "bounding radius was non-positive ({:.4f}).",
         bounding_radius);
    return 0;
  }
  if (sculpt_chunk_slot_by_coord_.empty()) {
    return 0; // Nothing resident -- a correct no-op, not an error.
  }

  // Conservative device-wide wait, same reasoning as every other sculpt-
  // family call -- about to rewrite chunk_indirection_buffer_/chunk_brick_
  // pool_buffer_/the brick+cluster free lists directly.
  vkDeviceWaitIdle(context_->device.logical_device);

  // 1. One bulk copy of level 0's ENTIRE indirection sub-range -- identical
  // first step to capture_sculpt_checkpoint()'s own (see its comment for
  // why a flat scan is simpler and just as fast as restricting the copy).
  const u64 level0_entries =
      static_cast<u64>(kMaxResidentChunks) * kChunkCellCount;
  const u64 level0_bytes = level0_entries * sizeof(i32);
  std::vector<i32> indirection(static_cast<size_t>(level0_entries));
  {
    VulkanBuffer staging(*context_, level0_bytes,
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!staging.is_valid()) {
      KERROR("crop_volume(): failed to allocate the indirection staging "
            "buffer -- crop aborted.");
      return 0;
    }
    chunk_indirection_buffer_->copy_to(staging, 0, 0, level0_bytes,
                                      context_->device.graphics_queue,
                                      context_->device.graphics_command_pool);
    if (void *mapped = staging.lock(0, level0_bytes, 0)) {
      std::memcpy(indirection.data(), mapped, level0_bytes);
      staging.unlock();
    }
  }

  // 2. Every occupied (slot, local_cell_index) pair across EVERY chunk slot
  // this session has ever allocated -- deliberately NOT limited to the
  // crop shape's own AABB (see this method's own header comment,
  // vulkan_raymarch_shader.h, for why: Crop/Cutout constrain the WHOLE
  // volume, not just what one shape's bounding box would touch).
  struct PendingCell {
    i32 slot = 0;
    i32 local_cell_index = 0;
    glm::vec3 cell_min{0.0f};
  };
  std::vector<PendingCell> pending;
  for (const auto &entry : sculpt_chunk_slot_by_coord_) {
    const ChunkKey &key = entry.first;
    const u32 slot = entry.second;
    const u64 base = static_cast<u64>(slot) * kChunkCellCount;
    for (u32 local = 0; local < kChunkCellCount; ++local) {
      if (indirection[static_cast<size_t>(base + local)] < 0) {
        continue;
      }
      const i32 dim = static_cast<i32>(kChunkCoarseDim);
      const i32 lx = static_cast<i32>(local) % dim;
      const i32 ly = (static_cast<i32>(local) / dim) % dim;
      const i32 lz = static_cast<i32>(local) / (dim * dim);
      const glm::vec3 cell_min(
          static_cast<f32>(static_cast<i32>(key.cx) * dim + lx) * kCoarseCellSize,
          static_cast<f32>(static_cast<i32>(key.cy) * dim + ly) * kCoarseCellSize,
          static_cast<f32>(static_cast<i32>(key.cz) * dim + lz) * kCoarseCellSize);
      pending.push_back(
          PendingCell{static_cast<i32>(slot), static_cast<i32>(local), cell_min});
    }
  }

  if (pending.empty()) {
    return 0;
  }

  // 3. Batched at kMaxSculptStampCellsPerCall entries per dispatch, same
  // "a whole-volume operation can easily exceed one dispatch's fixed
  // scratch capacity" reasoning restore_sculpt_checkpoint() already
  // documents for its own batching loop.
  const size_t batch_cap = kMaxSculptStampCellsPerCall;
  for (size_t start = 0; start < pending.size(); start += batch_cap) {
    const size_t count = std::min(batch_cap, pending.size() - start);

    std::vector<GpuSculptStampCell> gpu_cells(count);
    for (size_t i = 0; i < count; ++i) {
      const PendingCell &c = pending[start + i];
      gpu_cells[i].slot_cell = glm::ivec4(c.slot, c.local_cell_index, 0, 0);
      gpu_cells[i].cell_min_size = glm::vec4(c.cell_min, kCoarseCellSize);
    }
    chunk_sculpt_cell_buffer_->load_data(
        0, count * sizeof(GpuSculptStampCell), 0, gpu_cells.data());

    auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
        *context_, context_->device.graphics_command_pool);
    const u32 groups =
        (static_cast<u32>(count) + kSculptLocalSize - 1) / kSculptLocalSize;

    chunk_sculpt_crop_pipeline_->bind(*cmd);
    vkCmdBindDescriptorSets(cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                           chunk_sculpt_crop_pipeline_->layout(), 0, 1,
                           &chunk_sculpt_undo_restore_set_, 0, nullptr);
    SculptCropPushConstants crop_push{
        center.x,      center.y, center.z, params.x,
        params.y,      params.z, params.w, smoothness,
        static_cast<i32>(shape_type), static_cast<i32>(count),
        keep_outside ? 1 : 0};
    vkCmdPushConstants(cmd->handle(), chunk_sculpt_crop_pipeline_->layout(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(crop_push),
                      &crop_push);
    vkCmdDispatch(cmd->handle(), groups, 1, 1);

    // The splat-regen pass below reads chunk_indirection_buffer_/chunk_
    // brick_pool_buffer_, both just written above -- a cell the crop pass
    // just freed reads brick_index < 0 there and is silently skipped by
    // splat regen's own early-return, needing no special-casing here.
    VkBufferMemoryBarrier barriers[2]{};
    const VkBuffer written[2] = {chunk_indirection_buffer_->handle(),
                                 chunk_brick_pool_buffer_->handle()};
    for (u32 i = 0; i < 2; ++i) {
      barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[i].buffer = written[i];
      barriers[i].offset = 0;
      barriers[i].size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(cmd->handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2,
                        barriers, 0, nullptr);

    chunk_sculpt_splat_regen_pipeline_->bind(*cmd);
    vkCmdBindDescriptorSets(cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                           chunk_sculpt_splat_regen_pipeline_->layout(), 0, 1,
                           &chunk_sculpt_splat_regen_set_, 0, nullptr);
    const f32 voxel_size = kCoarseCellSize / static_cast<f32>(kChunkBrickDim);
    SculptSplatRegenPushConstants splat_push{static_cast<i32>(count),
                                             bake_stats_enabled_ ? 1 : 0,
                                             kCoarseCellSize, voxel_size};
    vkCmdPushConstants(cmd->handle(), chunk_sculpt_splat_regen_pipeline_->layout(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(splat_push),
                      &splat_push);
    vkCmdDispatch(cmd->handle(), groups, 1, 1);

    VulkanCommandBuffer::end_single_use(*context_,
                                        context_->device.graphics_command_pool,
                                        std::move(cmd),
                                        context_->device.graphics_queue);
  }

  // A crop can free/reallocate bricks any pending undo_stamp() entry
  // refers to -- clear it, same hazard restore_sculpt_checkpoint() already
  // documents for its own GPU-state rewrite. Crop/Cutout are NOT
  // integrated into the checkpoint/replay engine (see this method's own
  // header comment) -- this clears the PER-STAMP undo stack only; it does
  // not touch sculpt_checkpoints_ at all (those remain valid as "state at
  // an earlier edit index", since nothing about past STAMPS changed -- a
  // later delete_edit()/move_edit() replaying around a gap simply will not
  // reproduce this crop, a documented, accepted limitation -- see this
  // method's own header comment).
  undo_stack_.clear();
  invalidate_temporal_history();

  return static_cast<i32>(pending.size());
}

bool VulkanRaymarchShader::export_crop_outside(const std::string &path,
                                               glm::vec3 center,
                                               BrushShape shape_type,
                                               glm::vec4 params,
                                               f32 smoothness) {
  if (!valid_) {
    return false;
  }

  vkDeviceWaitIdle(context_->device.logical_device);

  // Steps 1-3 below duplicate capture_sculpt_checkpoint()'s own readback
  // mechanics verbatim rather than sharing a helper with it -- see this
  // method's own header comment (vulkan_raymarch_shader.h) for why: M4's
  // checkpoint/replay engine is already-complete, must-not-touch work, and
  // refactoring it to share a helper carries real regression risk for zero
  // behavioural benefit over simply duplicating ~30 lines of readback code.
  std::vector<CropOutsideBrickHeader> brick_headers;
  std::vector<std::vector<f32>> brick_voxels;

  if (!sculpt_chunk_slot_by_coord_.empty()) {
    const u64 level0_entries =
        static_cast<u64>(kMaxResidentChunks) * kChunkCellCount;
    const u64 level0_bytes = level0_entries * sizeof(i32);
    std::vector<i32> indirection(static_cast<size_t>(level0_entries));
    {
      VulkanBuffer staging(*context_, level0_bytes,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (!staging.is_valid()) {
        KERROR("export_crop_outside(): failed to allocate the indirection "
              "staging buffer -- export aborted.");
        return false;
      }
      chunk_indirection_buffer_->copy_to(staging, 0, 0, level0_bytes,
                                        context_->device.graphics_queue,
                                        context_->device.graphics_command_pool);
      if (void *mapped = staging.lock(0, level0_bytes, 0)) {
        std::memcpy(indirection.data(), mapped, level0_bytes);
        staging.unlock();
      }
    }

    struct PendingBrick {
      glm::ivec3 chunk_coord{0};
      i32 local_cell_index = 0;
      i32 brick_index = 0;
      glm::vec3 cell_min{0.0f};
    };
    std::vector<PendingBrick> pending;
    for (const auto &entry : sculpt_chunk_slot_by_coord_) {
      const ChunkKey &key = entry.first;
      const u32 slot = entry.second;
      const u64 base = static_cast<u64>(slot) * kChunkCellCount;
      for (u32 local = 0; local < kChunkCellCount; ++local) {
        const i32 brick_index = indirection[static_cast<size_t>(base + local)];
        if (brick_index < 0) {
          continue;
        }
        const i32 dim = static_cast<i32>(kChunkCoarseDim);
        const i32 lx = static_cast<i32>(local) % dim;
        const i32 ly = (static_cast<i32>(local) / dim) % dim;
        const i32 lz = static_cast<i32>(local) / (dim * dim);
        const glm::vec3 cell_min(
            static_cast<f32>(static_cast<i32>(key.cx) * dim + lx) * kCoarseCellSize,
            static_cast<f32>(static_cast<i32>(key.cy) * dim + ly) * kCoarseCellSize,
            static_cast<f32>(static_cast<i32>(key.cz) * dim + lz) * kCoarseCellSize);
        pending.push_back(PendingBrick{
            glm::ivec3(static_cast<i32>(key.cx), static_cast<i32>(key.cy),
                      static_cast<i32>(key.cz)),
            static_cast<i32>(local), brick_index, cell_min});
      }
    }

    if (!pending.empty()) {
      const u64 material_bytes = static_cast<u64>(pending.size()) * sizeof(i32);
      const u64 voxel_bytes = static_cast<u64>(pending.size()) *
          kChunkBrickVoxelCount * sizeof(f32);
      VulkanBuffer material_staging(*context_, material_bytes,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VulkanBuffer voxel_staging(*context_, voxel_bytes,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (!material_staging.is_valid() || !voxel_staging.is_valid()) {
        KERROR("export_crop_outside(): failed to allocate brick staging "
              "buffers for {} bricks -- export aborted.",
              pending.size());
        return false;
      }

      std::vector<VkBufferCopy> material_regions(pending.size());
      std::vector<VkBufferCopy> voxel_regions(pending.size());
      for (size_t i = 0; i < pending.size(); ++i) {
        const u64 brick_index = static_cast<u64>(pending[i].brick_index);
        material_regions[i].srcOffset = brick_index * sizeof(i32);
        material_regions[i].dstOffset = static_cast<u64>(i) * sizeof(i32);
        material_regions[i].size = sizeof(i32);
        voxel_regions[i].srcOffset =
            brick_index * kChunkBrickVoxelCount * sizeof(f32);
        voxel_regions[i].dstOffset =
            static_cast<u64>(i) * kChunkBrickVoxelCount * sizeof(f32);
        voxel_regions[i].size = kChunkBrickVoxelCount * sizeof(f32);
      }
      chunk_brick_primitive_buffer_->copy_regions_to(
          material_staging, material_regions, context_->device.graphics_queue,
          context_->device.graphics_command_pool);
      chunk_brick_pool_buffer_->copy_regions_to(
          voxel_staging, voxel_regions, context_->device.graphics_queue,
          context_->device.graphics_command_pool);

      std::vector<i32> materials(pending.size());
      std::vector<f32> voxels(pending.size() * kChunkBrickVoxelCount);
      if (void *mapped = material_staging.lock(0, material_bytes, 0)) {
        std::memcpy(materials.data(), mapped, material_bytes);
        material_staging.unlock();
      }
      if (void *mapped = voxel_staging.lock(0, voxel_bytes, 0)) {
        std::memcpy(voxels.data(), mapped, voxel_bytes);
        voxel_staging.unlock();
      }

      // 4. The CPU-only half: compute this crop shape's "outside" (removed)
      // complement for each readback brick -- smooth_subtraction(crop_sdf,
      // old_dist, k, h), the ORDINARY (not negated) subtraction identity,
      // since the FILE should hold whatever crop_volume(keep_outside=false)
      // is about to discard from the live volume (see this method's own
      // header comment). A brick whose complement has no voxel <= 0 left
      // (nothing survives outside the shape either) is omitted entirely --
      // same "no surface, nothing to write" reasoning Builtin.SculptCrop.
      // comp.glsl's own empty-brick-freeing uses GPU-side.
      for (size_t i = 0; i < pending.size(); ++i) {
        const PendingBrick &b = pending[i];
        std::vector<f32> out_voxels(kChunkBrickVoxelCount);
        bool any_inside = false;
        const f32 *src = voxels.data() + i * kChunkBrickVoxelCount;
        for (u32 pz = 0; pz < kChunkBrickApronDim; ++pz) {
          for (u32 py = 0; py < kChunkBrickApronDim; ++py) {
            for (u32 px = 0; px < kChunkBrickApronDim; ++px) {
              const glm::vec3 world_pos =
                  b.cell_min +
                  (glm::vec3(static_cast<f32>(px), static_cast<f32>(py),
                            static_cast<f32>(pz)) -
                   0.5f) *
                      (kCoarseCellSize / static_cast<f32>(kChunkBrickDim));
              const f32 crop_sdf = crop_cpu::eval_brush_shape(
                  world_pos - center, shape_type, params);
              const u32 voxel_index = px +
                  py * kChunkBrickApronDim +
                  pz * kChunkBrickApronDim * kChunkBrickApronDim;
              f32 unused_h = 0.0f;
              const f32 out_dist = crop_cpu::smooth_subtraction(
                  crop_sdf, src[voxel_index], smoothness, unused_h);
              out_voxels[voxel_index] = out_dist;
              if (out_dist <= 0.0f) {
                any_inside = true;
              }
            }
          }
        }
        if (!any_inside) {
          continue;
        }
        brick_headers.push_back(CropOutsideBrickHeader{
            b.chunk_coord.x, b.chunk_coord.y, b.chunk_coord.z,
            b.local_cell_index, materials[i]});
        brick_voxels.push_back(std::move(out_voxels));
      }
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    KERROR("export_crop_outside(): could not open '{}' for writing.", path);
    return false;
  }
  CropOutsideFileHeader header{kCropOutsideMagic, kCropOutsideVersion,
                               static_cast<u32>(brick_headers.size())};
  out.write(reinterpret_cast<const char *>(&header), sizeof(header));
  for (size_t i = 0; i < brick_headers.size(); ++i) {
    out.write(reinterpret_cast<const char *>(&brick_headers[i]),
             sizeof(CropOutsideBrickHeader));
    out.write(reinterpret_cast<const char *>(brick_voxels[i].data()),
             static_cast<std::streamsize>(brick_voxels[i].size() * sizeof(f32)));
  }
  if (!out) {
    KERROR("export_crop_outside(): write error while saving '{}'.", path);
    return false;
  }
  KINFO("export_crop_outside(): wrote {} brick(s) to '{}'.",
       brick_headers.size(), path);
  return true;
}

bool VulkanRaymarchShader::load_crop_outside_file(const std::string &path) {
  if (!valid_) {
    return false;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    KERROR("load_crop_outside_file(): could not open '{}' for reading.", path);
    return false;
  }
  CropOutsideFileHeader header{};
  in.read(reinterpret_cast<char *>(&header), sizeof(header));
  if (!in || header.magic != kCropOutsideMagic ||
      header.version != kCropOutsideVersion) {
    KERROR("load_crop_outside_file(): '{}' is not a valid crop-outside "
          "snapshot (bad magic/version).",
          path);
    return false;
  }

  struct LoadedBrick {
    CropOutsideBrickHeader header{};
    std::vector<f32> voxels;
  };
  std::vector<LoadedBrick> loaded;
  loaded.reserve(header.brick_count);
  for (u32 i = 0; i < header.brick_count; ++i) {
    LoadedBrick brick;
    in.read(reinterpret_cast<char *>(&brick.header), sizeof(CropOutsideBrickHeader));
    brick.voxels.resize(kChunkBrickVoxelCount);
    in.read(reinterpret_cast<char *>(brick.voxels.data()),
           static_cast<std::streamsize>(kChunkBrickVoxelCount * sizeof(f32)));
    if (!in) {
      KERROR("load_crop_outside_file(): '{}' is truncated (expected {} "
            "bricks, failed reading brick {}).",
            path, header.brick_count, i);
      return false;
    }
    loaded.push_back(std::move(brick));
  }

  if (loaded.empty()) {
    return true; // A valid, if trivial, "nothing was outside the shape" file.
  }

  vkDeviceWaitIdle(context_->device.logical_device);

  // Reuses restore_sculpt_checkpoint()'s own batched-upload-then-dispatch
  // mechanics (chunk_sculpt_checkpoint_restore_pipeline_ + chunk_sculpt_
  // splat_regen_pipeline_) rather than duplicating them -- see this
  // method's own header comment (vulkan_raymarch_shader.h) for why this is
  // safe to share (it is a pure "upload a captured brick verbatim" dispatch
  // with no dependency on the checkpoint-ring/undo-history machinery this
  // milestone must not touch).
  struct PendingCell {
    i32 slot = 0;
    i32 local_cell_index = 0;
    glm::vec3 cell_min{0.0f};
    i32 material = 0;
    const std::vector<f32> *voxels = nullptr;
  };
  std::vector<PendingCell> pending;
  std::vector<u32> new_slots;
  for (const LoadedBrick &brick : loaded) {
    const glm::ivec3 chunk_coord(brick.header.chunk_x, brick.header.chunk_y,
                                 brick.header.chunk_z);
    bool is_new = false;
    const i32 slot = acquire_sculpt_chunk_slot(chunk_coord, is_new);
    if (slot < 0) {
      KWARN("load_crop_outside_file(): ran out of level-0 chunk-table slots "
           "while loading '{}' -- some geometry will be missing.",
           path);
      continue;
    }
    if (is_new) {
      new_slots.push_back(static_cast<u32>(slot));
    }
    const i32 dim = static_cast<i32>(kChunkCoarseDim);
    const i32 lx = brick.header.local_cell_index % dim;
    const i32 ly = (brick.header.local_cell_index / dim) % dim;
    const i32 lz = brick.header.local_cell_index / (dim * dim);
    const glm::vec3 cell_min(
        static_cast<f32>(chunk_coord.x * dim + lx) * kCoarseCellSize,
        static_cast<f32>(chunk_coord.y * dim + ly) * kCoarseCellSize,
        static_cast<f32>(chunk_coord.z * dim + lz) * kCoarseCellSize);
    pending.push_back(PendingCell{slot, brick.header.local_cell_index, cell_min,
                                  brick.header.material, &brick.voxels});
  }

  if (pending.empty()) {
    return true;
  }

  if (!new_slots.empty()) {
    auto fill_cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
        *context_, context_->device.graphics_command_pool);
    for (u32 slot : new_slots) {
      const VkDeviceSize offset =
          static_cast<VkDeviceSize>(slot) * kChunkCellCount * sizeof(i32);
      const VkDeviceSize size = static_cast<VkDeviceSize>(kChunkCellCount) * sizeof(i32);
      vkCmdFillBuffer(fill_cmd->handle(), chunk_indirection_buffer_->handle(),
                      offset, size, 0xFFFFFFFFu);
    }
    VulkanCommandBuffer::end_single_use(*context_,
                                        context_->device.graphics_command_pool,
                                        std::move(fill_cmd),
                                        context_->device.graphics_queue);
  }

  const f32 voxel_size = kCoarseCellSize / static_cast<f32>(kChunkBrickDim);
  const size_t batch_cap = kMaxSculptUndoCellsPerStamp;
  for (size_t start = 0; start < pending.size(); start += batch_cap) {
    const size_t count = std::min(batch_cap, pending.size() - start);

    std::vector<GpuSculptStampCell> gpu_cells(count);
    std::vector<GpuUndoCaptureMeta> gpu_meta(count);
    std::vector<f32> voxel_upload(count * kChunkBrickVoxelCount);
    for (size_t i = 0; i < count; ++i) {
      const PendingCell &cell = pending[start + i];
      gpu_cells[i].slot_cell = glm::ivec4(cell.slot, cell.local_cell_index, 0, 0);
      gpu_cells[i].cell_min_size = glm::vec4(cell.cell_min, kCoarseCellSize);
      gpu_meta[i].change_type = 0; // Unused by the checkpoint-restore shader.
      gpu_meta[i].old_material = cell.material;
      // M3 (Looseness): the Cutout snapshot format (export_crop_outside())
      // does not capture looseness -- see this method's own known-
      // limitation note. Restoring 0.0 (uniform/clean) is the same
      // documented back-compat default every pre-Looseness brick reads --
      // not a regression specific to this path.
      gpu_meta[i].old_looseness = 0.0f;
      gpu_meta[i].pad1 = 0;
      std::memcpy(voxel_upload.data() + i * kChunkBrickVoxelCount,
                 cell.voxels->data(), kChunkBrickVoxelCount * sizeof(f32));
    }

    chunk_sculpt_cell_buffer_->load_data(
        0, count * sizeof(GpuSculptStampCell), 0, gpu_cells.data());
    chunk_sculpt_undo_capture_meta_buffer_->load_data(
        0, count * sizeof(GpuUndoCaptureMeta), 0, gpu_meta.data());
    chunk_sculpt_undo_capture_voxel_buffer_->load_data(
        0, voxel_upload.size() * sizeof(f32), 0, voxel_upload.data());

    auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
        *context_, context_->device.graphics_command_pool);
    const u32 groups =
        (static_cast<u32>(count) + kSculptLocalSize - 1) / kSculptLocalSize;

    chunk_sculpt_checkpoint_restore_pipeline_->bind(*cmd);
    vkCmdBindDescriptorSets(
        cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
        chunk_sculpt_checkpoint_restore_pipeline_->layout(), 0, 1,
        &chunk_sculpt_stamp_set_, 0, nullptr);
    SculptUndoRestorePushConstants restore_push{static_cast<i32>(count)};
    vkCmdPushConstants(cmd->handle(),
                      chunk_sculpt_checkpoint_restore_pipeline_->layout(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(restore_push),
                      &restore_push);
    vkCmdDispatch(cmd->handle(), groups, 1, 1);

    VkBufferMemoryBarrier barriers[2]{};
    const VkBuffer written[2] = {chunk_indirection_buffer_->handle(),
                                 chunk_brick_pool_buffer_->handle()};
    for (u32 i = 0; i < 2; ++i) {
      barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[i].buffer = written[i];
      barriers[i].offset = 0;
      barriers[i].size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(cmd->handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2,
                        barriers, 0, nullptr);

    chunk_sculpt_splat_regen_pipeline_->bind(*cmd);
    vkCmdBindDescriptorSets(cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                           chunk_sculpt_splat_regen_pipeline_->layout(), 0, 1,
                           &chunk_sculpt_splat_regen_set_, 0, nullptr);
    SculptSplatRegenPushConstants splat_push{static_cast<i32>(count),
                                             bake_stats_enabled_ ? 1 : 0,
                                             kCoarseCellSize, voxel_size};
    vkCmdPushConstants(cmd->handle(), chunk_sculpt_splat_regen_pipeline_->layout(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(splat_push),
                      &splat_push);
    vkCmdDispatch(cmd->handle(), groups, 1, 1);

    VulkanCommandBuffer::end_single_use(*context_,
                                        context_->device.graphics_command_pool,
                                        std::move(cmd),
                                        context_->device.graphics_queue);
  }

  undo_stack_.clear();
  invalidate_temporal_history();
  KINFO("load_crop_outside_file(): loaded {} brick(s) from '{}'.",
       pending.size(), path);
  return true;
}

// Generic debug probes against the chunked field -- dispatch Builtin.
// ChunkedFieldDebugQuery.comp.glsl at a single world point, with
// (query_clipmap_field()) or without (query_chunked_field()) the
// clipmap/camera-relative addressing. No primitive/streaming-manager
// dependency; kept for debug_probe_field() below (explicitly kept per the
// M0 CSG deletion pass's own instructions).
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
  push_constants.flags = (grid_visible_ ? kRenderFlagGrid : 0);
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
  // dispatch below is recorded can never disagree. (The "...and forced Off
  // while a primitive is being dragged" exception this comment used to
  // describe was CSG-primitive-specific and no longer applies -- see the
  // M0 CSG deletion pass, /home/h-i/.claude/plans/
  // joyful-skipping-giraffe.md.)
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
    splat_push.only_level = splat_only_level_;

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
  // M7 polish (TAA-ghosting fix): preview_overlay_image_ -- discard-and-
  // write, unconditionally, every frame, same reasoning as ao_image_'s own
  // unconditional transition above: deferred_shade_pipeline_ writes every
  // pixel of it regardless of whether the preview is actually visible this
  // frame (see Builtin.DeferredShade.comp.glsl's own unconditional
  // imageStore into it), so it must leave this dispatch in GENERAL layout
  // every frame, not just the frames a preview happens to be showing.
  transition_image(cmd, preview_overlay_image_.handle,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  deferred_shade_pipeline_->bind(command_buffer);
  // M3c: set 0 = render_set_ (unchanged), set 1 = paint_read_set_ (NEW --
  // see deferred_shade_pipeline_'s own construction comment). render_
  // pipeline_'s own bind call just above (the visibility pass) is
  // deliberately untouched -- its pipeline layout is still single-set.
  {
    const VkDescriptorSet deferred_shade_sets[2] = {render_set_,
                                                    paint_read_set_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                           deferred_shade_pipeline_->layout(), 0, 2,
                           deferred_shade_sets, 0, nullptr);
  }
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
  // M7 polish (TAA-ghosting fix): preview_overlay_image_'s write (deferred_
  // shade_pipeline_, above) -> this pass's read must be visible before
  // post_composite_pipeline_'s dispatch below touches it. Both compute,
  // same queue, so a layout-preserving access/stage barrier is enough --
  // same pattern as depth_image_'s WRITE->READ barrier before the AO pass.
  transition_image(cmd, preview_overlay_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
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
  // The disk brick cache's hit/store/too-dense report that used to print
  // here was removed along with that cache (M0 CSG deletion pass).
  if (candidate_chunks_total_ > 0) {
    KINFO("  candidate lists      {}/{} chunks fell back to whole-scene",
         candidate_chunks_fallback_, candidate_chunks_total_);
  }
  // The per-level Ready/Baking/Evicting residency snapshot that used to
  // print here read ChunkStreamingManager::resident_chunks() -- removed
  // along with that class (M0 CSG deletion pass, the camera-relative
  // streaming core; see /home/h-i/.claude/plans/
  // joyful-skipping-giraffe.md).
  // Pool occupancy, at its worst point in this interval (see record_pool_
  // gauges()). Both pools are shared across every level, and both fail
  // SILENTLY -- a dry brick pool leaves cells with no geometry, a dry
  // cluster pool leaves bricks with no splat points. Either reads as
  // "broken chunks" on screen. Percentages are of the pool, so "100% used"
  // is the number to look for.
  if (chunk_bricks_free_ != 0xFFFFFFFFu) {
    const u32 bricks_used = kMaxChunkBricks - std::min(chunk_bricks_free_,
                                                       kMaxChunkBricks);
    const u32 clusters_used =
        kMaxChunkClusters - std::min(chunk_clusters_free_, kMaxChunkClusters);
    KINFO("  pool peak use        bricks {}/{} ({:.0f}%)  clusters {}/{} "
         "({:.0f}%)",
         bricks_used, kMaxChunkBricks,
         100.0 * static_cast<f64>(bricks_used) / kMaxChunkBricks,
         clusters_used, kMaxChunkClusters,
         100.0 * static_cast<f64>(clusters_used) / kMaxChunkClusters);
    // The cliff that actually matters is not zero -- it is the reserve.
    // Once free pages fall to CLUSTER_FIRST_PAGE_RESERVE, every brick baked
    // from then on is capped at one page (64 points for a 16^3 brick
    // instead of 512), which reads as sparse, streaky surfaces.
    const u32 rollbacks = splat_rollbacks_total_ - splat_rollbacks_reported_;
    splat_rollbacks_reported_ = splat_rollbacks_total_;
    const u32 no_points = splat_no_points_total_ - splat_no_points_reported_;
    splat_no_points_reported_ = splat_no_points_total_;
    if (rollbacks > 0) {
      KINFO("  splat LOD rollbacks  {} coarser, {} WITH NO POINTS AT ALL "
           "(cluster-pool pressure)",
           rollbacks - std::min(no_points, rollbacks), no_points);
    }
    if (no_points > 0) {
      KWARN("{} bricks baked with NO splat points -- the cluster pool ran "
            "genuinely dry, so those surfaces are missing from the splat "
            "pass entirely (not just coarser). Raise kMaxChunkClusters, or "
            "lower kChunkMaxClustersPerBrick to halve per-brick demand.",
            no_points);
    }
    if (chunk_bricks_free_ == 0 || chunk_clusters_free_ == 0) {
      KWARN("Chunked-field pool EXHAUSTED this interval ({} bricks / {} "
            "cluster pages free at the low point) -- cells and splat points "
            "were dropped, which renders as missing or corrupted chunks. "
            "Raise kMaxChunkBricksPerLevel / kMaxChunkClusters (and "
            "MAX_BRICKS / MAX_CLUSTERS in the shaders), or hold fewer chunks "
            "resident (ChunkStreamingManager's lazy_evict_min_free).",
            chunk_bricks_free_, chunk_clusters_free_);
    }
  }
  chunk_bricks_free_ = 0xFFFFFFFFu;
  chunk_clusters_free_ = 0xFFFFFFFFu;
  // Brick deduplication -- how much of the baked content was a copy of
  // something already baked rather than freshly evaluated.
  if (alias_cells_total_ > 0) {
    KINFO("  brick dedup          {:.1f}% of cells copied ({} of {}), {} from"
         " an earlier bake",
         100.0 * static_cast<f64>(alias_cells_aliased_) /
             static_cast<f64>(alias_cells_total_),
         alias_cells_aliased_, alias_cells_total_, alias_cells_cached_);
    // When nothing aliases, the useful question is which precondition
    // failed -- otherwise "0%" is indistinguishable between "this scene has
    // no periodicity to exploit" and "the key is broken".
    if (alias_cells_aliased_ == 0) {
      KINFO("    dedup declined: {} chunks had no periodic candidate, {} had"
           " no cell step matching a repetition period at this level",
           alias_bail_no_periodic_, alias_bail_level_);
    }
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
  alias_bail_no_periodic_ = 0;
  alias_bail_level_ = 0;
}
