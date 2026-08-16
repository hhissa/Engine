// Shared query against the chunked field Builtin.ChunkVoxelize.comp.glsl
// bakes -- the chunked-field counterpart of Builtin.BakedFieldCommon.inc.
// glsl, kept as a fully separate file/function (not a mode switch inside
// that one) so this newer, less-battle-tested addressing scheme can never
// affect the existing fixed-cube field sdf_editor/games depend on -- see
// Builtin.SdfFieldConfig.inc.glsl's comment on why the two fields don't
// share any GPU buffer.
//
// The including shader must, BEFORE this #include:
//   1. #define these buffer binding slots (they differ per pipeline):
//        CHUNKED_FIELD_TABLE_BINDING
//        CHUNKED_FIELD_INDIRECTION_BINDING
//        CHUNKED_FIELD_BRICKPOOL_BINDING
//        CHUNKED_FIELD_BRICKPRIMITIVE_BINDING
//   2. Already have CHUNK_COARSE_DIM/CHUNK_CELL_COUNT/CHUNK_WORLD_SIZE/
//      CHUNK_TABLE_DIM/NUM_CHUNK_LEVELS/STREAM_RADIUS_CHUNKS/
//      chunk_level_world_size()/BRICK_DIM/BRICK_APRON_DIM/
//      BRICK_VOXEL_COUNT/COARSE_CELL_SIZE/MAX_DIST/SURF_DIST in scope
//      (Builtin.SdfFieldConfig.inc.glsl covers everything but MAX_DIST/
//      SURF_DIST, which -- like Builtin.BakedFieldCommon.inc.glsl -- stay
//      declared per-including-shader).
//
// Table layout is per-level: level L's CHUNK_TABLE_DIM^3 entries occupy
// global table indices [L*CHUNK_TABLE_DIM^3, (L+1)*CHUNK_TABLE_DIM^3) --
// must match VulkanRaymarchShader::voxelize_chunk()/evict_chunk()'s
// identical offset exactly (the ONE thing about level partitioning this
// file needs to know; the indirection/brick buffers below need no
// per-level offset at all, since a table lookup always yields a GLOBAL
// slot already unique across every level -- see chunk_table_buffer_'s own
// comment engine-side).

// NUM_CHUNK_LEVELS * CHUNK_TABLE_DIM^3 slot-index entries total,
// CPU-owned and CPU-written directly (host-visible/coherent -- see
// VulkanRaymarchShader's chunk_table_buffer_) every time chunk residency
// changes; this shader only ever reads it. Entry value is -1 (no chunk
// resident at this wrapped coordinate right now) or a valid GLOBAL index
// into ChunkIndirectionBuffer's per-chunk sub-blocks below.
layout(binding = CHUNKED_FIELD_TABLE_BINDING) readonly buffer ChunkTableBuffer {
    int chunk_table[];
};

layout(binding = CHUNKED_FIELD_INDIRECTION_BINDING) readonly buffer ChunkIndirectionBuffer {
    int chunk_indirection[];
};

layout(binding = CHUNKED_FIELD_BRICKPOOL_BINDING) readonly buffer ChunkBrickPoolBuffer {
    float chunk_bricks[];
};

layout(binding = CHUNKED_FIELD_BRICKPRIMITIVE_BINDING) readonly buffer ChunkBrickPrimitiveBuffer {
    int chunk_brick_primitive[];
};

// floor-based modulo for ivec3, result always in [0, m) per component --
// GLSL's `%` (and C's) takes the dividend's sign instead, which would give
// a negative result for a negative chunk coordinate and break the table
// index below. GLSL has no built-in integer mod() (only the float-genType
// overload), hence this.
ivec3 floor_mod3(ivec3 v, int m) {
    return ((v % m) + m) % m;
}

// Looks up the chunked field at p (render space -- see Builtin.
// SdfFieldConfig.inc.glsl's comment on chunk_world_min) within ONE
// specific level. Same dist/skip_dist/material_index contract as Builtin.
// BakedFieldCommon.inc.glsl's sample_field(): skip_dist > 0 +
// material_index == -1 means "safe to step forward by skip_dist, nothing
// here"; skip_dist == 0 means dist is a real trilinearly-interpolated
// surface distance.
//
// p always resolves to exactly one level-`level` chunk coordinate; if
// that coordinate's wrapped table slot doesn't currently hold it (either
// truly empty, or wrapped around onto a DIFFERENT chunk's slot -- see
// ChunkStreamingManager's ring-delay for why that can briefly happen),
// this treats it as empty space, identically to "no brick" below. Callers
// (sample_chunked_field()/sample_clipmap_field() below) are responsible
// for only calling this with a level whose own streaming window actually
// contains p -- this function has no way to check that itself.
void sample_chunked_field_at_level(vec3 p, vec3 ray_dir, int level, out float dist,
                                   out float skip_dist, out int material_index) {
    float chunk_world_size = chunk_level_world_size(level);
    float cell_size = chunk_world_size / float(CHUNK_COARSE_DIM);

    ivec3 chunk_coord = ivec3(floor(p / chunk_world_size));
    vec3 chunk_world_min = vec3(chunk_coord) * chunk_world_size;

    ivec3 wrapped = floor_mod3(chunk_coord, CHUNK_TABLE_DIM);
    int table_index = level * CHUNK_TABLE_DIM * CHUNK_TABLE_DIM * CHUNK_TABLE_DIM +
        wrapped.x + wrapped.y * CHUNK_TABLE_DIM + wrapped.z * CHUNK_TABLE_DIM * CHUNK_TABLE_DIM;
    int slot = chunk_table[table_index];

    vec3 local = (p - chunk_world_min) / cell_size;
    ivec3 cell = ivec3(floor(local));
    // Clamp defensively -- p is guaranteed inside chunk_coord's own span by
    // construction (chunk_coord was derived from p above), so cell should
    // always land in [0, CHUNK_COARSE_DIM), but floating-point rounding
    // right at a chunk boundary could in principle push it one past; this
    // avoids reading outside this chunk's own CHUNK_CELL_COUNT sub-block if
    // that ever happens, rather than corrupting a neighboring chunk's data.
    cell = clamp(cell, ivec3(0), ivec3(CHUNK_COARSE_DIM - 1));

    if (slot < 0) {
        // No chunk resident here at all -- step to this level's cell's
        // exit boundary, identical technique to Builtin.BakedFieldCommon.
        // inc.glsl's own "no brick" case (see its comment for why a
        // ray-box slab exit, not a flat step, is needed).
        vec3 cell_min = chunk_world_min + vec3(cell) * cell_size;
        vec3 cell_max = cell_min + vec3(cell_size);
        vec3 boundary = mix(cell_min, cell_max, greaterThan(ray_dir, vec3(0.0)));
        bvec3 stationary = lessThan(abs(ray_dir), vec3(1e-8));
        vec3 safe_ray_dir = mix(ray_dir, vec3(1.0), stationary);
        vec3 t_exit = mix((boundary - p) / safe_ray_dir, vec3(1e30), stationary);
        float exit_dist = min(min(t_exit.x, t_exit.y), t_exit.z);

        dist = MAX_DIST;
        skip_dist = max(exit_dist, SURF_DIST) + 0.0001;
        material_index = -1;
        return;
    }

    int local_cell_index = cell.x + cell.y * CHUNK_COARSE_DIM +
        cell.z * CHUNK_COARSE_DIM * CHUNK_COARSE_DIM;
    int brick_index = chunk_indirection[slot * CHUNK_CELL_COUNT + local_cell_index];

    if (brick_index < 0) {
        vec3 cell_min = chunk_world_min + vec3(cell) * cell_size;
        vec3 cell_max = cell_min + vec3(cell_size);
        vec3 boundary = mix(cell_min, cell_max, greaterThan(ray_dir, vec3(0.0)));
        bvec3 stationary = lessThan(abs(ray_dir), vec3(1e-8));
        vec3 safe_ray_dir = mix(ray_dir, vec3(1.0), stationary);
        vec3 t_exit = mix((boundary - p) / safe_ray_dir, vec3(1e30), stationary);
        float exit_dist = min(min(t_exit.x, t_exit.y), t_exit.z);

        dist = cell_size;
        skip_dist = max(exit_dist, SURF_DIST) + 0.0001;
        material_index = -1;
        return;
    }

    skip_dist = 0.0;
    material_index = chunk_brick_primitive[brick_index];

    vec3 cell_min = chunk_world_min + vec3(cell) * cell_size;
    float voxel_size = cell_size / float(BRICK_DIM);
    vec3 f = (p - cell_min) / voxel_size - 0.5;
    ivec3 i0 = clamp(ivec3(floor(f)), ivec3(-1), ivec3(BRICK_DIM));
    ivec3 i1 = clamp(i0 + ivec3(1), ivec3(-1), ivec3(BRICK_DIM));
    vec3 t = clamp(f - vec3(i0), 0.0, 1.0);

    ivec3 s0 = i0 + ivec3(1);
    ivec3 s1 = i1 + ivec3(1);

    int base = brick_index * BRICK_VOXEL_COUNT;
    float c000 = chunk_bricks[base + s0.x + s0.y * BRICK_APRON_DIM + s0.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c100 = chunk_bricks[base + s1.x + s0.y * BRICK_APRON_DIM + s0.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c010 = chunk_bricks[base + s0.x + s1.y * BRICK_APRON_DIM + s0.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c110 = chunk_bricks[base + s1.x + s1.y * BRICK_APRON_DIM + s0.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c001 = chunk_bricks[base + s0.x + s0.y * BRICK_APRON_DIM + s1.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c101 = chunk_bricks[base + s1.x + s0.y * BRICK_APRON_DIM + s1.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c011 = chunk_bricks[base + s0.x + s1.y * BRICK_APRON_DIM + s1.z * BRICK_APRON_DIM * BRICK_APRON_DIM];
    float c111 = chunk_bricks[base + s1.x + s1.y * BRICK_APRON_DIM + s1.z * BRICK_APRON_DIM * BRICK_APRON_DIM];

    float c00 = mix(c000, c100, t.x);
    float c10 = mix(c010, c110, t.x);
    float c01 = mix(c001, c101, t.x);
    float c11 = mix(c011, c111, t.x);

    float c0 = mix(c00, c10, t.y);
    float c1 = mix(c01, c11, t.y);

    dist = mix(c0, c1, t.z);
}

// Level-0-only convenience wrapper -- kept for Builtin.ChunkedFieldDebugQuery.
// comp.glsl's existing (Phase 3a/3b) single-level query harness, whose
// verification numbers were captured against exactly this call shape; see
// sample_clipmap_field() below for the real multi-level entry point.
void sample_chunked_field(vec3 p, vec3 ray_dir, out float dist, out float skip_dist,
                          out int material_index) {
    sample_chunked_field_at_level(p, ray_dir, 0, dist, skip_dist, material_index);
}

// Picks the finest chunk level whose streaming window (see
// ChunkStreamingManager, engine-side) actually contains p, and samples
// that level's chunked field there -- the real multi-level entry point a
// caller (Builtin.RaymarchShader.comp.glsl, once chunked_field_enabled_ is
// set -- see VulkanRaymarchShader::set_chunked_field_enabled()) should
// use. camera_pos must be in the same render space as p (see
// VulkanRendererBackend::world_origin_offset_) -- level containment is
// judged relative to the camera's own position, exactly mirroring
// ChunkStreamingManager::update()'s identical Chebyshev-window test
// engine-side (both MUST agree, or this could pick a level
// ChunkStreamingManager never actually keeps loaded there).
//
// Hard level selection, not a blended cross-fade -- a query exactly at a
// level boundary can show a visible seam (a small dist/lighting
// discontinuity between two independently-baked levels' samples).
// Deliberately simple-first, matching the plan's own stated scope: a
// seamless multi-resolution blend (smoothly mixing two
// sample_chunked_field_at_level() calls near every level transition) is
// real additional work with no correctness payoff by itself -- worth
// adding once this hard-cut version is confirmed otherwise correct, not
// before.
void sample_clipmap_field(vec3 p, vec3 ray_dir, vec3 camera_pos, out float dist,
                          out float skip_dist, out int material_index) {
    for (int level = 0; level < NUM_CHUNK_LEVELS; ++level) {
        float level_world_size = chunk_level_world_size(level);
        ivec3 chunk_coord = ivec3(floor(p / level_world_size));
        ivec3 camera_chunk = ivec3(floor(camera_pos / level_world_size));
        ivec3 delta = abs(chunk_coord - camera_chunk);
        bool within_window = delta.x <= STREAM_RADIUS_CHUNKS &&
            delta.y <= STREAM_RADIUS_CHUNKS && delta.z <= STREAM_RADIUS_CHUNKS;
        if (within_window) {
            sample_chunked_field_at_level(p, ray_dir, level, dist, skip_dist,
                                          material_index);
            return;
        }
    }

    // Outside every level's window -- past the total coverage NUM_CHUNK_
    // LEVELS/STREAM_RADIUS_CHUNKS currently implies. Treat as empty space,
    // same contract as any other "nothing here" case above.
    dist = MAX_DIST;
    skip_dist = chunk_level_world_size(NUM_CHUNK_LEVELS - 1);
    material_index = -1;
}

// Soft shadow ray march against the CHUNKED field -- a deliberate near-
// duplicate of Builtin.BakedFieldCommon.inc.glsl's shadow_march() (see its
// own extensive comment for the soft-shadow technique/parameter meanings,
// identical here), calling sample_clipmap_field() instead of sample_field()
// so a caller that's opted into the chunked/streamed field (see
// VulkanRaymarchShader::set_chunked_field_enabled()) gets correctly shadowed
// results too, not just a correct primary hit-test. Duplicated rather than
// having shadow_march() itself branch: that function lives in a file shared
// with Builtin.ProbeBake.comp.glsl, which has no awareness of the chunked
// field at all (and shouldn't yet -- a later phase's GI cascades are what
// actually needs GI to read this field) -- see Builtin.SdfFieldConfig.inc.
// glsl's comment on why the two fields stay fully separate.
float chunked_shadow_march(vec3 origin, vec3 dir, vec3 camera_pos, float max_dist,
                           float k, int max_steps, int exclude_material) {
    const float SELF_SKIP_STEP = 0.1;

    float shadow = 1.0;
    float travelled = 0.0;
    for (int i = 0; i < max_steps; ++i) {
        vec3 p = origin + dir * travelled;

        float dist, skip_dist;
        int material;
        sample_clipmap_field(p, dir, camera_pos, dist, skip_dist, material);
        bool valid = (skip_dist == 0.0);

        if (valid && abs(dist) < SURF_DIST) {
            if (material == exclude_material) {
                travelled += SELF_SKIP_STEP;
                continue;
            }
            return 0.0;
        }

        float step = valid ? abs(dist) : skip_dist;
        if (valid) {
            shadow = min(shadow, k * step / max(travelled, SURF_DIST));
        }
        travelled += max(step, SURF_DIST);

        if (travelled > max_dist || shadow < 0.005) {
            break;
        }
    }
    return clamp(shadow, 0.0, 1.0);
}
