#version 450

// Reuses VulkanUIShader's exact unit-quad vertex layout (4 vertices, local
// [0,1]^2, indices 0,1,2,2,3,0), mapped directly onto an axis-aligned
// screen rect via push.position/size -- simpler than
// Builtin.LineShader.vert.glsl's reinterpretation of the same quad into a
// rotated line segment, since a solid box has no orientation to account
// for.
layout(location = 0) in vec2 in_position;

// Set once per frame by VulkanSolidQuadShader::render_to() -- same
// screen-pixel orthographic projection convention as
// Builtin.UIShader.vert.glsl/Builtin.LineShader.vert.glsl.
layout(set = 0, binding = 0) uniform GlobalUniformObject {
    mat4 projection;
    mat4 view;
} global_ubo;

// Per-quad state (screen pixels + colour) -- no material/texture
// involved, like Builtin.LineShader.vert.glsl: this shader only ever
// draws solid-colour rectangles (e.g. SH's face/crotch censor boxes).
layout(push_constant) uniform PushConstants {
    vec2 position; // top-left corner, screen pixels
    vec2 size;      // width/height, screen pixels
    vec4 colour;
} push;

layout(location = 0) out vec4 out_colour;

void main() {
    vec2 pos = push.position + in_position * push.size;
    out_colour = push.colour;
    gl_Position = global_ubo.projection * global_ubo.view * vec4(pos, 0.0, 1.0);
}
