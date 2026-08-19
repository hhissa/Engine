#pragma once

#include "../defines.h"

typedef enum renderer_backend_type {
  RENDERER_BACKEND_TYPE_VULKAN,
  RENDERER_BACKEND_TYPE_OPENGL,
  RENDERER_BACKEND_TYPE_DIRECTX
} renderer_backend_type;

struct render_packet {
  f32 delta_time;
};

// Identifies one renderer_load_scene() call -- every primitive that call
// registered is tracked under this handle, so renderer_remove_scene() can
// unload exactly that scene's primitives without touching any other
// concurrently loaded scene. 0 (kInvalidSceneHandle) is never a valid
// handle -- renderer_load_scene() returns it on failure (missing/malformed
// file).
using SceneHandle = u32;
constexpr SceneHandle kInvalidSceneHandle = 0;

// How the renderer uses the chunked field's baked surface-point cloud for
// primary visibility -- the Dreams-style point-splatting technique (see
// Builtin.ChunkPointSplat.comp.glsl's header comment). Lives here, in the
// shared types header, so the frontend API and the backend interface can
// both name it without either including the Vulkan-specific headers;
// VulkanRaymarchShader::SplatMode mirrors these values exactly (a
// static_assert in vulkan_backend.cpp pins them together).
//
// Only meaningful while the chunked field is enabled (see
// renderer_set_chunked_field_enabled()) -- the fixed-cube field bakes no
// points, so nothing splats there regardless.
enum class RendererSplatMode : i32 {
  // No splat prepass; every primary ray marches from the camera, exactly
  // as this renderer always did.
  Off = 0,
  // Splat, then start each primary ray just short of its pixel's nearest
  // splat. Same image as Off (the starts are conservative and uncovered
  // pixels start at 0), less empty space marched. The default.
  Prime = 1,
  // Shade the winning splat directly -- no primary march for a covered
  // pixel. Pixels no splat covers fall back to Prime, so the image stays
  // complete with no temporal accumulation needed.
  Visibility = 2,
};
