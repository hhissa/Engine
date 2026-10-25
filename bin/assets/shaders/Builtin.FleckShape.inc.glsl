// Fleck shapes: the 16 Dreams fleck styles and a fleck's paint relief,
// shared by the splat passes that draw flecks -- Builtin.ChunkPointSplat.
// comp.glsl (a surface's loose flecks) and Builtin.FleckSplat.comp.glsl
// (paint mode's free-floating ones), so both draw the same marks.

// Fleck shape (see fleck_covers()), in units of the fleck's own square.
const float FLECK_WOBBLE = 0.05;    // gentle wobble of the outline, so no
                                    // two flecks are quite the same shape
// Each fleck is a raised dab of paint, not a flat card: its surface rises
// by up to this many fleck radii (stored in each pixel's depth, so the
// render pass lights the real relief -- see fleck_paint_height()).
const float FLECK_DOME = 0.35;
// The dab's relief, as fractions of that height:
const float FLECK_BRISTLE = 0.14;   // grooves the brush's bristles left,
const float FLECK_BRISTLES = 5.0;   //   this many across the stroke
const float FLECK_LIP = 0.15;       // paint pushed up into a rim
const float FLECK_GRAIN = 0.04;     // fine unevenness

// Smooth 2D value noise in [0, 1].
float fleck_hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float fleck_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(fleck_hash(i), fleck_hash(i + vec2(1.0, 0.0)), f.x),
               mix(fleck_hash(i + vec2(0.0, 1.0)), fleck_hash(i + vec2(1.0, 1.0)), f.x),
               f.y);
}
const int FLECK_STYLE_CIRCLE = 1;

float fleck_segment_dist(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0));
}

// Whether point q of a fleck's own square [-1,1]^2 (u along the fleck's
// direction, v across it) is inside fleck `style`'s shape -- the 16 Dreams
// styles, in order. tools/modelling_software_v2/icons/make_fleck_icons.py
// draws the UI icons from the same shapes, so keep the two in step.
bool fleck_mask(int style, vec2 q) {
    float r = length(q);
    switch (style) {
    case 0: // Square
        return max(abs(q.x), abs(q.y)) <= 0.8;
    case 1: // Circle
        return r <= 1.0;
    case 2: // Triangle
        return q.y >= -0.48 && abs(q.x) <= (0.95 - q.y) * 0.5734;
    case 3: { // Hexagon
        vec2 a = abs(q);
        return max(a.x * 0.866 + a.y * 0.5, a.y) <= 0.85;
    }
    case 4: // Dots
        return length(q) <= 0.3 || length(q - vec2(0.6, 0.55)) <= 0.27 ||
               length(q - vec2(-0.6, 0.5)) <= 0.27 ||
               length(q - vec2(0.55, -0.6)) <= 0.27 ||
               length(q - vec2(-0.55, -0.6)) <= 0.27;
    case 5: // Scribble
        return abs(q.x) <= 0.95 && abs(q.y - 0.4 * sin(q.x * 7.0)) <= 0.2;
    case 6: // Veins
        return fleck_segment_dist(q, vec2(0.0, -0.95), vec2(0.0, 0.95)) <= 0.1 ||
               fleck_segment_dist(q, vec2(0.0, 0.1), vec2(0.7, 0.7)) <= 0.08 ||
               fleck_segment_dist(q, vec2(0.0, -0.3), vec2(-0.65, 0.25)) <= 0.08;
    case 7: { // Splat
        float a = atan(q.y, q.x);
        return r <= 0.66 + 0.18 * sin(5.0 * a + 0.7) + 0.12 * sin(9.0 * a + 2.1);
    }
    case 8: // Straight
        return abs(q.y) <= 0.22 && abs(q.x) <= 0.95;
    case 9: // Heavy
        return fleck_segment_dist(q, vec2(-0.55, 0.0), vec2(0.55, 0.0)) <= 0.45;
    case 10: // Hatching
        return max(abs(q.x), abs(q.y)) <= 0.9 && fract((q.y + 1.0) * 2.0) < 0.35;
    case 11: { // Cubist: an irregular four-sided shard
        vec2 v0 = vec2(-0.9, -0.3), v1 = vec2(0.2, -0.95);
        vec2 v2 = vec2(0.85, 0.4), v3 = vec2(-0.3, 0.85);
        float c0 = (v1.x - v0.x) * (q.y - v0.y) - (v1.y - v0.y) * (q.x - v0.x);
        float c1 = (v2.x - v1.x) * (q.y - v1.y) - (v2.y - v1.y) * (q.x - v1.x);
        float c2 = (v3.x - v2.x) * (q.y - v2.y) - (v3.y - v2.y) * (q.x - v2.x);
        float c3 = (v0.x - v3.x) * (q.y - v3.y) - (v0.y - v3.y) * (q.x - v3.x);
        return c0 >= 0.0 && c1 >= 0.0 && c2 >= 0.0 && c3 >= 0.0;
    }
    case 12: // Impressionist: a short dab
        return (q.x * q.x) / 0.9025 + (q.y * q.y) / 0.2025 <= 1.0;
    case 13: // Expressive: a curved, tapering stroke
        return abs(q.x) <= 0.95 &&
               abs(q.y - (0.6 * q.x * q.x - 0.3)) <= 0.3 * (1.0 - abs(q.x)) + 0.03;
    case 14: { // Baroque: a spiral
        float a = atan(q.y, q.x) + 3.14159265;
        for (int k = 0; k < 2; ++k) {
            float target = 0.2 + (a + 6.2831853 * float(k)) / 6.2831853 * 0.38;
            if (target < 0.97 && abs(r - target) < 0.1) {
                return true;
            }
        }
        return false;
    }
    case 15: // Streaky: broken streaks
        return abs(q.x) <= 0.98 &&
               ((abs(q.y - 0.1) <= 0.13 && fract(q.x * 2.5 + 0.3) < 0.72) ||
                (abs(q.y + 0.45) <= 0.08 && fract(q.x * 1.7) < 0.6));
    default:
        return r <= 1.0;
    }
}

// A fleck as a quick hand-made mark rather than a clean stamp: fleck_mask()'s
// shape, but each fleck its own size and stretch, its outline bent by a slow
// wobble and roughened by finer noise, and filled not solid but with a few
// wobbly scrawled lines running along it (u), gaps between -- so each one
// reads as a pen/brush scrawl, and neighbouring flecks of one style differ.
// `seed` is the fleck's own (fixed) random number.
// Paint height of a dab at shape-space point q (inside the shape): a soft
// mound, scored along the stroke (fleck x) by bristle grooves, with a lip
// of paint pushed up at the rim and a little grain. 0 at the edge, ~1 top.
float fleck_paint_height(vec2 q, vec2 offset) {
    float r = sqrt(dot(q, q) * 0.85);
    float mound = sqrt(max(1.0 - r * r, 0.0));
    // Grooves run along the stroke, wandering slightly, deepest mid-dab.
    float wander = 0.6 * fleck_noise(vec2(q.x * 1.3, 0.0) + offset + 4.1);
    float groove = 0.5 + 0.5 * sin((q.y + wander * 0.25) * FLECK_BRISTLES * 3.14159 +
                                   offset.y * 6.2831853);
    float bristle_depth = fleck_noise(vec2(q.x * 0.9, q.y * FLECK_BRISTLES) + offset);
    float bristles = groove * (0.5 + 0.5 * bristle_depth);
    float lip = smoothstep(0.5, 0.82, r) * (1.0 - smoothstep(0.82, 1.0, r));
    float grain = fleck_noise(q * 6.0 + offset + 3.3);
    return mound * (1.0 - FLECK_BRISTLE + FLECK_BRISTLE * bristles) +
           FLECK_LIP * lip + FLECK_GRAIN * grain * mound;
}

// How much of a fleck's fine detail -- its outline's wobble and its paint
// relief -- to draw at `radius_px` pixels across (half-width): none on a
// fleck too small for it to show, all of it from about 8 pixels up. The
// detail is several noise lookups per pixel, which is most of what a small
// fleck costs.
float fleck_detail(float radius_px) {
    return clamp((radius_px - 3.0) / 5.0, 0.0, 1.0);
}

// Whether fleck-space point q is inside this fleck; height = its paint
// height there (see fleck_paint_height()), with `detail` (fleck_detail())
// of its fine detail.
bool fleck_covers(int style, vec2 q, uint seed, float detail, out float height) {
    height = 0.0;
    float size = 0.85 + 0.35 * float((seed >> 20) & 63u) / 63.0;
    float stretch = 0.85 + 0.3 * float((seed >> 26) & 31u) / 31.0;
    q /= vec2(size * stretch, size / stretch);
    // Past every style's reach (the widest, a square's corner, is under
    // 1.3 out, plus the wobble): out, before any noise.
    if (dot(q, q) > 1.96) {
        return false;
    }
    if (detail <= 0.0) {
        if (!fleck_mask(style, q)) {
            return false;
        }
        float r2 = dot(q, q) * 0.85;
        height = sqrt(max(1.0 - r2, 0.0)); // the plain mound
        return true;
    }
    vec2 offset = vec2(float(seed & 1023u), float((seed >> 10) & 1023u)) * 0.173;
    q += detail * FLECK_WOBBLE * 2.0 *
         (vec2(fleck_noise(q * 1.7 + offset), fleck_noise(q * 1.7 + offset + 17.3)) - 0.5);
    if (!fleck_mask(style, q)) {
        return false;
    }
    float r2 = dot(q, q) * 0.85;
    height = mix(sqrt(max(1.0 - r2, 0.0)), fleck_paint_height(q, offset), detail);
    return true;
}
