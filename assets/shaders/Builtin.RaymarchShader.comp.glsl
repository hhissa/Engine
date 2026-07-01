#version 450

// Pass 2 (repeating): marches a ray per pixel against the sparse voxel
// field baked by Builtin.RaymarchVoxelize.comp.glsl, instead of evaluating
// the SDF directly. Coarse cells with no brick are skipped across in one
// step; bricked cells are sampled with trilinear interpolation.
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 0, rgba8) uniform writeonly image2D out_image;

const int COARSE_DIM = 16;
const int BRICK_DIM = 8;
// Matches the voxelize shader's 1-voxel-per-side apron: storage indices run
// over BRICK_APRON_DIM (not BRICK_DIM), so sampling can blend across brick
// boundaries instead of clamping at them.
const int BRICK_APRON_DIM = BRICK_DIM + 2;
const int BRICK_VOXEL_COUNT = BRICK_APRON_DIM * BRICK_APRON_DIM * BRICK_APRON_DIM;
const float BOUNDS = 2.0;
const float COARSE_CELL_SIZE = (2.0 * BOUNDS) / float(COARSE_DIM);

layout(binding = 1) readonly buffer IndirectionBuffer {
    int indirection[COARSE_DIM * COARSE_DIM * COARSE_DIM];
};

layout(binding = 2) readonly buffer BrickPoolBuffer {
    float bricks[];
};

// Triplanar-mapped surface texture: the sphere has no UV coordinates (it's
// an implicit surface, not a mesh), so instead of a single 2D unwrap this is
// sampled 3 times (once per axis-aligned world plane) and blended by how
// much the surface normal faces each axis.
layout(binding = 3) uniform sampler2D surface_tex;
const float TEXTURE_SCALE = 0.6; // world units per texture tile

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
    vec4 model_offset;    // xyz + unused pad; see main() for how this moves
                          // the object without touching the baked field.
} push;

const int MAX_STEPS = 128;
const float MAX_DIST = 8.0;
const float SURF_DIST = 0.001;

// Looks up the field at p. If the containing coarse cell has no brick,
// returns skip_dist > 0 (safe to step forward by that much, since no
// surface exists anywhere in that whole cell). Otherwise returns the
// trilinearly-interpolated fine distance in `dist` and skip_dist == 0.
void sample_field(vec3 p, vec3 ray_dir, out float dist, out float skip_dist) {
    vec3 local = (p + vec3(BOUNDS)) / COARSE_CELL_SIZE;
    ivec3 cell = ivec3(floor(local));

    if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(COARSE_DIM)))) {
        dist = MAX_DIST;
        skip_dist = COARSE_CELL_SIZE;
        return;
    }

    int cell_index = cell.x + cell.y * COARSE_DIM + cell.z * COARSE_DIM * COARSE_DIM;
    int brick_index = indirection[cell_index];

    if (brick_index < 0) {
        // No brick: no surface anywhere in this whole cell. Step exactly to
        // this cell's exit boundary along ray_dir (a standard ray-box slab
        // exit distance), not a flat COARSE_CELL_SIZE -- a fixed step can
        // otherwise jump clean over an adjacent bricked cell whenever the
        // ray travels at an oblique angle to the grid, since a fixed-length
        // step along a diagonal advances an uneven amount per axis
        // depending on where within the cell it started.
        vec3 cell_min = vec3(-BOUNDS) + vec3(cell) * COARSE_CELL_SIZE;
        vec3 cell_max = cell_min + vec3(COARSE_CELL_SIZE);
        vec3 boundary = mix(cell_min, cell_max, greaterThan(ray_dir, vec3(0.0)));
        // Axes the ray isn't moving along (ray_dir component == 0, exactly
        // true for the screen's whole center column/row where uv is 0 on
        // that axis) must never constrain the exit distance. Left as a
        // literal division, (boundary - p) / 0.0 is +-infinity depending on
        // sign -- and unlike a true "no constraint" (+infinity, correctly
        // ignored by min()), the -infinity case poisons min() into always
        // returning -infinity, collapsing the step size to ~0 for that
        // whole column/row. Explicitly force those axes to a large
        // sentinel instead of trusting the division's sign.
        bvec3 stationary = lessThan(abs(ray_dir), vec3(1e-8));
        vec3 safe_ray_dir = mix(ray_dir, vec3(1.0), stationary);
        vec3 t_exit = mix((boundary - p) / safe_ray_dir, vec3(1e30), stationary);
        float exit_dist = min(min(t_exit.x, t_exit.y), t_exit.z);

        dist = COARSE_CELL_SIZE;
        skip_dist = max(exit_dist, SURF_DIST) + 0.0001;
        return;
    }

    skip_dist = 0.0;

    vec3 cell_min = vec3(-BOUNDS) + vec3(cell) * COARSE_CELL_SIZE;
    float voxel_size = COARSE_CELL_SIZE / float(BRICK_DIM);
    vec3 f = (p - cell_min) / voxel_size - 0.5;
    // Local voxel index range is -1..BRICK_DIM inclusive (the apron).
    ivec3 i0 = clamp(ivec3(floor(f)), ivec3(-1), ivec3(BRICK_DIM));
    ivec3 i1 = clamp(i0 + ivec3(1), ivec3(-1), ivec3(BRICK_DIM));
    vec3 t = clamp(f - vec3(i0), 0.0, 1.0);

    // Map local voxel indices (-1..BRICK_DIM) to storage indices (0..BRICK_APRON_DIM-1).
    ivec3 s0 = i0 + ivec3(1);
    ivec3 s1 = i1 + ivec3(1);

    int base = brick_index * BRICK_VOXEL_COUNT;
    float c000 = bricks[base + s0.x + s0.y * BRICK_APRON_DIM + s0.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c100 = bricks[base + s1.x + s0.y * BRICK_APRON_DIM + s0.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c010 = bricks[base + s0.x + s1.y * BRICK_APRON_DIM + s0.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c110 = bricks[base + s1.x + s1.y * BRICK_APRON_DIM + s0.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c001 = bricks[base + s0.x + s0.y * BRICK_APRON_DIM + s1.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c101 = bricks[base + s1.x + s0.y * BRICK_APRON_DIM + s1.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c011 = bricks[base + s0.x + s1.y * BRICK_APRON_DIM + s1.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c111 = bricks[base + s1.x + s1.y * BRICK_APRON_DIM + s1.z * BRICK_APRON_DIM * BRICK_APRON_DIM];

    float c00 = mix(c000, c100, t.x);
    float c10 = mix(c010, c110, t.x);
    float c01 = mix(c001, c101, t.x);
    float c11 = mix(c011, c111, t.x);

    float c0 = mix(c00, c10, t.y);
    float c1 = mix(c01, c11, t.y);

    dist = mix(c0, c1, t.z);
}

vec3 calc_normal(vec3 p) {
    // Offsets are tiny (0.0025) relative to a coarse cell (0.25), so these
    // queries essentially always land in the same bricked cell as p itself
    // -- the no-brick/skip_dist branch (which needs a real ray direction)
    // shouldn't trigger here, so any placeholder direction is fine.
    vec3 dummy_dir = vec3(0.0, 0.0, 1.0);
    vec2 e = vec2(0.0025, 0.0);
    float dx0, dx1, dy0, dy1, dz0, dz1, skip;
    sample_field(p + e.xyy, dummy_dir, dx1, skip);
    sample_field(p - e.xyy, dummy_dir, dx0, skip);
    sample_field(p + e.yxy, dummy_dir, dy1, skip);
    sample_field(p - e.yxy, dummy_dir, dy0, skip);
    sample_field(p + e.yyx, dummy_dir, dz1, skip);
    sample_field(p - e.yyx, dummy_dir, dz0, skip);
    return normalize(vec3(dx1 - dx0, dy1 - dy0, dz1 - dz0));
}

float raymarch(vec3 ray_origin, vec3 ray_dir) {
    float travelled = 0.0;
    for (int i = 0; i < MAX_STEPS; ++i) {
        vec3 p = ray_origin + ray_dir * travelled;
        float d, skip_dist;
        sample_field(p, ray_dir, d, skip_dist);

        if (skip_dist > 0.0) {
            // No brick here: no surface anywhere in this whole coarse
            // cell, so it's safe to skip straight across it.
            travelled += skip_dist;
        } else {
            if (abs(d) < SURF_DIST) {
                break;
            }
            travelled += max(abs(d), SURF_DIST);
        }

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

    // Transform the ray into object-local space by subtracting the model
    // offset, rather than transforming the (baked, origin-centered) field
    // itself -- this moves the visible object without re-voxelizing.
    // Equivalent to a rigid translation of the object in world space; a
    // rotation would need the ray *direction* transformed too, but the
    // object here is a sphere, for which self-rotation is invisible anyway.
    vec3 ray_origin = push.camera_position.xyz - push.model_offset.xyz;
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

    float travelled = raymarch(ray_origin, ray_dir);

    // Background gradient.
    vec3 colour = mix(vec3(0.02, 0.02, 0.05), vec3(0.05, 0.05, 0.12), uv.y + 0.5);

    if (travelled < MAX_DIST) {
        vec3 p = ray_origin + ray_dir * travelled;
        vec3 normal = calc_normal(p);
        vec3 light_dir = normalize(vec3(0.6, 0.7, -0.6));
        float diffuse = max(dot(normal, light_dir), 0.0);
        float ambient = 0.15;

        // Triplanar sample: project p onto each of the 3 axis planes, then
        // blend by how much the normal faces that axis (a normal facing
        // mostly +/-X should be dominated by the YZ-plane projection, since
        // that's the plane you'd actually be looking at face-on there).
        vec3 tri_p = p / TEXTURE_SCALE;
        vec3 blend = abs(normal);
        blend /= (blend.x + blend.y + blend.z);
        vec3 tex_colour = texture(surface_tex, tri_p.yz).rgb * blend.x +
                          texture(surface_tex, tri_p.xz).rgb * blend.y +
                          texture(surface_tex, tri_p.xy).rgb * blend.z;

        colour = tex_colour * (diffuse * 0.85 + ambient);
    }

    imageStore(out_image, pixel_coord, vec4(colour, 1.0));
}
