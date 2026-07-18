// Shared query against the sparse voxel field Builtin.RaymarchVoxelize.
// comp.glsl bakes -- included by BOTH Builtin.RaymarchShader.comp.glsl (the
// primary per-pixel raymarch) and Builtin.ProbeBake.comp.glsl (gather rays
// for the light-probe GI bake), which both need to march against the exact
// same baked field and must never drift apart on how they read it.
//
// The including shader must, BEFORE this #include:
//   1. #define these buffer binding slots (they differ per pipeline):
//        BAKED_FIELD_INDIRECTION_BINDING
//        BAKED_FIELD_BRICKPOOL_BINDING
//        BAKED_FIELD_BRICKPRIMITIVE_BINDING
//   2. Already have COARSE_DIM/BRICK_DIM/BRICK_APRON_DIM/BRICK_VOXEL_COUNT/
//      BOUNDS/COARSE_CELL_SIZE/MAX_DIST/SURF_DIST in scope -- these stay
//      duplicated per-shader (like they already were between the voxelize
//      and render passes before this file existed) rather than factored
//      here, so this file only has to assume they exist under these names.

layout(binding = BAKED_FIELD_INDIRECTION_BINDING) readonly buffer IndirectionBuffer {
    int indirection[COARSE_DIM * COARSE_DIM * COARSE_DIM];
};

layout(binding = BAKED_FIELD_BRICKPOOL_BINDING) readonly buffer BrickPoolBuffer {
    float bricks[];
};

// Which primitive is nearest at each brick, baked by the voxelize pass --
// lets a query here pick a material for a hit without re-evaluating the
// analytic scene itself.
layout(binding = BAKED_FIELD_BRICKPRIMITIVE_BINDING) readonly buffer BrickPrimitiveBuffer {
    int brick_primitive[];
};

// Looks up the baked static field at p. If the containing coarse cell has
// no brick, returns skip_dist > 0 (safe to step forward by that much,
// since no static surface exists anywhere in that whole cell) and
// material_index == -1. Otherwise returns the trilinearly-interpolated
// fine distance in `dist`, skip_dist == 0, and the brick's baked material
// index.
void sample_field(vec3 p, vec3 ray_dir, out float dist, out float skip_dist, out int material_index) {
    vec3 local = (p + vec3(BOUNDS)) / COARSE_CELL_SIZE;
    ivec3 cell = ivec3(floor(local));

    if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(COARSE_DIM)))) {
        dist = MAX_DIST;
        skip_dist = COARSE_CELL_SIZE;
        material_index = -1;
        return;
    }

    int cell_index = cell.x + cell.y * COARSE_DIM + cell.z * COARSE_DIM * COARSE_DIM;
    int brick_index = indirection[cell_index];

    if (brick_index < 0) {
        // No brick: no static surface anywhere in this whole cell. Step
        // exactly to this cell's exit boundary along ray_dir (a standard
        // ray-box slab exit distance), not a flat COARSE_CELL_SIZE -- a
        // fixed step can otherwise jump clean over an adjacent bricked cell
        // whenever the ray travels at an oblique angle to the grid, since a
        // fixed-length step along a diagonal advances an uneven amount per
        // axis depending on where within the cell it started.
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
        material_index = -1;
        return;
    }

    skip_dist = 0.0;
    material_index = brick_primitive[brick_index];

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
