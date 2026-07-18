#version 450

// Reuses VulkanUIShader's exact unit-quad vertex layout (4 vertices,
// local [0,1]^2, indices 0,1,2,2,3,0) but reinterprets it here: x selects
// which endpoint (0=start, 1=end), y selects which side of the line
// (0=one edge, 1=the other) -- so the same static quad geometry draws an
// arbitrary-angle, fixed-thickness line segment instead of an
// axis-aligned rectangle.
layout(location = 0) in vec2 in_position;

// Set once per frame by VulkanLineShader::render_to() -- same screen-pixel
// orthographic projection convention as Builtin.UIShader.vert.glsl.
layout(set = 0, binding = 0) uniform GlobalUniformObject {
    mat4 projection;
    mat4 view;
} global_ubo;

// Per-line-segment state (screen pixels + colour) -- no material/texture
// involved at all, unlike VulkanUIShader; this shader only ever draws
// solid-colour lines (the SDF editor's move-gizmo axes).
layout(push_constant) uniform PushConstants {
    vec2 start;
    vec2 end;
    vec4 colour;
} push;

layout(location = 0) out vec4 out_colour;

const float THICKNESS = 3.0;

void main() {
    vec2 dir = push.end - push.start;
    float len = length(dir);
    vec2 dir_n = len > 0.0001 ? dir / len : vec2(1.0, 0.0);
    vec2 normal = vec2(-dir_n.y, dir_n.x) * (THICKNESS * 0.5);

    vec2 base = mix(push.start, push.end, in_position.x);
    vec2 offset = mix(-normal, normal, in_position.y);
    vec2 pos = base + offset;

    out_colour = push.colour;
    gl_Position = global_ubo.projection * global_ubo.view * vec4(pos, 0.0, 1.0);
}
