#version 450

// First of the two-pass separable bloom blur: extracts the bright-pass
// (only the part of each pixel's colour above a threshold -- bloom is
// "light spilling from bright things," not a blur of the whole image) and
// blurs it horizontally, downsampling to half resolution at the same
// time. Builtin.BloomBlurV.comp.glsl finishes the blur vertically (and
// composites the result back on top of the full-resolution image, along
// with vignette and pixelation) -- splitting a wide 2D blur into two 1D
// passes like this is the standard way to make a large-radius blur
// affordable: an NxN kernel done separably costs O(2N) samples per pixel
// instead of O(N^2).
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 0, rgba8) uniform readonly image2D scene_colour;
layout(binding = 1, rgba8) uniform writeonly image2D bloom_temp;

layout(push_constant) uniform PushConstants {
    int full_width;
    int full_height;
    float bloom_threshold; // luminance above which a pixel starts blooming
} push;

float luminance(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// Soft-knee bright-pass: pixels at or below the threshold contribute
// nothing, pixels above it contribute the excess over the threshold --
// so bloom reads as "glow spilling from the brightest things" (emissive
// surfaces, strongly lit highlights) rather than a haze over the whole
// image.
vec3 bright_pass(vec3 colour) {
    float l = luminance(colour);
    float excess = max(l - push.bloom_threshold, 0.0);
    // Scale the whole colour by how much luminance was over the
    // threshold relative to the total, preserving hue/tint instead of
    // just returning a flat excess-luminance grey.
    return colour * (excess / max(l, 0.0001));
}

// 9-tap Gaussian-ish weights (unnormalized -- divided by their sum below),
// sampled 2 full-resolution pixels apart per tap: at half-resolution
// output this reaches +-16 full-res pixels either side of center, a wide
// enough radius for bloom to read as "glow" rather than a tight blur.
const int TAP_COUNT = 9;
const float TAP_WEIGHTS[TAP_COUNT] = float[](0.05, 0.09, 0.12, 0.15, 0.18, 0.15, 0.12, 0.09, 0.05);
const float TAP_STEP = 2.0; // full-resolution pixels between taps

void main() {
    ivec2 half_coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 half_size = ivec2((push.full_width + 1) / 2, (push.full_height + 1) / 2);
    if (half_coord.x >= half_size.x || half_coord.y >= half_size.y) {
        return;
    }

    ivec2 full_center = half_coord * 2;
    vec3 sum = vec3(0.0);
    float weight_sum = 0.0;
    for (int i = 0; i < TAP_COUNT; ++i) {
        float offset = (float(i) - float(TAP_COUNT / 2)) * TAP_STEP;
        ivec2 sample_coord = ivec2(full_center.x + int(offset), full_center.y);
        sample_coord = clamp(sample_coord, ivec2(0), ivec2(push.full_width - 1, push.full_height - 1));
        vec3 colour = imageLoad(scene_colour, sample_coord).rgb;
        sum += bright_pass(colour) * TAP_WEIGHTS[i];
        weight_sum += TAP_WEIGHTS[i];
    }

    imageStore(bloom_temp, half_coord, vec4(sum / weight_sum, 1.0));
}
