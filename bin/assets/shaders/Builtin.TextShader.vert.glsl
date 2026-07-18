#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_texcoord;

// Set once per frame by VulkanTextShader::render_to().
layout(set = 0, binding = 0) uniform GlobalUniformObject {
    mat4 projection;
    mat4 view;
} global_ubo;

layout(location = 0) out vec2 out_texcoord;

void main() {
    out_texcoord = in_texcoord;
    // Vertex positions are already absolute screen-space pixels (baked by
    // BitmapFont::layout() every render_to() call), so there's no per-quad
    // model transform here the way VulkanUIShader has one -- projection *
    // view alone maps them to clip space.
    gl_Position = global_ubo.projection * global_ubo.view *
                 vec4(in_position, 0.0, 1.0);
}
