#version 450
#extension GL_GOOGLE_include_directive : require

// Pass 2 (repeating): marches a ray per pixel against the sparse voxel
// field baked by Builtin.RaymarchVoxelize.comp.glsl (every static
// primitive currently registered with GeometrySystem). Coarse cells with
// no static brick are skipped across in one step; bricked cells are
// sampled with trilinear interpolation.
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 0, rgba8) uniform writeonly image2D out_image;

// COARSE_DIM/BOUNDS scaled up together (16/2.0 -> 128/16.0, 8x) so
// COARSE_CELL_SIZE -- and therefore voxel resolution -- is unchanged from
// before; only the world volume actually baked grows. Must match
// Builtin.RaymarchVoxelize.comp.glsl and kCoarseDim in
// vulkan_raymarch_shader.cpp exactly.
const int COARSE_DIM = 128;
const int BRICK_DIM = 8;
// Matches the voxelize shader's 1-voxel-per-side apron: storage indices run
// over BRICK_APRON_DIM (not BRICK_DIM), so sampling can blend across brick
// boundaries instead of clamping at them.
const int BRICK_APRON_DIM = BRICK_DIM + 2;
const int BRICK_VOXEL_COUNT = BRICK_APRON_DIM * BRICK_APRON_DIM * BRICK_APRON_DIM;
const float BOUNDS = 16.0;
const float COARSE_CELL_SIZE = (2.0 * BOUNDS) / float(COARSE_DIM);

// One entry per registered light (see GeometrySystem::Light,
// engine-side). vector_type.xyz is a direction (Directional) or
// world-space position (Point), vector_type.w its LightType as a float
// (0=directional, 1=point); colour_intensity.rgb/.a are the light's colour
// and intensity -- see the lighting loop in main() below for exactly how
// each type is evaluated. Read push.light_count of these, not lights.length()
// -- the buffer is sized to a fixed capacity (see kMaxLights engine-side),
// not to how many are actually registered.
struct Light {
    vec4 vector_type;
    vec4 colour_intensity;
};

layout(binding = 3) readonly buffer LightBuffer {
    Light lights[];
};

// rgb: the primitive's material diffuse tint. a: its material's
// texture_scale (world units per texture tile -- see Material::
// texture_scale engine-side), NOT colour opacity; packed here because the
// alpha slot was unused and it saves a whole separate buffer + binding.
layout(binding = 5) readonly buffer ScenePrimitiveColours {
    vec4 scene_diffuse_colours[];
};

// Fixed-size, unlike the buffers above -- GLSL requires a compile-time
// array length for opaque handles like combined image samplers. Unused
// slots are always bound to a valid filler texture (engine-side) since
// Vulkan requires every element of a bound sampler array to reference a
// real image regardless of whether the shader ever reads that index.
const int MAX_SCENE_PRIMITIVES = 1000;
layout(binding = 6) uniform sampler2D scene_textures[MAX_SCENE_PRIMITIVES];

// The analytic scene SDF (shared with the voxelize pass -- one copy, so
// the baked field and this pass's per-pixel material provenance can never
// disagree about what the scene is). Only scene_map()'s nearest_primitive
// out-param is used here: the *distance* still comes from the baked field
// (that's the whole point of baking), but the *material* at a hit is
// re-derived analytically at the exact hit point, because the per-brick
// index above quantizes provenance to whole 0.25-unit cells.
#define SDF_PRIMITIVE_BUFFER_BINDING 7
#define SDF_LAYER_BUFFER_BINDING 8
#define SDF_PARAM_EXPR_BUFFER_BINDING 9
#include "Builtin.SdfSceneCommon.inc.glsl"

// Per-frame camera/model state, uploaded by VulkanRaymarchShader::render_to()
// each dispatch. A push constant rather than a UBO/descriptor binding: it's
// recorded directly into the command buffer, so there's no separate buffer
// memory a previous, still in-flight frame could be reading while this
// frame's value gets written -- no per-swapchain-image duplication needed.
layout(push_constant) uniform PushConstants {
    vec4 camera_position; // xyz + unused pad
    vec4 camera_forward;  // xyz + unused pad -- orthonormal basis (Camera
    vec4 camera_right;    // xyz + unused pad    class, engine-side) for
    vec4 camera_up;       // xyz + unused pad    building the view-space ray.
    int light_count;    // How many of LightBuffer's entries to sum, below.
    float ambient;        // Scene-wide ambient factor, added once regardless
                         // of light_count -- see GeometrySystem::ambient().
    int selected_primitive_index; // scene_textures/scene_diffuse_colours
                                  // index to draw a selection outline
                                  // around, or -1 for none.
    int grid_enabled;             // Nonzero to composite the reference grid
                                  // (see apply_reference_grid() below) over
                                  // this frame.
    int layer_count;              // How many LayerBuffer entries scene_map()
                                  // folds -- same value the voxelize pass
                                  // baked with.
} push;

// MAX_STEPS must be large enough to guarantee a ray can actually reach
// MAX_DIST, not just bound the loop -- outside the scene's baked bounds
// (see BOUNDS/sample_field()'s out-of-range branch above), a miss ray only
// ever advances by a fixed COARSE_CELL_SIZE per step, so crossing MAX_DIST
// world units of empty space alone costs MAX_DIST/COARSE_CELL_SIZE steps.
// Falling short of that would let a distant miss ray exhaust MAX_STEPS
// while travelled is still < MAX_DIST, which main() reads as a false hit
// (hit_material left at -1) instead of background. The remaining ~128-step
// margin beyond that (640 - 128.0/COARSE_CELL_SIZE) is for genuine hits
// needing several small steps to converge near a real surface.
const int MAX_STEPS = 640;
const float MAX_DIST = 128.0;
const float SURF_DIST = 0.001;

// sample_field() (the baked-field query) plus its indirection/brick-pool/
// brick-primitive bindings live in the shared include below -- also used
// by Builtin.ProbeBake.comp.glsl's GI gather rays, which must march
// against the exact same baked field this pass does.
#define BAKED_FIELD_INDIRECTION_BINDING 1
#define BAKED_FIELD_BRICKPOOL_BINDING 2
#define BAKED_FIELD_BRICKPRIMITIVE_BINDING 4
#include "Builtin.BakedFieldCommon.inc.glsl"

// Baked indirect-light probe grid -- a dense regular grid of GI samples
// covering the same world volume as the voxel field (BOUNDS), computed by
// Builtin.ProbeBake.comp.glsl once per rebake (see
// VulkanRaymarchShader::bake_probes()) and read here every frame,
// trilinearly interpolated at each hit point (see sample_probe_grid()
// below). This is what stands in for the flat `push.ambient` scalar the
// render pass used to add everywhere: instead every point in the scene
// gets its own approximation of the light actually bouncing around it --
// darker in corners a probe's gather rays mostly found nearby geometry,
// brighter in open rooms, tinted by whatever coloured surfaces are nearby
// (colour bleeding) -- adapted from Inigo Quilez's "simplegi" technique
// (https://iquilezles.org/articles/simplegi/): that article bakes GI onto
// *mesh vertices*, gathering each one's incoming light via a rendered
// hemisphere and iterating a few bounces; this engine has no meshes
// anywhere in its SDF/voxel pipeline, so the same idea -- treat a handful
// of sample points as light emitters/receivers, gather their incoming
// light by "rendering" (raymarching) a sphere of directions from each,
// iterate a few bounces -- is retargeted onto a spatial probe grid
// instead of vertices, the natural fit for a volume that has no surface
// mesh to hang samples off of.
const int PROBE_DIM = 16; // must match kProbeDim in vulkan_raymarch_shader.cpp
layout(binding = 10) readonly buffer ProbeBuffer {
    vec4 probes[]; // rgb = baked indirect irradiance at this probe; see
                   // Builtin.ProbeBake.comp.glsl for how it's computed.
};

// Trilinearly samples the baked probe grid at world point p -- the
// render-time counterpart to the GI bake, reading exactly the regular
// PROBE_DIM^3 grid Builtin.ProbeBake.comp.glsl wrote, covering the same
// [-BOUNDS, BOUNDS] cube the voxel field does. Unlike sample_field()'s
// brick apron there's no sparsity to route around here -- probes exist
// everywhere in the grid, so this is a plain 8-corner trilinear fetch, no
// indirection layer needed. Deliberately ignores the surface normal at p
// (a probe stores one isotropic average over its whole surrounding
// sphere, not a directional/hemispherical value) -- a further
// simplification on top of an already-approximate technique, in the same
// spirit as the source article's own acknowledged shortcut of not
// cosine-weighting each gathered sample by the angle it arrived from.
vec3 sample_probe_grid(vec3 p) {
    float cell_size = (2.0 * BOUNDS) / float(PROBE_DIM - 1);
    vec3 local = (p + vec3(BOUNDS)) / cell_size;
    vec3 local_clamped = clamp(local, vec3(0.0), vec3(float(PROBE_DIM - 1)));
    ivec3 i0 = ivec3(floor(local_clamped));
    ivec3 i1 = min(i0 + ivec3(1), ivec3(PROBE_DIM - 1));
    vec3 t = local_clamped - vec3(i0);

    #define PROBE_AT(ix, iy, iz) probes[(ix) + (iy) * PROBE_DIM + (iz) * PROBE_DIM * PROBE_DIM].rgb

    vec3 c000 = PROBE_AT(i0.x, i0.y, i0.z);
    vec3 c100 = PROBE_AT(i1.x, i0.y, i0.z);
    vec3 c010 = PROBE_AT(i0.x, i1.y, i0.z);
    vec3 c110 = PROBE_AT(i1.x, i1.y, i0.z);
    vec3 c001 = PROBE_AT(i0.x, i0.y, i1.z);
    vec3 c101 = PROBE_AT(i1.x, i0.y, i1.z);
    vec3 c011 = PROBE_AT(i0.x, i1.y, i1.z);
    vec3 c111 = PROBE_AT(i1.x, i1.y, i1.z);

    #undef PROBE_AT

    vec3 c00 = mix(c000, c100, t.x);
    vec3 c10 = mix(c010, c110, t.x);
    vec3 c01 = mix(c001, c101, t.x);
    vec3 c11 = mix(c011, c111, t.x);

    vec3 c0 = mix(c00, c10, t.y);
    vec3 c1 = mix(c01, c11, t.y);

    return mix(c0, c1, t.z);
}

// Normal of the baked static field at p via finite differences. Offsets
// are tiny (0.0025) relative to a coarse cell (0.25), so these queries
// essentially always land in the same bricked cell as p itself -- the
// no-brick/skip_dist branch (which needs a real ray direction) shouldn't
// trigger here, so any placeholder direction is fine.
vec3 calc_static_normal(vec3 p) {
    vec3 dummy_dir = vec3(0.0, 0.0, 1.0);
    vec2 e = vec2(0.0025, 0.0);
    float dx0, dx1, dy0, dy1, dz0, dz1, skip;
    int unused_material;
    sample_field(p + e.xyy, dummy_dir, dx1, skip, unused_material);
    sample_field(p - e.xyy, dummy_dir, dx0, skip, unused_material);
    sample_field(p + e.yxy, dummy_dir, dy1, skip, unused_material);
    sample_field(p - e.yxy, dummy_dir, dy0, skip, unused_material);
    sample_field(p + e.yyx, dummy_dir, dz1, skip, unused_material);
    sample_field(p - e.yyx, dummy_dir, dz0, skip, unused_material);
    return normalize(vec3(dx1 - dx0, dy1 - dy0, dz1 - dz0));
}

// Loops over every scene_textures slot instead of indexing directly by
// `index`: Vulkan only guarantees well-defined results when a combined
// image sampler array is indexed by something "dynamically uniform" (the
// same value across the whole dispatch), and a per-pixel hit's material
// index is anything but. The loop counter itself *is* dynamically uniform
// (every invocation executes the same sequence of i values in lockstep),
// so indexing by it here is legal without needing descriptor-indexing
// device features.
vec3 sample_scene_texture(int index, vec2 uv) {
    vec3 result = vec3(0.0);
    for (int i = 0; i < MAX_SCENE_PRIMITIVES; ++i) {
        if (i == index) {
            result = texture(scene_textures[i], uv).rgb;
        }
    }
    return result;
}

// Raymarch analog of kohi's "051 Normal Maps" commit: perturbs the shaded
// normal with fine surface detail. Kohi's version samples a dedicated
// tangent-space normal-map texture unwrapped onto a mesh's authored UVs;
// SDF primitives here have neither a mesh nor authored UVs (see the
// triplanar diffuse sampling above for why colour is already triplanar), so
// there's no "normal_texture" asset to sample instead. This derives the
// same kind of detail directly from the existing diffuse texture's own
// luminance -- a standard technique ("bump mapping from a height/luminance
// field") for when no explicit normal map exists -- via a 3-tap finite
// difference per axis-plane, then recombines the three tangent-space bumps
// into the base normal using the "whiteout blend" construction (Golus,
// "Normal Mapping for a Triplanar Shader"), reusing the same per-axis
// `blend` weights as the diffuse triplanar mix above.
const float BUMP_UV_EPSILON = 0.015;
const float BUMP_STRENGTH = 0.25;

float luminance(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// Converts 3 nearby luminance samples (centre, +epsilon along each local
// axis) into a tangent-space bump normal: (dHeight/du, dHeight/dv, 1),
// negated and scaled so brighter neighbours slope the normal away from
// them, then normalized -- the same convention an authored tangent-space
// normal-map texture's RGB channels would encode.
vec3 bump_from_heights(float h_centre, float h_u, float h_v) {
    float du = (h_u - h_centre) / BUMP_UV_EPSILON;
    float dv = (h_v - h_centre) / BUMP_UV_EPSILON;
    return normalize(vec3(-du * BUMP_STRENGTH, -dv * BUMP_STRENGTH, 1.0));
}

void sample_scene_heights(int index, vec2 uv, out float h_centre, out float h_u, out float h_v) {
    h_centre = 0.0;
    h_u = 0.0;
    h_v = 0.0;
    for (int i = 0; i < MAX_SCENE_PRIMITIVES; ++i) {
        if (i == index) {
            h_centre = luminance(texture(scene_textures[i], uv).rgb);
            h_u = luminance(texture(scene_textures[i], uv + vec2(BUMP_UV_EPSILON, 0.0)).rgb);
            h_v = luminance(texture(scene_textures[i], uv + vec2(0.0, BUMP_UV_EPSILON)).rgb);
        }
    }
}

// Whiteout-blend recombination: each axis-plane's tangent-space bump is
// added into the two components of the base normal it doesn't itself
// represent, swizzled back into world space, then blended by the same
// per-axis weights used for triplanar colour.
vec3 apply_triplanar_bump(vec3 base_normal, vec3 blend, vec3 bump_x, vec3 bump_y, vec3 bump_z) {
    vec3 nx = vec3(bump_x.xy + base_normal.zy, base_normal.x);
    vec3 ny = vec3(bump_y.xy + base_normal.xz, base_normal.y);
    vec3 nz = vec3(bump_z.xy + base_normal.xy, base_normal.z);

    return normalize(nx.zyx * blend.x + ny.xzy * blend.y + nz.xyz * blend.z);
}

// Reference grid: the ground plane y=0 subdivided into 1-unit cells, with a
// heavier line every GRID_MAJOR_SPACING units and the world X/Z axes tinted
// (the usual modelling-package look). The plane is intersected analytically
// per-ray rather than baked into the voxel field: it's an infinite editor
// aid, not scene content, so it must never show up in the SDF field the
// authoring tools measure against -- and an analytic hit costs nothing while
// a baked plane would burn bricks across the whole volume.
const float GRID_MINOR_SPACING = 1.0;
const float GRID_MAJOR_SPACING = 10.0;
// Distance (world units, along the ray) over which the grid fades out.
// Without a fade the 1-unit lines alias into shimmer long before MAX_DIST.
const float GRID_FADE_DIST = 60.0;

const vec3 GRID_MINOR_COLOUR = vec3(0.32, 0.32, 0.35);
const vec3 GRID_MAJOR_COLOUR = vec3(0.5, 0.5, 0.55);
const vec3 GRID_X_AXIS_COLOUR = vec3(0.85, 0.3, 0.3);  // the z=0 line
const vec3 GRID_Z_AXIS_COLOUR = vec3(0.3, 0.45, 0.9);  // the x=0 line

// Anti-aliased coverage of the line family {coord = n * spacing}: distance
// from coord to the nearest line, pushed through a smoothstep whose ramp is
// one filter_width wide. filter_width is the ~1-pixel world-space footprint
// at the hit point (computed in apply_reference_grid() -- no fwidth() in a
// compute shader), so lines stay about a pixel wide on screen at any
// distance instead of a fixed world width that aliases far away and looks
// bloated up close.
float grid_line_coverage(float coord, float spacing, float filter_width) {
    float dist_to_line = abs(fract(coord / spacing + 0.5) - 0.5) * spacing;
    return 1.0 - smoothstep(0.5 * filter_width, 1.5 * filter_width, dist_to_line);
}

// Composites the reference grid into colour (in place) for this pixel's
// ray. scene_dist is how far the ray travelled before hitting scene content
// (>= MAX_DIST on a miss): geometry in front of the plane occludes the grid,
// while the grid draws over the background and over anything behind it.
void apply_reference_grid(inout vec3 colour, vec3 ray_origin, vec3 ray_dir,
                          float scene_dist, float pixel_height) {
    if (abs(ray_dir.y) < 1e-6) {
        return; // Ray parallel to the plane -- no intersection.
    }
    float plane_t = -ray_origin.y / ray_dir.y;
    if (plane_t <= 0.0 || plane_t >= min(scene_dist, GRID_FADE_DIST)) {
        return;
    }

    vec3 hit = ray_origin + ray_dir * plane_t;

    // Approximate world-space size of one pixel on the plane at the hit
    // point: angular pixel size scaled by distance, stretched by how
    // obliquely the ray strikes the plane (grazing rays smear a pixel
    // across far more of the plane). The 0.05 clamp caps that stretch near
    // the horizon, where the fade below takes over anyway.
    float filter_width = plane_t * pixel_height / max(abs(ray_dir.y), 0.05);

    float minor = max(grid_line_coverage(hit.x, GRID_MINOR_SPACING, filter_width),
                      grid_line_coverage(hit.z, GRID_MINOR_SPACING, filter_width));
    float major = max(grid_line_coverage(hit.x, GRID_MAJOR_SPACING, filter_width),
                      grid_line_coverage(hit.z, GRID_MAJOR_SPACING, filter_width));
    // The X axis is the line z=0 (and vice versa), so each axis' coverage
    // comes from the *other* coordinate's distance to 0. Slightly wider
    // than the regular lines so the axes read at a glance.
    float axis_x = 1.0 - smoothstep(0.75 * filter_width, 2.0 * filter_width, abs(hit.z));
    float axis_z = 1.0 - smoothstep(0.75 * filter_width, 2.0 * filter_width, abs(hit.x));

    // Layered: axis colour wins over major, major over minor; opacity is
    // the strongest layer present. The plane between lines stays fully
    // transparent -- it's a reference grid, not a floor.
    vec3 grid_colour = GRID_MINOR_COLOUR;
    float grid_alpha = minor * 0.5;
    grid_colour = mix(grid_colour, GRID_MAJOR_COLOUR, major);
    grid_alpha = max(grid_alpha, major * 0.75);
    grid_colour = mix(grid_colour, GRID_X_AXIS_COLOUR, axis_x);
    grid_colour = mix(grid_colour, GRID_Z_AXIS_COLOUR, axis_z);
    grid_alpha = max(grid_alpha, max(axis_x, axis_z) * 0.9);

    float fade = 1.0 - smoothstep(0.5 * GRID_FADE_DIST, GRID_FADE_DIST, plane_t);
    colour = mix(colour, grid_colour, grid_alpha * fade);
}

// Marches the ray against the baked static field. hit_material receives
// the winning surface's material index once something is hit (always >=0
// -- an index into scene_textures/scene_diffuse_colours).
float raymarch(vec3 ray_origin, vec3 ray_dir, out int hit_material) {
    float travelled = 0.0;
    hit_material = -1;
    for (int i = 0; i < MAX_STEPS; ++i) {
        vec3 p = ray_origin + ray_dir * travelled;

        float static_dist, skip_dist;
        int static_material;
        sample_field(p, ray_dir, static_dist, skip_dist, static_material);
        bool static_valid = (skip_dist == 0.0);

        if (static_valid && abs(static_dist) < SURF_DIST) {
            hit_material = static_material;
            break;
        }

        float step = static_valid ? abs(static_dist) : skip_dist;
        travelled += max(step, SURF_DIST);

        if (travelled > MAX_DIST) {
            break;
        }
    }
    return travelled;
}

void main() {
    ivec2 pixel_coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 image_size = imageSize(out_image);
    if (pixel_coord.x >= image_size.x || pixel_coord.y >= image_size.y) {
        return;
    }

    vec2 uv = (vec2(pixel_coord) - 0.5 * vec2(image_size)) / float(image_size.y);

    // Fully world-space: static registered primitives already carry their
    // own world position, baked directly into the field.
    vec3 ray_origin = push.camera_position.xyz;
    // Build the ray in world space from the camera's basis instead of the
    // fixed world axes: uv.x moves along camera_right, uv.y along
    // camera_up, and the base direction is camera_forward -- this is what
    // lets the camera look around instead of always facing +Z. At the
    // identity orientation (yaw = pitch = 0) the basis is exactly
    // (right=+X, up=+Y, forward=+Z), so this reduces to the old
    // normalize(vec3(uv, 1.0)) in that case.
    vec3 ray_dir = normalize(uv.x * push.camera_right.xyz +
                            uv.y * push.camera_up.xyz +
                            push.camera_forward.xyz);

    int hit_material;
    float travelled = raymarch(ray_origin, ray_dir, hit_material);

    // Background gradient.
    vec3 colour = mix(vec3(0.02, 0.02, 0.05), vec3(0.05, 0.05, 0.12), uv.y + 0.5);

    if (travelled < MAX_DIST) {
        vec3 p = ray_origin + ray_dir * travelled;
        vec3 normal = calc_static_normal(p);

        // Re-derive the material at the exact hit point from the analytic
        // scene, replacing the baked per-brick guess raymarch() returned --
        // the brick index quantizes material provenance to whole coarse
        // cells, which bleeds each primitive's texture up to a full
        // 0.25-unit cell onto whatever it touches. Falls back to the brick
        // material only if the analytic scene is somehow empty here (e.g.
        // a stale field mid-edit).
        int analytic_material;
        scene_map(p, push.layer_count, analytic_material);
        if (analytic_material >= 0) {
            hit_material = analytic_material;
        }

        // World units per texture tile, from this hit's material (packed
        // in the colour's alpha slot -- see ScenePrimitiveColours above).
        // The clamp only guards a degenerate authored scale of ~0 turning
        // the division below into inf/NaN UVs.
        float texture_scale = max(scene_diffuse_colours[hit_material].a, 0.01);

        // Triplanar sample: project p onto each of the 3 axis planes, then
        // blend by how much the normal faces that axis (a normal facing
        // mostly +/-X should be dominated by the YZ-plane projection, since
        // that's the plane you'd actually be looking at face-on there).
        vec3 tri_p = p / texture_scale;
        vec3 blend = abs(normal);
        blend /= (blend.x + blend.y + blend.z);

        vec3 tex_colour = sample_scene_texture(hit_material, tri_p.yz) * blend.x +
                          sample_scene_texture(hit_material, tri_p.xz) * blend.y +
                          sample_scene_texture(hit_material, tri_p.xy) * blend.z;
        vec3 tint = scene_diffuse_colours[hit_material].rgb;

        float h_centre, h_u, h_v;
        sample_scene_heights(hit_material, tri_p.yz, h_centre, h_u, h_v);
        vec3 bump_x = bump_from_heights(h_centre, h_u, h_v);
        sample_scene_heights(hit_material, tri_p.xz, h_centre, h_u, h_v);
        vec3 bump_y = bump_from_heights(h_centre, h_u, h_v);
        sample_scene_heights(hit_material, tri_p.xy, h_centre, h_u, h_v);
        vec3 bump_z = bump_from_heights(h_centre, h_u, h_v);

        normal = apply_triplanar_bump(normal, blend, bump_x, bump_y, bump_z);

        // Sum every registered light's direct diffuse contribution on top
        // of this point's baked indirect/bounce light (see
        // sample_probe_grid() above) -- the GI probe grid fully replaces
        // the old flat `push.ambient` scalar (push.ambient still matters,
        // just indirectly now: it's what a probe's gather rays pick up
        // when they escape the scene entirely -- see Builtin.ProbeBake.
        // comp.glsl -- rather than being re-added flatly at every pixel
        // here, which would double-count it).
        vec3 lighting = sample_probe_grid(p);
        for (int i = 0; i < push.light_count; ++i) {
            Light light = lights[i];
            int light_type = int(light.vector_type.w);
            vec3 light_colour = light.colour_intensity.rgb;
            float intensity = light.colour_intensity.a;

            vec3 light_dir;
            float attenuation;
            if (light_type == 1) {
                // Point: direction from the surface to the light, with
                // inverse-square falloff (intensity == brightness at 1
                // world unit away).
                vec3 to_light = light.vector_type.xyz - p;
                float dist = length(to_light);
                light_dir = to_light / max(dist, 0.0001);
                attenuation = intensity / max(dist * dist, 0.0001);
            } else {
                // Directional: shines uniformly from this direction, no
                // falloff.
                light_dir = normalize(light.vector_type.xyz);
                attenuation = intensity;
            }

            float diffuse = max(dot(normal, light_dir), 0.0);
            lighting += light_colour * diffuse * attenuation;
        }

        colour = tex_colour * tint * lighting;

        // Selection outline: a rim-light glow, brightest where the surface
        // grazes away from the camera (silhouette edges) and fading toward
        // the center of the shape when viewed face-on -- the standard "glow
        // outline" look editors use to show what's selected, without
        // needing a second geometry pass or edge-detection post-process.
        if (hit_material == push.selected_primitive_index) {
            float facing = max(dot(normal, -ray_dir), 0.0);
            float rim = pow(1.0 - facing, 3.0);
            const vec3 HIGHLIGHT_COLOUR = vec3(1.0, 0.55, 0.1);
            colour += HIGHLIGHT_COLOUR * rim * 1.5;
        }
    }

    if (push.grid_enabled != 0) {
        // travelled is >= MAX_DIST on a miss, which apply_reference_grid()
        // reads as "nothing occludes the plane". pixel_height matches the
        // uv mapping above: one pixel subtends 1/image_size.y of the ray
        // basis regardless of aspect.
        apply_reference_grid(colour, ray_origin, ray_dir, travelled,
                             1.0 / float(image_size.y));
    }

    imageStore(out_image, pixel_coord, vec4(colour, 1.0));
}
