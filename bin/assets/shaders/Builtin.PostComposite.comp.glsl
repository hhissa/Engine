#version 450

// Second half of the post-process chain: finishes the bloom blur
// vertically (see Builtin.BloomBlurH.comp.glsl for the horizontal half and
// why it's split in two), then composites bloom + vignette + pixelation
// together and writes the final frame. One pass rather than three
// separate ones for vignette/pixelation since none of them need each
// other's *output* -- only pixelation needs its own extra image reads
// (the block's representative pixel), so folding all three into the pass
// that's already doing the bloom's second blur direction costs nothing
// extra in dispatches.
//
// Ordering matters here: pixelation picks *which pixel* of scene_colour to
// treat as this fragment's base colour (its own, if exempt; its block's
// representative pixel otherwise) BEFORE bloom is added on top and
// vignette darkens the result -- so a pixelated block's flat colour still
// blooms and vignettes normally, rather than pixelation flattening an
// already-bloomed/vignetted image into blocky bands.
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// rgb = this pixel's shaded colour; a = 1.0 if the primitive hit here is
// pixelation-exempt (Material::pixelation_exempt), 0.0 otherwise (or for
// the background/a miss) -- see the render pass's final imageStore().
layout(binding = 0, rgba8) uniform readonly image2D scene_colour;
// Half-resolution, horizontally-pre-blurred bright-pass from
// Builtin.BloomBlurH.comp.glsl -- this pass finishes blurring it
// vertically before adding it on top of scene_colour.
layout(binding = 1, rgba8) uniform readonly image2D bloom_temp;
layout(binding = 2, rgba8) uniform writeonly image2D post_process_out;

layout(push_constant) uniform PushConstants {
    int full_width;
    int full_height;
    float bloom_intensity;      // how strongly the blurred bright-pass adds back in
    float vignette_strength;    // 0 = no vignette, 1 = fully black at the corners
    float vignette_radius;      // normalized distance from center where falloff starts
    int pixelation_enabled;     // nonzero to quantize non-exempt pixels into blocks
    int pixelation_block_size;  // block edge length, in full-resolution pixels
} push;

// Same tap pattern as Builtin.BloomBlurH.comp.glsl, now stepping through
// bloom_temp's own (half-resolution) pixels vertically -- one half-res
// pixel of step here already covers 2 full-res pixels' worth of distance,
// matching the horizontal pass's radius.
const int TAP_COUNT = 9;
const float TAP_WEIGHTS[TAP_COUNT] = float[](0.05, 0.09, 0.12, 0.15, 0.18, 0.15, 0.12, 0.09, 0.05);
const float TAP_STEP = 1.0; // half-resolution pixels between taps

vec3 sample_bloom(ivec2 full_coord, ivec2 half_size) {
    ivec2 half_coord = full_coord / 2;
    vec3 sum = vec3(0.0);
    float weight_sum = 0.0;
    for (int i = 0; i < TAP_COUNT; ++i) {
        float offset = (float(i) - float(TAP_COUNT / 2)) * TAP_STEP;
        ivec2 sample_coord = ivec2(half_coord.x, half_coord.y + int(offset));
        sample_coord = clamp(sample_coord, ivec2(0), half_size - ivec2(1));
        sum += imageLoad(bloom_temp, sample_coord).rgb * TAP_WEIGHTS[i];
        weight_sum += TAP_WEIGHTS[i];
    }
    return sum / weight_sum;
}

void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= push.full_width || coord.y >= push.full_height) {
        return;
    }

    vec4 own_pixel = imageLoad(scene_colour, coord);
    bool exempt = own_pixel.a > 0.5;

    // Pixelation: a non-exempt pixel borrows its *block's* representative
    // pixel (the block's top-left-most sample -- a plain nearest-sample
    // mosaic, the classic "pixelation" look) instead of its own, flattening
    // every pixel in that block to one shared colour. An exempt pixel
    // always uses its own colour regardless of pixelation_enabled, so a
    // marked primitive stays crisp even while its surroundings pixelate.
    vec3 base_colour = own_pixel.rgb;
    if (push.pixelation_enabled != 0 && !exempt) {
        int block = max(push.pixelation_block_size, 1);
        ivec2 block_origin = (coord / block) * block;
        base_colour = imageLoad(scene_colour, block_origin).rgb;
    }

    ivec2 half_size = ivec2((push.full_width + 1) / 2, (push.full_height + 1) / 2);
    vec3 bloom = sample_bloom(coord, half_size);
    vec3 colour = base_colour + bloom * push.bloom_intensity;

    // Vignette: smooth radial darkening from screen center, in
    // aspect-corrected normalized coordinates (uv.x scaled by aspect ratio
    // so the falloff is circular, not stretched into an ellipse on a
    // widescreen framebuffer).
    vec2 uv = (vec2(coord) + 0.5) / vec2(push.full_width, push.full_height) * 2.0 - 1.0;
    uv.x *= float(push.full_width) / float(push.full_height);
    float dist = length(uv);
    float vignette = 1.0 - push.vignette_strength *
                          smoothstep(push.vignette_radius, 1.4142, dist);
    colour *= vignette;

    imageStore(post_process_out, coord, vec4(colour, 1.0));
}
