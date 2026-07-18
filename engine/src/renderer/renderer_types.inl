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
