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

void main() {
    out_colour = instance_ubo.diffuse_colour * texture(diffuse_sampler, in_texcoord);
}
