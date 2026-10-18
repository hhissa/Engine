// Builtin.FleckPattern.inc.glsl
// M6 (Flecks): Dreams-style painterly surface-stroke styles -- originally 16
// named presets (Square, Circle, Triangle, Hexagon, Dots, Straight, Heavy,
// Hatching, Streaky, Scribble, Veins, Splat, Cubist, Impressionist,
// Expressive, Baroque), realized as ONE parameterized procedural evaluator
// rather than 16 distinct algorithms (see /home/h-i/.claude/plans/
// joyful-skipping-giraffe.md's own §6 -- an honest architectural
// simplification, not a cop-out: visual-fidelity risk here is tuning, not
// architecture). A 17th preset, Impasto (id 16), was added after M6 -- see
// its own dedicated family below (fleck_impasto_value()) and the dispatcher
// comment for why it is NOT shoehorned into the organic family despite
// looking superficially similar on paper. An 18th preset, Clay (id 17), was
// added after that -- see fleck_clay_value()'s own dedicated family below.
//
// POST-LAUNCH VISUAL PASS (this revision): M6 landed and ran correctly, but
// direct user feedback on the live result was "the flecks look too similar
// and not what they have labelled". Diagnosis + fix, in order of leverage:
//   1. fleck_modulate_albedo() was capped at mix(1.0, 0.7, darken) -- at
//      most 30% darkening, nowhere near enough contrast for a pattern to
//      read as distinct, let alone 16 of them from each other. Widened
//      dramatically (see FLECK_MIN_BRIGHTNESS below) AND given a small
//      per-family colour bias in the darkened regions (NOT a hue rotation
//      of the base material colour -- a fixed multiplier on top of it, so
//      a white material's Fleck still reads as "a shaded region" with a
//      believable cast, not a flat grey smear). This is the single biggest
//      fix -- see fleck_modulate_albedo()'s own comment.
//   2. FLECK_MAX_NORMAL_STRENGTH raised too (0.8 -> 1.4) now that albedo
//      contrast makes the pattern geometry itself worth seeing in relief.
//   3. Hexagon (old local_index==3) and Triangle (old local_index==2) were
//      honest approximations (a fat Minkowski-p metric, a rotated Manhattan
//      metric) that the ORIGINAL comments here admitted read as rounded
//      blobs / diamonds, not hexagons / triangles. Both are now REAL 2D
//      tilings (regular hex lattice via axial-round nearest-cell; an
//      equilateral triangular lattice via the standard simplex skew/
//      unskew), each projected onto all 3 triplanar planes and blended by
//      the same `blend = abs(normal)` weights fleck_perturb_normal() already
//      uses for its bump recombination -- see fleck_hex_cell_value()/
//      fleck_tri_cell_value() and their call sites in fleck_cellular_value().
//   4. Splat (organic local_index==2) no longer reuses the shared warped-fBm
//      -- it is now a genuinely different technique (irregular per-cell
//      blob placement with randomized radius and a per-cell presence
//      coin-flip, borrowing the Worley jitter machinery but NOT fBm at all),
//      because continuous warped noise reads as "blobby haze", not "paint
//      flicked at a surface". See fleck_splat_value().
//   5. The remaining organic presets (Scribble/Veins/Cubist/Impressionist/
//      Expressive/Baroque) had their frequency/warp/ridge-sharpness/band
//      parameters pushed further apart, and Impressionist gained a soft
//      contrast remap (daub-like blotches instead of a smooth gradient) --
//      see fleck_organic_value()'s own comment for the specific reasoning
//      per preset.
//
// POST-M6 ADDITION: Impasto (preset 16) -- a 4th family, not a parameter
// variant of organic. Real impasto (thick oil/acrylic dragged by a palette
// knife or loaded brush) has a very specific signature organic's isotropic
// domain-warped fBm cannot produce by tuning alone: directionally-coherent
// ridges (paint gets DRAGGED, so strokes have a dominant local direction,
// like the directional family) that are physically raised, so each ridge
// has an ASYMMETRIC cross-section -- a steep catch-light flank and a soft
// shadow flank -- not the symmetric `ridged` fold (1-abs(2v-1)) organic
// uses, which is mirror-symmetric across its crest by construction and so
// can only ever shade both flanks of a ridge identically. See fleck_
// impasto_value()'s own comment for the sawtooth-ramp technique that fixes
// this.
//
// POST-IMPASTO ADDITION: Clay (preset 17) -- a 5th family, not a parameter
// variant of Splat despite reusing its per-cell jitter/coin-flip placement
// machinery (see fleck_splat_value() above). "Soft clay molded into flecks"
// needs ROUNDED, CONCAVE press-marks -- the surface pushed IN at each mark
// with a smooth, fully-rounded gradient from rim to centre, like a thumb or
// a soft tool dabbed into damp clay -- the opposite sign, and a softer,
// full-radius falloff, from every other organic/Impasto preset (all of
// which read as raised material: paint, veins, ridges). See fleck_clay_
// value()'s own comment for the technique and why its field is a literal
// height map (background-high, dimple-low) rather than a "feature
// presence" mask, and fleck_modulate_albedo()'s Clay branch for why its
// albedo contribution is deliberately restrained relative to every other
// preset -- clay's own colour barely varies; the moulded relief is what
// has to sell it.
//
// Every preset maps to exactly one of 5 families, selected by simple range
// checks on the 0-17 preset id (no separate "pattern_family" value is ever
// stored -- it is always derived from the id, here and on the C++ side):
//   0-4  Square/Circle/Triangle/Hexagon/Dots   -- cellular  (Square/Circle/
//        Dots: true 3D Worley/Voronoi, metric/threshold varies the cell
//        shape; Triangle/Hexagon: true 2D tilings projected triplanar, see
//        above)
//   5-8  Straight/Heavy/Hatching/Streaky       -- directional (anisotropic
//        striped fBm, direction count/warp/sharpness varies)
//   9-15 Scribble/Veins/Splat/Cubist/
//        Impressionist/Expressive/Baroque      -- organic (domain-warped
//        fBm for 6 of the 7, Splat is a distinct blob-placement technique --
//        see above)
//   16   Impasto                               -- its own family: directional
//        stroke coherence + an asymmetric sawtooth ridge profile (see above)
//   17   Clay                                  -- its own family: soft,
//        irregularly-placed CONCAVE press-marks (see above)
//
// Entry points (the only two symbols anything outside this file calls):
//   fleck_perturb_normal(world_pos, normal, pattern_family, scale, strength)
//   fleck_modulate_albedo(world_pos, normal, pattern_family, scale, strength)
// `pattern_family` in both signatures is literally the 0-17 preset id, not
// a pre-reduced family index -- the name is kept as specified; the family
// switch happens internally (fleck_pattern_value() below), together with a
// per-preset "local index" (id minus its family's base) that drives the
// preset's own metric/threshold/warp tuning. Both are no-ops (return the
// input unchanged) when strength <= 0 or pattern_family < 0, so a material
// with no Fleck configured (every pre-M6 slot, including Default) takes
// neither function's cost nor its output -- see call sites in Builtin.
// DeferredShade.comp.glsl, both gated the same way. fleck_modulate_albedo()
// now also takes `normal` (it didn't before this revision) -- purely to
// compute the SAME triplanar blend weights fleck_perturb_normal() already
// derives from it, needed for Hexagon/Triangle's plane-projected lookups;
// every other preset ignores the extra parameter.
//
// Sampling is fully 3D (world_pos, optionally divided by `scale`) for every
// preset except Hexagon/Triangle, not a 2D texture projected per triplanar
// plane -- cellular (Square/Circle/Dots)/directional/organic below are all
// genuine volumetric fields, so they need no UV unwrap and have no seams.
// fleck_perturb_normal() still recombines 3 axis-aligned finite-difference
// taps via the SAME triplanar blend weights (`blend = abs(normal); blend /=
// blend.x+blend.y+blend.z`) and whiteout-blend recombination Builtin.
// DeferredShade.comp.glsl's own apply_triplanar_bump() uses for texture-
// based bump mapping, so a Fleck reads as "the same kind of surface detail"
// bump mapping already produces, just procedural instead of texture-
// sampled -- see fleck_perturb_normal()'s own comment for why this needs
// only 4 field samples total (not the 6 two-per-plane bump_from_heights()/
// sample_scene_heights() needs), a win a true 3D field gets for free that a
// 2D texture-per-plane technique cannot. Hexagon/Triangle spend a bit more
// (3 plane lookups per field sample instead of 1) in exchange for actually
// looking like their names -- see point 3 above. Clay (see below) is a
// genuine 3D field too, so it takes the same cheap 4-sample path.

// --- Hashing / base value noise -------------------------------------------

// Scalar hash, 3D -> [0,1). Deliberately a different formula from the
// parent shader's own hash12() (2D, interleaved-gradient-noise style, used
// there only for volumetric dither) -- this file is self-contained (see its
// own header above) and is #include-d before hash12() is defined further
// down Builtin.DeferredShade.comp.glsl, so it cannot depend on it anyway.
float fleck_hash13(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Vector hash, 3D -> [0,1)^3 -- used as each cellular feature point's
// jittered offset within its lattice cell (classic Worley/Voronoi jitter).
vec3 fleck_hash33(vec3 p) {
    p = vec3(dot(p, vec3(127.1, 311.7, 74.7)),
             dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453123);
}

// Trilinearly-interpolated value noise over the 8 corners of the lattice
// cell containing p -- the base octave fleck_fbm() below sums.
float fleck_value_noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f); // smoothstep interpolant, same as GLM's smoothstep

    float n000 = fleck_hash13(i + vec3(0.0, 0.0, 0.0));
    float n100 = fleck_hash13(i + vec3(1.0, 0.0, 0.0));
    float n010 = fleck_hash13(i + vec3(0.0, 1.0, 0.0));
    float n110 = fleck_hash13(i + vec3(1.0, 1.0, 0.0));
    float n001 = fleck_hash13(i + vec3(0.0, 0.0, 1.0));
    float n101 = fleck_hash13(i + vec3(1.0, 0.0, 1.0));
    float n011 = fleck_hash13(i + vec3(0.0, 1.0, 1.0));
    float n111 = fleck_hash13(i + vec3(1.0, 1.0, 1.0));

    float nx00 = mix(n000, n100, u.x);
    float nx10 = mix(n010, n110, u.x);
    float nx01 = mix(n001, n101, u.x);
    float nx11 = mix(n011, n111, u.x);

    float nxy0 = mix(nx00, nx10, u.y);
    float nxy1 = mix(nx01, nx11, u.y);

    return mix(nxy0, nxy1, u.z);
}

// Fractal Brownian motion: `octaves` (clamped to FLECK_MAX_OCTAVES, a fixed
// loop bound GLSL requires) summed layers of fleck_value_noise, each at
// roughly double the previous layer's frequency and half its amplitude. The
// 2.03 (not 2.0) lattice-frequency multiplier is deliberate -- an exact
// octave-doubling re-aligns every layer's lattice on the same integer grid
// periodically, which reads as a faint repeating grid in the result; 2.03
// avoids that alignment entirely.
const int FLECK_MAX_OCTAVES = 5;

float fleck_fbm(vec3 p, int octaves) {
    float sum = 0.0;
    float amp = 0.5;
    vec3 pp = p;
    for (int i = 0; i < FLECK_MAX_OCTAVES; ++i) {
        if (i >= octaves) {
            break;
        }
        sum += amp * fleck_value_noise(pp);
        pp *= 2.03;
        amp *= 0.5;
    }
    return sum;
}

// Point-to-segment distance, 2D -- shared by Triangle's edge-emphasis field
// below (and cheap enough it isn't worth specializing per axis).
float fleck_point_segment_dist(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
    vec2 proj = a + ab * t;
    return length(p - proj);
}

// --- Real 2D hex-lattice helpers (Hexagon) --------------------------------
//
// Standard axial-coordinate hex grid ("which hex cell contains this 2D
// point", "cube-coordinate rounding" -- see Red Blob Games' hex grid guide
// for the general technique, reproduced here from first principles). Unlike
// a Worley/Voronoi search, this lattice has FIXED, unjittered cell centres
// -- that regularity is exactly what makes it read as an actual hexagonal
// tile grid rather than randomly-sized rounded blobs.
const float FLECK_SQRT3 = 1.7320508;

vec3 fleck_hex_round(vec3 cube) {
    vec3 r = round(cube);
    vec3 d = abs(r - cube);
    if (d.x > d.y && d.x > d.z) {
        r.x = -r.y - r.z;
    } else if (d.y > d.z) {
        r.y = -r.x - r.z;
    } else {
        r.z = -r.x - r.y;
    }
    return r;
}

vec2 fleck_hex_axial_to_px(vec2 axial) {
    return vec2(1.5 * axial.x, FLECK_SQRT3 * (axial.y + axial.x * 0.5));
}

// Edge-emphasis field (near 1 at a hex cell's boundary, fading to 0 toward
// its interior) over a REGULAR 2D hex lattice. Checks the nearest cell's
// own centre plus its 6 immediate neighbours (always enough to find the
// true second-nearest centre near any shared edge) and reuses the same
// "f2 - f1" edge read the Worley cellular presets use.
float fleck_hex_cell_value(vec2 p) {
    vec2 axial = vec2((2.0 / 3.0) * p.x,
                      (-1.0 / 3.0) * p.x + (FLECK_SQRT3 / 3.0) * p.y);
    vec3 cube = vec3(axial.x, -axial.x - axial.y, axial.y);
    vec3 rounded = fleck_hex_round(cube);
    vec2 centre_axial = vec2(rounded.x, rounded.z);

    const vec2 kHexNeighbours[7] = vec2[7](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, -1.0), vec2(0.0, -1.0),
        vec2(-1.0, 0.0), vec2(-1.0, 1.0), vec2(0.0, 1.0));

    float f1 = 1e9;
    float f2 = 1e9;
    for (int i = 0; i < 7; ++i) {
        vec2 centre_px = fleck_hex_axial_to_px(centre_axial + kHexNeighbours[i]);
        float dist = length(p - centre_px);
        if (dist < f1) {
            f2 = f1;
            f1 = dist;
        } else if (dist < f2) {
            f2 = dist;
        }
    }
    return clamp(1.0 - smoothstep(0.0, 0.42, f2 - f1), 0.0, 1.0);
}

// --- Real 2D equilateral-triangle lattice helpers (Triangle) --------------
//
// The standard 2D simplex skew/unskew (the exact technique simplex NOISE
// uses to find which unit equilateral triangle of the lattice a point falls
// in -- used here for an actual triangular TILING, not noise). Each unit
// cell of the skewed square grid splits into exactly 2 equilateral
// triangles ("upper"/"lower"); this returns the 3 vertices (in original,
// unskewed space) of whichever one contains `p`.
const float FLECK_TRI_F2 = 0.3660254; // 0.5 * (sqrt(3) - 1)
const float FLECK_TRI_G2 = 0.2113249; // (3 - sqrt(3)) / 6

vec2 fleck_tri_unskew(vec2 cell) {
    float t = (cell.x + cell.y) * FLECK_TRI_G2;
    return cell - t;
}

void fleck_tri_cell(vec2 p, out vec2 v0, out vec2 v1, out vec2 v2) {
    float s = (p.x + p.y) * FLECK_TRI_F2;
    vec2 skewed = p + s;
    vec2 cell = floor(skewed);
    vec2 local = p - fleck_tri_unskew(cell);

    v0 = fleck_tri_unskew(cell);
    if (local.x > local.y) {
        v1 = fleck_tri_unskew(cell + vec2(1.0, 0.0)); // "lower" triangle
    } else {
        v1 = fleck_tri_unskew(cell + vec2(0.0, 1.0)); // "upper" triangle
    }
    v2 = fleck_tri_unskew(cell + vec2(1.0, 1.0));
}

// Edge-emphasis field over the true triangular lattice fleck_tri_cell()
// defines -- near 1 close to any of the current triangle's 3 edges, fading
// to 0 toward its centroid, so it reads as faceted triangular panels with
// visible seams rather than the old diamond/rhombus Voronoi cells.
float fleck_tri_cell_value(vec2 p) {
    vec2 v0, v1, v2;
    fleck_tri_cell(p, v0, v1, v2);

    float d0 = fleck_point_segment_dist(p, v0, v1);
    float d1 = fleck_point_segment_dist(p, v1, v2);
    float d2 = fleck_point_segment_dist(p, v2, v0);
    float edge_dist = min(d0, min(d1, d2));

    return clamp(1.0 - smoothstep(0.0, 0.14, edge_dist), 0.0, 1.0);
}

// --- Family 0: cellular (0-4 -- Square/Circle/Triangle/Hexagon/Dots) ------
//
// Square/Circle/Dots stay a 3x3x3-neighbourhood 3D Worley/Voronoi search
// (one jittered feature point per lattice cell, nearest/second-nearest
// distance under a metric that varies the cell shape -- Square: Chebyshev,
// true square cells; Circle: Euclidean, rounded cells; Dots: Euclidean but
// reading only the single nearest point's own small radial mask, "mostly
// background, isolated small features", not "tiled cells with seams").
// Triangle/Hexagon instead go through the real 2D tilings above, projected
// onto all 3 triplanar planes (p.yz/p.xz/p.xy) and blended by `blend` --
// the SAME blend weights (`abs(normal)`, normalized) fleck_perturb_normal()
// already derives for its own triplanar bump recombination, now threaded
// in here too (see fleck_pattern_value()/fleck_modulate_albedo()).
float fleck_cellular_value(vec3 p, int local_index, vec3 blend) {
    if (local_index == 2) {
        // Triangle.
        float v_x = fleck_tri_cell_value(p.yz);
        float v_y = fleck_tri_cell_value(p.xz);
        float v_z = fleck_tri_cell_value(p.xy);
        return v_x * blend.x + v_y * blend.y + v_z * blend.z;
    }
    if (local_index == 3) {
        // Hexagon.
        float v_x = fleck_hex_cell_value(p.yz);
        float v_y = fleck_hex_cell_value(p.xz);
        float v_z = fleck_hex_cell_value(p.xy);
        return v_x * blend.x + v_y * blend.y + v_z * blend.z;
    }

    vec3 cell = floor(p);
    float f1 = 1e9;
    float f2 = 1e9;
    float nearest_point_dist = 1e9;

    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                vec3 neighbor = cell + vec3(float(dx), float(dy), float(dz));
                vec3 jitter = fleck_hash33(neighbor);
                vec3 d = (neighbor + jitter) - p;

                float dist;
                if (local_index == 0) {
                    // Square: Chebyshev (L-infinity) -- true square cells.
                    vec3 a = abs(d);
                    dist = max(a.x, max(a.y, a.z));
                } else {
                    // Circle (1) and Dots (4): plain Euclidean.
                    dist = length(d);
                }

                if (dist < f1) {
                    f2 = f1;
                    f1 = dist;
                } else if (dist < f2) {
                    f2 = dist;
                }
                nearest_point_dist = min(nearest_point_dist, length(d));
            }
        }
    }

    if (local_index == 4) {
        // Dots: a small, sharp-edged radial mask around each feature point
        // -- 1 at the point, falling to 0 by radius 0.28 (in cell units, so
        // independent of `scale`, which already rescales world_pos before
        // this is ever called).
        return 1.0 - smoothstep(0.10, 0.28, nearest_point_dist);
    }
    // Square/Circle: edge emphasis -- near 1 exactly where two cells meet
    // (f2 approx f1, the "grout line"), fading to 0 toward a cell's
    // interior. This is what makes Square/Circle read as tiled surface
    // facets rather than a uniform haze.
    return clamp(1.0 - smoothstep(0.0, 0.35, f2 - f1), 0.0, 1.0);
}

// --- Family 1: directional (5-8 -- Straight/Heavy/Hatching/Streaky) -------
//
// An anisotropic striped fBm: a fixed oblique 3D direction (not axis-
// aligned, so the stripes read across all 3 triplanar planes instead of
// vanishing edge-on against one of them) is dot-producted with p to get a
// 1D coordinate, which an fBm-warped sine-like triangle wave turns into
// stripes. `local_index` varies frequency/warp-amount/sharpness, pushed
// further apart than M6's original tuning so the 4 read as distinct at a
// glance: Straight is now thin, crisp and nearly dead straight (high
// sharpness, minimal warp); Heavy is thick and bold but NOT wavy (low
// sharpness, low warp -- "heavy", not "streaky"); Streaky leans hard into
// irregular wavy warp instead (so it no longer looks like a slightly-
// thinner Heavy); Hatching additionally crosses a SECOND stripe field along
// a different oblique direction and takes the max -- the one preset
// genuinely structurally different from its 3 siblings, not just a
// parameter change.
float fleck_directional_value(vec3 p, int local_index) {
    const vec3 kDirA = vec3(0.8018, 0.2673, 0.5345); // fixed oblique axis 1
    const vec3 kDirB = vec3(-0.2857, 0.8571, -0.4286); // fixed oblique axis 2 (Hatching only)

    float freq;
    float warp_amount;
    float sharpness;
    int warp_octaves;
    if (local_index == 0) { // Straight
        freq = 12.0; warp_amount = 0.04; sharpness = 4.5; warp_octaves = 1;
    } else if (local_index == 1) { // Heavy
        freq = 4.5; warp_amount = 0.08; sharpness = 0.9; warp_octaves = 1;
    } else if (local_index == 2) { // Hatching
        freq = 9.0; warp_amount = 0.15; sharpness = 3.0; warp_octaves = 2;
    } else { // Streaky (3)
        freq = 5.5; warp_amount = 1.4; sharpness = 1.5; warp_octaves = 3;
    }

    float warp = fleck_fbm(p * 0.5, warp_octaves) - 0.5;

    float coord_a = dot(p, kDirA) * freq + warp * warp_amount * freq;
    float stripe_a = abs(fract(coord_a) * 2.0 - 1.0);
    float line = 1.0 - pow(stripe_a, sharpness);

    if (local_index == 2) { // Hatching: cross with the second direction.
        float coord_b = dot(p, kDirB) * freq + warp * warp_amount * freq;
        float stripe_b = abs(fract(coord_b) * 2.0 - 1.0);
        float line_b = 1.0 - pow(stripe_b, sharpness);
        line = max(line, line_b);
    }

    return clamp(line, 0.0, 1.0);
}

// --- Organic family helper: Splat (9-15 local_index==2) -------------------
//
// Splat used to just be a 7th set of warped-fBm parameters, which reads as
// "blobby haze" like its 6 siblings, not "paint flicked at a surface". This
// is a genuinely different technique: one candidate blob per 3D lattice
// cell (reusing the SAME Worley jitter fleck_cellular_value() uses for
// "one feature point per cell"), but a cell only gets a blob ~58% of the
// time (a hashed per-cell coin-flip -- sparse, irregular placement) and
// each blob's RADIUS is itself randomized per-cell (not a fixed size like
// Dots) -- sparse + widely-varying sizes is what reads as discrete splatter
// droplets rather than a regular polka-dot grid or a continuous noise field.
float fleck_splat_value(vec3 p) {
    vec3 cell = floor(p);
    float best = 0.0;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                vec3 neighbor = cell + vec3(float(dx), float(dy), float(dz));
                float present = fleck_hash13(neighbor + vec3(9.1, 4.7, 2.3));
                if (present > 0.42) {
                    continue; // ~58% of cells get no blob at all.
                }
                vec3 jitter = fleck_hash33(neighbor);
                float radius = 0.14 + 0.34 * fleck_hash13(neighbor + vec3(1.7, 6.2, 3.3));
                vec3 d = (neighbor + jitter) - p;
                float dist = length(d);
                float mask = 1.0 - smoothstep(radius * 0.55, radius, dist);
                best = max(best, mask);
            }
        }
    }
    return clamp(best, 0.0, 1.0);
}

// --- Family 2: organic/fractal (9-15) -------------------------------------
//
// Scribble/Veins/Cubist/Impressionist/Expressive/Baroque -- a domain-warped
// fBm (the position fed to the final fBm is itself displaced by 3
// independent fBm fields, the standard "warped noise" technique for organic
// rather than grid-aligned results), with `local_index` driving warp
// strength, octave count and post-processing (`ridged` folds the result
// into thin veins/strokes via 1-abs(2v-1), raised to `ridge_power` to
// control how thin/sharp vs thick/soft the strokes read; `bands` quantizes
// into flat steps for a faceted look; `soft_contrast` -- new -- remaps via
// smoothstep around the midpoint for a blotchy "separate daubs" look
// without hard banding). Splat (local_index==2) is the one exception --
// see fleck_splat_value() above, a different technique entirely. Parameters
// were pushed further apart from M6's original tuning (frequency spread
// roughly 1.1 - 4.8 across the family now, vs. all 6 clustered 1.2 - 3.0
// before) specifically because a tight cluster of "warped fBm with minor
// parameter tweaks" is exactly what reads as "random blobby noise" rather
// than 7 distinct painterly styles:
//   Scribble: busier/finer than before (freq 3.0 -> 4.8) so it doesn't
//     read as a slightly-coarser Veins; looser ridge_power (1.6) for
//     scrawl-like, less uniform lines.
//   Veins: lower freq (1.6 -> 1.1, bigger/sparser network) + higher
//     ridge_power (3.2, thinner/sharper veins) so it reads as an open
//     branching network, not a denser Scribble.
//   Cubist: bands coarsened 4 -> 3 (blockier, fewer distinct facets per
//     area) with lower freq (1.5 -> 1.05, bigger facets) and higher warp
//     (1.5 -> 2.1, more irregular facet shapes) so facets read as actual
//     angular panels, not fine banding.
//   Impressionist: gained soft_contrast (smoothstep remap) so it reads as
//     separate soft daubs/blotches instead of a smooth smeared gradient --
//     the thing that most made it indistinguishable from "no fleck, just
//     low-contrast shading" before.
//   Expressive: warp pushed up (2.2 -> 2.6) and ridge_power lowered (1.0,
//     thick bold strokes) so it reads as bold/chaotic, distinct from
//     Baroque's fine ornate density below.
//   Baroque: freq raised (2.2 -> 3.6) with full 5 octaves and a tighter
//     ridge_power (2.4) for dense, fine ornate detail -- distinct from
//     Expressive's thick bold strokes at the opposite end of the family's
//     frequency range.
float fleck_organic_value(vec3 p, int local_index) {
    if (local_index == 2) { // Splat
        return fleck_splat_value(p * 1.1);
    }

    float warp_strength;
    int warp_octaves;
    int octaves;
    float freq;
    bool ridged;
    float ridge_power;
    int bands;
    bool soft_contrast;
    if (local_index == 0) { // Scribble
        warp_strength = 1.3; warp_octaves = 2; octaves = 4; freq = 4.8;
        ridged = true; ridge_power = 1.6; bands = 0; soft_contrast = false;
    } else if (local_index == 1) { // Veins
        warp_strength = 2.0; warp_octaves = 2; octaves = 3; freq = 1.1;
        ridged = true; ridge_power = 3.2; bands = 0; soft_contrast = false;
    } else if (local_index == 3) { // Cubist
        warp_strength = 2.1; warp_octaves = 1; octaves = 2; freq = 1.05;
        ridged = false; ridge_power = 1.0; bands = 3; soft_contrast = false;
    } else if (local_index == 4) { // Impressionist
        warp_strength = 0.8; warp_octaves = 2; octaves = 5; freq = 2.2;
        ridged = false; ridge_power = 1.0; bands = 0; soft_contrast = true;
    } else if (local_index == 5) { // Expressive
        warp_strength = 2.6; warp_octaves = 3; octaves = 3; freq = 2.5;
        ridged = true; ridge_power = 1.0; bands = 0; soft_contrast = false;
    } else { // Baroque (6)
        warp_strength = 1.9; warp_octaves = 3; octaves = 5; freq = 3.6;
        ridged = true; ridge_power = 2.4; bands = 0; soft_contrast = false;
    }

    vec3 wp = p * freq;
    vec3 warp_offset = vec3(
        fleck_fbm(wp + vec3(0.0, 0.0, 0.0), warp_octaves),
        fleck_fbm(wp + vec3(5.2, 1.3, 2.8), warp_octaves),
        fleck_fbm(wp + vec3(8.3, 2.8, 4.1), warp_octaves));
    vec3 q = wp + warp_strength * (warp_offset - 0.5) * 2.0;
    float v = fleck_fbm(q, octaves);

    if (ridged) {
        v = 1.0 - abs(v * 2.0 - 1.0);
        v = pow(clamp(v, 0.0, 1.0), ridge_power);
    }
    if (bands > 0) {
        v = floor(v * float(bands)) / float(bands);
    }
    if (soft_contrast) {
        v = smoothstep(0.32, 0.68, v);
    }
    return clamp(v, 0.0, 1.0);
}

// --- Family 3 (new): Impasto (16) -- directional ridges, asymmetric profile
//
// The organic family's `ridged` fold (fleck_organic_value(): v =
// 1-abs(2v-1)) is SYMMETRIC around each ridge crest by construction -- walk
// away from a crest in either direction and the value falls off identically,
// so the finite-difference normal fleck_perturb_normal() derives from it is
// mirror-symmetric too: both flanks of every ridge shade the same way. Real
// impasto cannot look like that -- paint physically raised by a palette
// knife or loaded brush has one flank the knife/brush pushed up steeply
// (catches the key light hard, a sharp bright highlight) and one flank it
// trailed off into gradually (a soft shadow trough). The fix is a sawtooth-
// style ramp instead of a triangle wave: within each period, a SHORT steep
// rise (0 -> 1 over `rise_frac` of the period) followed by a LONG gradual
// fall (1 -> 0 over the rest) -- asymmetric by construction, so the two
// flanks' finite-difference slopes differ in magnitude and the lighting
// naturally follows (no special-casing needed in fleck_perturb_normal()
// itself; it just differentiates whatever height field it's handed).
//
// Direction coherence comes from the SAME oblique-axis technique the
// directional family uses (a fixed 3D direction dotted with p, not an
// axis-aligned stripe, so it reads across all 3 triplanar planes) --
// "paint gets dragged" means a dominant local direction, like Heavy/
// Streaky, not organic's isotropic warp. A much MILDER domain warp than
// organic's (0.35 vs. organic's 0.8-2.6) lets ridges meander like an actual
// knife stroke without destroying that directionality into blobbiness.
// Frequency is low (1.8) for deliberately chunky, thick-looking ridges, not
// fine-grained strokes. A second, lower-frequency cross-grain modulation
// (a different oblique direction) breaks otherwise-perfectly-parallel
// ridges into shorter dragged segments with a value break partway along
// each one -- the "brush lifts and re-touches" look real impasto shows,
// and exactly the per-stroke "value/colour break" called for. The final
// pow(.., 0.6) broadens/rounds the crest into a flattish plateau (a thick
// raised BEAD of paint) rather than a knife-thin spike.
float fleck_impasto_value(vec3 p) {
    const vec3 kImpastoDir = vec3(0.8018, 0.2673, 0.5345); // same fixed-oblique-axis convention as the directional family
    const vec3 kImpastoCrossDir = vec3(-0.5345, 0.8018, 0.2673); // a different oblique axis, for the cross-grain break

    const float freq = 1.8;          // low -- chunky, thick ridges, not fine strokes
    const float warp_amount = 0.35;  // mild -- meanders without losing direction coherence
    const float rise_frac = 0.22;    // short steep flank / long soft flank, per period

    float warp = fleck_fbm(p * 0.35, 2) - 0.5;
    float coord = dot(p, kImpastoDir) * freq + warp * warp_amount;
    float t = fract(coord);

    float ridge = (t < rise_frac)
        ? t / rise_frac                                    // steep rise -> sharp bright flank
        : 1.0 - (t - rise_frac) / (1.0 - rise_frac);        // gradual fall -> soft shadow flank
    ridge = pow(clamp(ridge, 0.0, 1.0), 0.6);                // broaden the crest into a thick plateau

    float cross_coord = dot(p, kImpastoCrossDir) * (freq * 0.6);
    float cross_mod = fleck_value_noise(vec3(cross_coord, cross_coord * 0.7, 0.0) * 1.3);
    ridge *= mix(0.72, 1.0, cross_mod); // per-stroke value break (brush lift/re-touch)

    return clamp(ridge, 0.0, 1.0);
}

// --- Family 4 (new): Clay (17) -- soft, concave press-marks --------------
//
// Splat (fleck_splat_value() above) places discrete RAISED blobs with a
// narrow-band edge (smoothstep(radius*0.55, radius, dist) -- flat interior,
// sharp-ish rim) -- it reads as paint flicked at a surface. Clay needs the
// opposite sign and a completely different falloff shape: a real press-
// mark (a thumb or a soft tool pushed into damp clay) is a DENT, not a
// blob -- the surface recedes smoothly from the surrounding flat level
// down to a rounded low point at the mark's centre, with NO flat plateau
// and NO defined rim (an actual press is a continuous, fully-smooth
// gradient the entire way from rim to centre -- a soft paraboloid/rounded-
// cone profile, not a Voronoi cell boundary or Splat's raised-disc edge).
//
// fleck_clay_dip() is a FULL-RADIUS smoothstep (zero derivative at BOTH
// the centre and the rim), unlike Splat's narrow band -- that is what
// removes the plateau/hard-edge look and makes every mark read as a
// single smooth, rounded dimple instead of a flat-topped raised disc.
// fleck_clay_value() reuses the SAME per-cell Worley jitter + coin-flip
// presence test fleck_splat_value() introduced (irregular, non-grid
// placement -- the one piece of machinery genuinely worth sharing rather
// than re-deriving), but with a lower presence probability and a wider,
// larger radius range: a worked clay surface shows individual dabs you
// can see the shape of, not fine grain, so marks here are deliberately
// bigger and sparser than Splat's.
//
// The value returned is a literal HEIGHT field, not an inverted blob mask:
// 1.0 (the undisturbed flat clay level) minus the deepest dip found at
// this point, so background is high/flat and a dimple's centre is the
// lowest point nearby. fleck_perturb_normal() needs no special-casing to
// read this as concave -- it already just differentiates whatever height
// field fleck_pattern_value() hands it (same philosophy as fleck_impasto_
// value()'s asymmetric ridge -- see its own comment); a point at a
// dimple's centre simply sits lower than its neighbours, and the ordinary
// finite-difference normal does the rest. fleck_modulate_albedo() has to
// invert this (dark where the field is LOW, not high) precisely because
// of that same sign flip -- see its own Clay branch.
float fleck_clay_dip(float dist, float radius) {
    float t = clamp(dist / max(radius, 1e-4), 0.0, 1.0);
    return 1.0 - smoothstep(0.0, 1.0, t);
}

float fleck_clay_value(vec3 p) {
    vec3 cell = floor(p);
    float best_dip = 0.0;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                vec3 neighbor = cell + vec3(float(dx), float(dy), float(dz));
                float present = fleck_hash13(neighbor + vec3(5.3, 8.9, 1.1));
                if (present > 0.46) {
                    continue; // sparser than Splat -- fewer, bigger, individually-readable dabs.
                }
                vec3 jitter = fleck_hash33(neighbor);
                // Bigger and more size-varied than Splat's 0.14-0.48 -- real
                // thumb presses are large enough to read individually, and
                // uneven in size.
                float radius = 0.30 + 0.45 * fleck_hash13(neighbor + vec3(2.9, 7.1, 4.4));
                vec3 d = (neighbor + jitter) - p;
                float dist = length(d);
                best_dip = max(best_dip, fleck_clay_dip(dist, radius));
            }
        }
    }
    return clamp(1.0 - best_dip, 0.0, 1.0);
}

// --- The single dispatcher every entry point below goes through ----------
//
// `pattern` is the raw 0-17 preset id; the family is derived here with a
// plain range check (0-4 cellular, 5-8 directional, 9-15 organic, 16
// Impasto, 17 Clay), exactly as the plan's own §6 specifies (extended for
// Impasto and Clay), rather than being a separately stored value -- see
// this file's own header comment. `blend` is only consumed by the cellular
// family's Triangle/Hexagon branches (see fleck_cellular_value()); every
// other branch ignores it, so passing it unconditionally here costs
// nothing extra for the other presets. `pattern > 16` (not `== 17`) is the
// Clay test so any accidental future id past 17 degrades to Clay (the new
// "last" preset) rather than Impasto or an out-of-range organic
// local_index -- the same forward-compatible convention Impasto's own
// `pattern > 15` test established.
float fleck_pattern_value(vec3 p, int pattern, vec3 blend) {
    if (pattern <= 4) {
        return fleck_cellular_value(p, pattern, blend);
    } else if (pattern <= 8) {
        return fleck_directional_value(p, pattern - 5);
    } else if (pattern <= 15) {
        return fleck_organic_value(p, pattern - 9);
    } else if (pattern <= 16) {
        return fleck_impasto_value(p);
    }
    return fleck_clay_value(p);
}

// --- Public entry point 1: normal perturbation ----------------------------

// Epsilon (in already-scale-divided pattern space, so independent of
// `scale` itself) the finite-difference taps below are spaced at --
// analogous to Builtin.DeferredShade.comp.glsl's own BUMP_UV_EPSILON, just
// in a space where one full pattern period is roughly 1 unit instead of the
// texture's UV [0,1].
const float FLECK_BUMP_EPSILON = 0.06;
// The strongest a Fleck is ever allowed to perturb the normal by, reached
// at strength = 1. Raised from M6's original 0.8 -- now that albedo
// contrast (see fleck_modulate_albedo() below) makes the pattern itself
// worth seeing in relief under this tool's one-directional-light setup,
// the old ceiling left bump-mapped detail visible only right at specular
// highlights/grazing angles; 1.4 still stops well short of reading as
// broken geometry (apply_triplanar_bump()'s own whiteout blend keeps this
// additive on top of the base normal, not a replacement for it).
const float FLECK_MAX_NORMAL_STRENGTH = 1.4;

// Same tangent-space construction as Builtin.DeferredShade.comp.glsl's own
// bump_from_heights(): two nearby samples minus the centre give a
// (dHeight/du, dHeight/dv) slope, negated and stacked on a unit Z so
// brighter/higher neighbours push the normal away from them.
vec3 fleck_bump_from_heights(float h_centre, float h_u, float h_v, float strength) {
    float du = (h_u - h_centre) / FLECK_BUMP_EPSILON;
    float dv = (h_v - h_centre) / FLECK_BUMP_EPSILON;
    return normalize(vec3(-du * strength, -dv * strength, 1.0));
}

// Identical whiteout-blend recombination to Builtin.DeferredShade.comp.
// glsl's own apply_triplanar_bump() (duplicated, not shared -- this file is
// self-contained per its own header comment, the same "near-copy rather
// than force a shared include" convention the parent shader's own top-of-
// file comment already documents for its raymarch/deferred split).
vec3 fleck_apply_triplanar(vec3 base_normal, vec3 blend, vec3 bump_x, vec3 bump_y, vec3 bump_z) {
    vec3 nx = vec3(bump_x.xy + base_normal.zy, base_normal.x);
    vec3 ny = vec3(bump_y.xy + base_normal.xz, base_normal.y);
    vec3 nz = vec3(bump_z.xy + base_normal.xy, base_normal.z);
    return normalize(nx.zyx * blend.x + ny.xzy * blend.y + nz.xyz * blend.z);
}

// Perturbs `normal` with the Fleck pattern's surface relief, triplanar-
// blended by the SAME `blend = abs(normal)` weights Builtin.DeferredShade.
// comp.glsl's own bump mapping uses (see that call site, right after which
// this is meant to run). Returns `normal` unchanged (not even sampling the
// field) when strength <= 0 or pattern_family < 0 -- the gate that makes a
// material with no Fleck configured cost nothing extra and render bit-for-
// bit as it did before this milestone.
//
// Needs only 4 field samples (centre + one tap along each world axis) for
// 16 of the 18 presets -- Triangle/Hexagon's plane-projected lookups (see
// fleck_cellular_value()) cost a bit more per sample, but the sampling
// STRUCTURE here (centre + 3 axis taps, reused across all 3 triplanar
// planes) is unchanged -- a genuine win unique to this being a true field
// rather than a 2D texture projected per plane: the centre height is
// identical for all 3 triplanar planes (there is only one 3D point, not 3
// independent 2D ones), so each plane's pair of taps is just 2 of these 3
// shared per-axis samples.
vec3 fleck_perturb_normal(vec3 world_pos, vec3 normal, int pattern_family,
                          float scale, float strength) {
    if (strength <= 0.0 || pattern_family < 0) {
        return normal;
    }

    float safe_scale = max(scale, 0.05);
    vec3 tri_p = world_pos / safe_scale;
    vec3 blend = abs(normal);
    blend /= max(blend.x + blend.y + blend.z, 1e-5);

    float bump_strength = clamp(strength, 0.0, 1.0) * FLECK_MAX_NORMAL_STRENGTH;

    float h_c = fleck_pattern_value(tri_p, pattern_family, blend);
    float h_x = fleck_pattern_value(tri_p + vec3(FLECK_BUMP_EPSILON, 0.0, 0.0), pattern_family, blend);
    float h_y = fleck_pattern_value(tri_p + vec3(0.0, FLECK_BUMP_EPSILON, 0.0), pattern_family, blend);
    float h_z = fleck_pattern_value(tri_p + vec3(0.0, 0.0, FLECK_BUMP_EPSILON), pattern_family, blend);

    // Plane facing +/-X (varies y, z), facing +/-Y (varies x, z), facing
    // +/-Z (varies x, y) -- same per-axis convention apply_triplanar_bump()
    // itself follows.
    vec3 bump_x_plane = fleck_bump_from_heights(h_c, h_y, h_z, bump_strength);
    vec3 bump_y_plane = fleck_bump_from_heights(h_c, h_x, h_z, bump_strength);
    vec3 bump_z_plane = fleck_bump_from_heights(h_c, h_x, h_y, bump_strength);

    return fleck_apply_triplanar(normal, blend, bump_x_plane, bump_y_plane, bump_z_plane);
}

// --- Public entry point 2: albedo modulation ------------------------------

// The darkest a Fleck is ever allowed to push the diffuse tint, reached at
// strength = 1 right in the middle of the pattern's "on" region. M6's
// original ceiling was 0.7 (at most 30% darkening) -- direct user feedback
// was that the result was too subtle to read as a distinct pattern at all,
// let alone distinguish 16 of them from each other. 0.22 (nearly 80%
// darkening at full strength) makes the pattern geometry itself unmissable;
// tuned by actually looking at renders (see this milestone's own
// verification pass), not picked blind.
const float FLECK_MIN_BRIGHTNESS = 0.22;

// Returns a MULTIPLIER vec3 for the diffuse tint (not a scalar, as M6
// originally had it -- see below for why) -- a single direct 3D sample at
// world_pos/scale for every preset except Triangle/Hexagon, which need
// `normal` purely to rebuild the same triplanar blend weights fleck_
// perturb_normal() already derives from it (see fleck_cellular_value()'s
// own comment); every other preset's field is already seamless and stable
// under camera movement by construction (a genuine 3D volumetric sample,
// not a screen-space or even a 2D-per-plane one).
//
// Returns vec3 instead of M6's original float specifically so a Fleck can
// apply a small per-family COLOUR bias on top of plain darkening, not just
// darkening -- direct user feedback was that uniform darkening alone makes
// every preset look like "a dimmer version of the same colour". This is
// NOT a hue rotation of the base material colour (a material can be any
// colour, and still should look like that colour) -- it's a fixed, small
// multiplier (`bias` below) applied only in proportion to how "on" the
// pattern is at this pixel, so a white material's Square vs. its Baroque
// still differ by a believable material cast (ink/mortar-grey for
// cellular facets, sepia/charcoal for directional strokes, warm umber for
// organic painterly texture), not just by how dark the dark parts are.
// The call site (`tint *= fleck_modulate_albedo(...)`) did not need to
// change for this -- GLSL's vec3 *= vec3 already does the right thing.
vec3 fleck_modulate_albedo(vec3 world_pos, vec3 normal, int pattern_family,
                           float scale, float strength) {
    if (strength <= 0.0 || pattern_family < 0) {
        return vec3(1.0);
    }

    float safe_scale = max(scale, 0.05);
    vec3 blend = abs(normal);
    blend /= max(blend.x + blend.y + blend.z, 1e-5);

    float v = clamp(fleck_pattern_value(world_pos / safe_scale, pattern_family, blend), 0.0, 1.0);
    float s = clamp(strength, 0.0, 1.0);

    // Clay (17) is the one preset whose field is a literal height map (see
    // fleck_clay_value()'s own comment) rather than a "feature presence"
    // mask -- v is HIGH on the undisturbed flat clay and LOW at the centre
    // of each press-mark, the opposite sense every other preset's v has
    // (high v = "the feature is here", be it a raised blob, a stroke
    // crest, or a ridge). Darken by how DENTED a point is (1 - v), or the
    // flat background would darken instead of the dimples.
    float pattern_amount = (pattern_family > 16) ? (1.0 - v) : v;
    float darken = s * pattern_amount;

    vec3 bias;
    float min_brightness = FLECK_MIN_BRIGHTNESS;
    if (pattern_family <= 4) {
        bias = vec3(0.80, 0.86, 1.02); // cellular: cool grey-blue (grout/ink)
    } else if (pattern_family <= 8) {
        bias = vec3(1.04, 0.90, 0.74); // directional: warm sepia (pen/charcoal)
    } else if (pattern_family <= 15) {
        bias = vec3(1.0, 0.80, 0.58); // organic: warm umber (oil paint)
    } else if (pattern_family <= 16) {
        // Impasto: a more saturated, richer warm cast than organic's umber
        // (thick loaded oil paint pooling in the shadow trough of each
        // ridge reads as denser/more pigmented than a thin painted stroke)
        // -- distinct from organic's bias, not a copy of it.
        bias = vec3(1.08, 0.70, 0.42);
    } else {
        // Clay (17): deliberately RESTRAINED relative to every preset
        // above, per the brief -- real damp clay barely varies in colour
        // at all; what has to sell "clay" is the moulded RELIEF (the
        // concave dimples fleck_perturb_normal() produces, shaded by the
        // actual lighting model) not a strongly darkened/tinted pattern
        // stamped on top of it. A near-neutral, very faintly cool-grey
        // cast (unfired/damp clay reads a touch cooler and greyer in its
        // shadowed dents, not warmer like pigment pooling) combined with a
        // much shallower minimum brightness (0.84 vs. the generic 0.22 --
        // at most ~16% darkening here, not ~80%) keeps this a subtle
        // assist to the real shading instead of a second, competing
        // pattern fighting it for attention.
        bias = vec3(0.96, 0.97, 1.0);
        min_brightness = 0.84;
    }

    float brightness = mix(1.0, min_brightness, darken);
    vec3 colour_mult = mix(vec3(1.0), bias, darken);
    return brightness * colour_mult;
}
