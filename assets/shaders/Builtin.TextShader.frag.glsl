#version 450

layout(location = 0) in vec2 in_texcoord;
layout(location = 0) out vec4 out_colour;

// The font atlas texture is the only instance-scope uniform this shader
// declares (no instance UBO -- there's no per-material colour here, the
// text's colour is fully determined by the push constant below), so it
// takes binding 0 within set 1 rather than binding 1 the way
// Builtin.UIShader.frag.glsl's sampler does (which follows its instance
// UBO at binding 0).
layout(set = 1, binding = 0) uniform sampler2D atlas_sampler;

layout(push_constant) uniform PushConstants {
    vec4 colour;
} push;

void main() {
    // The atlas stores glyph coverage in alpha only (RGB is always white --
    // see BitmapFont), so the text's actual colour comes entirely from the
    // push constant, modulated by that coverage.
    out_colour = vec4(push.colour.rgb, push.colour.a * texture(atlas_sampler, in_texcoord).a);
}
