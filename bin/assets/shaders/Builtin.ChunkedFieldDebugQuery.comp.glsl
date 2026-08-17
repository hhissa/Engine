#version 450
#extension GL_GOOGLE_include_directive : require

// Phase-3a verification aid ONLY -- not part of the render pipeline. Runs a
// single sample_chunked_field() query at a CPU-supplied world position and
// writes the result back to a host-visible buffer, so
// VulkanRaymarchShader's (temporary) verification path can compare it
// against an independently, analytically hand-computed expected distance
// without needing a live visual render (unreliable to eyeball in this
// engine's target environments -- see the comment on why this exists in
// vulkan_raymarch_shader.cpp). Every buffer/constant it touches is the same
// chunked-field state Builtin.ChunkVoxelize.comp.glsl bakes and Builtin.
// ChunkedFieldCommon.inc.glsl reads -- this shader adds no new state of its
// own, it's purely a query harness.
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

#include "Builtin.SdfFieldConfig.inc.glsl"

const float MAX_DIST = 128.0;
const float SURF_DIST = 0.001;

#define CHUNKED_FIELD_TABLE_BINDING 0
#define CHUNKED_FIELD_INDIRECTION_BINDING 1
#define CHUNKED_FIELD_BRICKPOOL_BINDING 2
#define CHUNKED_FIELD_BRICKPRIMITIVE_BINDING 3
#include "Builtin.ChunkedFieldCommon.inc.glsl"

layout(binding = 4) buffer DebugOutputBuffer {
    float out_dist;
    float out_skip_dist;
    int out_material_index;
};

layout(push_constant) uniform PushConstants {
    float query_x;
    float query_y;
    float query_z;
    // Phase 4: when non-zero, calls sample_clipmap_field() (multi-level)
    // instead of sample_chunked_field() (level-0 only) -- camera_x/y/z are
    // only meaningful in that mode.
    int use_clipmap;
    float camera_x;
    float camera_y;
    float camera_z;
} push;

void main() {
    vec3 p = vec3(push.query_x, push.query_y, push.query_z);
    float dist;
    float skip_dist;
    int material_index;
    if (push.use_clipmap != 0) {
        vec3 camera_pos = vec3(push.camera_x, push.camera_y, push.camera_z);
        sample_clipmap_field(p, vec3(0.0, 0.0, 1.0), camera_pos, dist, skip_dist,
                             material_index);
    } else {
        sample_chunked_field(p, vec3(0.0, 0.0, 1.0), dist, skip_dist, material_index);
    }
    out_dist = dist;
    out_skip_dist = skip_dist;
    out_material_index = material_index;
}
