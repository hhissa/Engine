#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_texcoord;

// Set once per frame by VulkanUIShader::render_to() -- an orthographic
// projection (screen pixels -> clip space) and an identity view (there's
// no camera for 2D UI).
layout(set = 0, binding = 0) uniform GlobalUniformObject {
    mat4 projection;
    mat4 view;
} global_ubo;

// Per-quad transform (this demo draws one quad, but the pipeline supports
// as many draws as needed, each with its own push constants). The
// material's diffuse tint now lives in the instance UBO (set 1, binding 0
// -- see the fragment shader) instead of here, since it's per-material
// rather than per-draw.
layout(push_constant) uniform PushConstants {
    mat4 model; // 64 bytes.
} push;

layout(location = 0) out vec2 out_texcoord;

void main() {
    out_texcoord = in_texcoord;
    gl_Position = global_ubo.projection * global_ubo.view * push.model *
                 vec4(in_position, 0.0, 1.0);
}
