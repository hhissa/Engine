#version 450

// Temporal anti-aliasing resolve -- the first of the six "bridge the gap to
// Dreams" steps (see the renderer study). Dreams' whole renderer is built on
// the assumption that a history buffer will clean up after it: stochastic
// alpha instead of sorted transparency, deliberately undersampled shadow
// maps, one ambient-occlusion ray per pixel, depth of field done by
// jittering splats in a screen-space disc. None of that is affordable in a
// single frame. This pass is what makes those techniques payable.
//
// The scheme is the standard one: pass 3 jitters its ray origins by a
// sub-pixel offset that changes every frame (a Halton (2,3) sequence, see
// kTaaJitterCount engine-side), so over N frames the same pixel is sampled
// at N different points inside its own footprint. This pass reprojects the
// PREVIOUS resolved frame into the current view, clamps it to the range of
// colours actually present in this frame's 3x3 neighbourhood (which is what
// rejects history that has been occluded, disoccluded or lit differently),
// and blends a small fraction of the new sample in.
//
// REPROJECTION WITHOUT VELOCITY. The scene is static geometry viewed by a
// moving camera, so no per-pixel motion vectors are needed: a pixel's world
// position is recovered from pass 3's depth (distance along that pixel's own
// ray, see out_depth there), and re-projected through the previous frame's
// camera basis with the exact inverse of the mapping pass 3 builds rays with
// (uv = (pixel - 0.5*size) / size.y). When dynamic geometry arrives, THIS is
// the function that needs a velocity buffer -- nothing else here changes.
//
// The history and the resolve target are separate images, ping-ponged
// engine-side, because a pixel reads history at its REPROJECTED position:
// reading and writing one image would let an invocation read a texel another
// invocation had already overwritten this frame.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// This frame's freshly rendered (jittered) colour. Alpha is the pixelation-
// exempt flag pass 3 writes, not opacity -- it is passed through untouched,
// never blended, since it is a per-pixel boolean about the hit primitive
// rather than a colour (see Builtin.PostComposite.comp.glsl).
layout(binding = 0, rgba8) uniform readonly image2D current_colour;

// Distance along this pixel's ray to whatever it hit, or negative for a
// background pixel (nothing hit) -- see pass 3's out_depth store.
layout(binding = 1, r32f) uniform readonly image2D current_depth;

// Previous frame's resolved output, and this frame's. rgba16f rather than
// the 8-bit output format: at a 0.1 blend factor an 8-bit history quantizes
// small differences to zero and pixels visibly stick. Alpha carries the
// depth (distance along the pixel's ray, negative for background) the
// resolve ran with, so next frame can test whether reprojected history
// belongs to the same surface at all -- see the depth rejection below.
layout(binding = 2, rgba16f) uniform readonly image2D history_in;
layout(binding = 3, rgba16f) uniform writeonly image2D history_out;

// What the post-process chain reads (pass 4a/4b) -- same format as pass 3's
// own output so nothing downstream changes.
layout(binding = 4, rgba8) uniform writeonly image2D resolved_colour;

layout(push_constant) uniform PushConstants {
    vec4 camera_position; // xyz + pad -- THIS frame's basis, matching the
    vec4 camera_forward;  // basis pass 3 rendered with.
    vec4 camera_right;
    vec4 camera_up;
    vec4 prev_camera_position; // ...and last frame's, for the reprojection.
    vec4 prev_camera_forward;
    vec4 prev_camera_right;
    vec4 prev_camera_up;
    vec2 jitter;      // Sub-pixel offset pass 3 added to pixel coordinates
                     // this frame; needed to rebuild the exact ray each
                     // pixel was traced along.
    float blend;      // Weight of the NEW sample. 1.0 disables temporal
                     // accumulation entirely (a straight copy), which is
                     // what the engine passes when TAA is off or when the
                     // history has been invalidated -- a resize, a floating-
                     // origin recenter, a full rebake.
    int pad;
} push;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(current_colour);
    if (pixel.x >= size.x || pixel.y >= size.y) {
        return;
    }

    vec4 current = imageLoad(current_colour, pixel);
    float depth = imageLoad(current_depth, pixel).r;

    if (push.blend >= 1.0) {
        imageStore(history_out, pixel, vec4(current.rgb, depth));
        imageStore(resolved_colour, pixel, current);
        return;
    }

    // Rebuild this pixel's ray exactly as pass 3 did, jitter included.
    vec2 uv = (vec2(pixel) + push.jitter - 0.5 * vec2(size)) / float(size.y);
    vec3 ray_dir = normalize(uv.x * push.camera_right.xyz +
                             uv.y * push.camera_up.xyz +
                             push.camera_forward.xyz);

    // Where that ray's hit sits relative to the previous camera. A
    // background pixel (depth < 0) has no position to reproject, but its
    // colour is a pure function of direction, so the direction alone
    // reprojects it correctly -- which is what keeps a panning sky stable
    // instead of smearing.
    vec3 prev_view;
    if (depth < 0.0) {
        prev_view = ray_dir;
    } else {
        vec3 world = push.camera_position.xyz + ray_dir * depth;
        prev_view = world - push.prev_camera_position.xyz;
    }

    float prev_z = dot(prev_view, push.prev_camera_forward.xyz);
    vec4 out_colour = current;
    if (prev_z > 1e-4) {
        vec2 prev_uv = vec2(dot(prev_view, push.prev_camera_right.xyz),
                            dot(prev_view, push.prev_camera_up.xyz)) / prev_z;
        // Inverse of the uv construction above, WITHOUT jitter: the history
        // holds resolved pixels at their centres, not at last frame's
        // sample offsets.
        vec2 prev_pixel = prev_uv * float(size.y) + 0.5 * vec2(size);

        // Bilinear fetch by hand -- these are storage images, so there is no
        // sampler to do it. Nearest-neighbour history is what makes TAA look
        // like it is dragging a stipple pattern around.
        vec2 base = floor(prev_pixel - 0.5);
        vec2 frac = prev_pixel - 0.5 - base;
        ivec2 p00 = ivec2(base);
        if (p00.x >= 0 && p00.y >= 0 && p00.x + 1 < size.x && p00.y + 1 < size.y) {
            vec4 h00 = imageLoad(history_in, p00);
            vec4 h10 = imageLoad(history_in, p00 + ivec2(1, 0));
            vec4 h01 = imageLoad(history_in, p00 + ivec2(0, 1));
            vec4 h11 = imageLoad(history_in, p00 + ivec2(1, 1));
            vec4 history = mix(mix(h00, h10, frac.x), mix(h01, h11, frac.x),
                               frac.y);

            // DEPTH REJECTION. The colour clamp below cannot tell stale
            // history from plausible history when the current frame is
            // noisy (one AO ray and four dithered shadow taps per pixel
            // make the local colour range wide), which is exactly what let
            // an occluder's colour survive as a trail across the surface it
            // used to cover. Depth can tell: history's alpha holds how far
            // that pixel's surface was from the camera that resolved it,
            // and this pixel's hit reprojected into that same camera gives
            // the distance it SHOULD hold if it is the same surface. A
            // mismatch means the history belongs to something else (the
            // primitive that was in front, most commonly), so the blend is
            // boosted to flush it in a frame or two instead of nursing it
            // along at the accumulation rate. Background pixels (depth < 0)
            // agree by both being background; bilinear mixing across a
            // silhouette lands between the two surfaces and is rejected by
            // either test, which is the right answer for a pixel whose
            // history straddles an edge.
            float expected = length(prev_view); // == |ray|*t; 1 for background
            bool same_surface = depth < 0.0
                ? history.a < 0.0
                : abs(history.a - expected) <= max(0.1 * expected, 0.05);
            float blend = same_surface ? push.blend : max(push.blend, 0.75);

            // Neighbourhood clamp, tightened by variance. The 3x3 colour
            // box around this pixel is the set of colours the current frame
            // says are plausible here, but with stochastic shading the raw
            // min/max box is inflated by whichever neighbour drew the most
            // extreme sample this frame. Clamping to mean +/- gamma*sigma
            // as well (standard variance clipping) keeps the box sized to
            // where the colours actually cluster, which both rejects stale
            // history sooner and stops single outlier samples from dragging
            // the accumulated result around -- less ghosting AND less grain
            // from the same change.
            vec3 lo = current.rgb;
            vec3 hi = current.rgb;
            vec3 m1 = vec3(0.0);
            vec3 m2 = vec3(0.0);
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    ivec2 q = clamp(pixel + ivec2(dx, dy), ivec2(0),
                                    size - ivec2(1));
                    vec3 c = imageLoad(current_colour, q).rgb;
                    lo = min(lo, c);
                    hi = max(hi, c);
                    m1 += c;
                    m2 += c * c;
                }
            }
            const float VARIANCE_GAMMA = 1.25;
            vec3 mu = m1 / 9.0;
            vec3 sigma = sqrt(max(m2 / 9.0 - mu * mu, vec3(0.0)));
            // mu always sits inside [lo, hi], so the intersection of the
            // two boxes can never be empty.
            lo = max(lo, mu - VARIANCE_GAMMA * sigma);
            hi = min(hi, mu + VARIANCE_GAMMA * sigma);
            history.rgb = clamp(history.rgb, lo, hi);
            out_colour.rgb = mix(history.rgb, current.rgb, blend);
        }
    }

    imageStore(history_out, pixel, vec4(out_colour.rgb, depth));
    // Alpha stays this frame's flag, never the blended one.
    imageStore(resolved_colour, pixel, vec4(out_colour.rgb, current.a));
}
