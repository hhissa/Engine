#version 450

layout(location = 0) in vec2 in_texcoord;
layout(location = 0) out vec4 out_colour;

// Per-material instance data (set 1) -- written once when the material
// acquires its shader instance resources, refreshed every frame via
// MaterialSystem::apply_instance().
layout(set = 1, binding = 0) uniform InstanceUniformObject {
    vec4 diffuse_colour;
} instance_ubo;
layout(set = 1, binding = 1) uniform sampler2D diffuse_sampler;

// Must match the vertex shader's block. tint multiplies this one draw
// (e.g. alpha to fade an image in and out).
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 tint;
} push;

void main() {
    out_colour = push.tint * instance_ubo.diffuse_colour *
                 texture(diffuse_sampler, in_texcoord);
}
