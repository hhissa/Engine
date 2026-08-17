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
//      chunk_level_world_size()/CHUNK_BRICK_DIM/CHUNK_BRICK_APRON_DIM/
//      CHUNK_BRICK_VOXEL_COUNT/COARSE_CELL_SIZE/MAX_DIST/SURF_DIST in scope
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
        // No chunk resident here at all -- step to the WHOLE CHUNK's exit
        // boundary, not just this one cell's: slot < 0 means chunk_
        // indirection has no sub-block allocated for this coordinate at
        // all, so every one of this chunk's CHUNK_COARSE_DIM cells is
        // equally non-resident, not just the one p happens to be in.
        // Stepping cell-by-cell here (the old behaviour) cost up to
        // CHUNK_COARSE_DIM (16) redundant sample_chunked_field_at_level()
        // calls to cross one empty chunk, since every one of those calls
        // re-resolves chunk_coord back to this same non-resident chunk.
        // Same ray-box slab exit technique as Builtin.BakedFieldCommon.
        // inc.glsl's own "no brick" case (see its comment for why a slab
        // exit, not a flat step, is needed) -- just sized to the chunk
        // instead of the cell. Contrast with the brick_index < 0 branch
        // below, which MUST stay cell-bounded: there, only THIS cell has
        // been confirmed brickless -- sibling cells in the same (resident)
        // chunk can still hold real bricks.
        vec3 chunk_world_max = chunk_world_min + vec3(chunk_world_size);
        vec3 boundary = mix(chunk_world_min, chunk_world_max, greaterThan(ray_dir, vec3(0.0)));
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

    // CHUNK_BRICK_DIM, not BRICK_DIM -- must match Builtin.ChunkVoxelize.
    // comp.glsl's population loop exactly (see CHUNK_BRICK_DIM's own
    // comment, Builtin.SdfFieldConfig.inc.glsl, for why the chunked field
    // uses a separate, finer brick resolution than the fixed-cube field).
    vec3 cell_min = chunk_world_min + vec3(cell) * cell_size;
    float voxel_size = cell_size / float(CHUNK_BRICK_DIM);
    vec3 f = (p - cell_min) / voxel_size - 0.5;
    ivec3 i0 = clamp(ivec3(floor(f)), ivec3(-1), ivec3(CHUNK_BRICK_DIM));
    ivec3 i1 = clamp(i0 + ivec3(1), ivec3(-1), ivec3(CHUNK_BRICK_DIM));
    vec3 t = clamp(f - vec3(i0), 0.0, 1.0);

    ivec3 s0 = i0 + ivec3(1);
    ivec3 s1 = i1 + ivec3(1);

    int base = brick_index * CHUNK_BRICK_VOXEL_COUNT;
    float c000 = chunk_bricks[base + s0.x + s0.y * CHUNK_BRICK_APRON_DIM + s0.z * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM];
    float c100 = chunk_bricks[base + s1.x + s0.y * CHUNK_BRICK_APRON_DIM + s0.z * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM];
    float c010 = chunk_bricks[base + s0.x + s1.y * CHUNK_BRICK_APRON_DIM + s0.z * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM];
    float c110 = chunk_bricks[base + s1.x + s1.y * CHUNK_BRICK_APRON_DIM + s0.z * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM];
    float c001 = chunk_bricks[base + s0.x + s0.y * CHUNK_BRICK_APRON_DIM + s1.z * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM];
    float c101 = chunk_bricks[base + s1.x + s0.y * CHUNK_BRICK_APRON_DIM + s1.z * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM];
    float c011 = chunk_bricks[base + s0.x + s1.y * CHUNK_BRICK_APRON_DIM + s1.z * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM];
    float c111 = chunk_bricks[base + s1.x + s1.y * CHUNK_BRICK_APRON_DIM + s1.z * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM];

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
// How much of the outermost chunk's own width (in each level's window,
// see below) is spent cross-fading with the next, coarser level, as a
// fraction of that chunk's half-width -- 0 disables blending entirely
// (the original hard cutoff), 1 would blend across the entire outermost
// chunk.
//
// Defaulted back to 0 (disabled) after a live editor test surfaced visible
// banding/corruption across a domain-repeated ceiling with fine (zigzag/
// hex) detail narrower than the coarser level's own voxel size: level+1's
// voxels are simply too coarse to represent that detail at all, so its
// "nearest primitive" bookkeeping can pick a wrong/inconsistent primitive
// there, and mixing that into the blend doesn't read as a smooth
// transition -- it reads as the fine detail getting washed out/corrupted
// across the whole outer ring of chunks the repeated pattern spans. That's
// a fundamentally different failure than the skip_dist==min() sparse-
// voxelization bug this file's sample_clipmap_field() comment describes
// (already fixed, independent of this value) -- this one isn't fixable by
// correctness alone, since it's the coarser level's own resolution that's
// insufficient, not a bug in how it's sampled. The hard cutoff (this
// engine's original, thoroughly live-tested Phase 4 behavior) still pops
// at a level boundary, but only there, and never corrupts a primitive's
// own detail the way blending toward an under-resolved level can. Raise
// this again only after confirming (visually, not just via buffer
// readback -- see debug_verify_lod_blend()'s own honest limits here) that
// it holds up against fine repeated detail, not just an isolated primitive
// sitting cleanly on one level.
const float LOD_BLEND_FRACTION = 0.0;

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
// Cross-fades with the next, coarser level over the outer LOD_BLEND_
// FRACTION of a level's OUTERMOST resident chunk (Chebyshev chunk-index
// distance from camera's own chunk exactly STREAM_RADIUS_CHUNKS -- the
// chunk that would fall outside this level's window if the camera stepped
// one chunk further), instead of hard-cutting to the coarser level's
// (independently baked, generally slightly different) sample right at the
// boundary -- that hard cut was a visible pop/seam as the camera moved.
// Deliberately scoped to just that one outermost ring, not a window-wide
// continuous distance: level L+1's chunk size is always exactly double
// level L's (see chunk_level_world_size()), so level L's entire window
// (radius STREAM_RADIUS_CHUNKS chunks, i.e. reaching at most (STREAM_
// RADIUS_CHUNKS+1)*level_world_size continuous units from camera_pos in
// the worst case, accounting for camera's own fractional offset within
// its chunk) sits comfortably inside level L+1's OWN window (reaching
// (STREAM_RADIUS_CHUNKS+1)*2*level_world_size) -- so level L+1 is
// guaranteed already resident everywhere this blend could possibly sample
// it, with no extra streaming radius needed.
void sample_clipmap_field(vec3 p, vec3 ray_dir, vec3 camera_pos, out float dist,
                          out float skip_dist, out int material_index) {
    for (int level = 0; level < NUM_CHUNK_LEVELS; ++level) {
        float level_world_size = chunk_level_world_size(level);
        ivec3 chunk_coord = ivec3(floor(p / level_world_size));
        ivec3 camera_chunk = ivec3(floor(camera_pos / level_world_size));
        ivec3 delta = chunk_coord - camera_chunk;
        ivec3 abs_delta = abs(delta);
        int chebyshev = max(abs_delta.x, max(abs_delta.y, abs_delta.z));
        if (chebyshev > STREAM_RADIUS_CHUNKS) {
            continue;
        }

        if (LOD_BLEND_FRACTION > 0.0 && chebyshev == STREAM_RADIUS_CHUNKS &&
            level + 1 < NUM_CHUNK_LEVELS) {
            // p's chunk is this level's outermost ring -- how far along the
            // axis (or axes) actually AT that ring's limit (abs_delta ==
            // chebyshev; an axis merely mid-chunk doesn't count, even if
            // it's numerically large on some other level) does p sit
            // toward that chunk's away-from-camera face. In [-1,1] within
            // the chunk along each qualifying axis; only the outward
            // portion (positive, in the direction delta already points)
            // matters for "about to cross out."
            vec3 chunk_center = (vec3(chunk_coord) + 0.5) * level_world_size;
            vec3 local = (p - chunk_center) / (level_world_size * 0.5);
            float outward = -1.0;
            if (abs_delta.x == chebyshev) {
                outward = max(outward, sign(float(delta.x)) * local.x);
            }
            if (abs_delta.y == chebyshev) {
                outward = max(outward, sign(float(delta.y)) * local.y);
            }
            if (abs_delta.z == chebyshev) {
                outward = max(outward, sign(float(delta.z)) * local.z);
            }
            float t = clamp((outward - (1.0 - LOD_BLEND_FRACTION)) / LOD_BLEND_FRACTION,
                            0.0, 1.0);
            if (t > 0.0) {
                float dist_a, skip_a, dist_b, skip_b;
                int material_a, material_b;
                sample_chunked_field_at_level(p, ray_dir, level, dist_a, skip_a,
                                              material_a);
                // skip_dist is a binary "trust dist" flag (0.0 = real baked
                // distance, nonzero = no brick here, dist is a placeholder --
                // see sample_chunked_field_at_level()'s own "no brick"
                // branches), NOT a magnitude to minimize. Voxelization is
                // sparse (only near-surface cells get bricks -- see this
                // field's own baking comment), so it's routine for one side
                // of a blend to have a real brick while the other doesn't;
                // only reach for level+1's sample (and only actually blend)
                // once level's own side has confirmed a real brick, and only
                // trust the blend if level+1's does too. Falling through to
                // level's own untouched single-level result otherwise avoids
                // ever mixing a real distance with a MAX_DIST-scale "no
                // brick" placeholder into something the raymarcher would
                // wrongly treat as trustworthy (skip_dist==0) -- that
                // corruption is what caused the banding/striping artifacts
                // near primitive edges this comment's fix resolved.
                if (skip_a == 0.0) {
                    sample_chunked_field_at_level(p, ray_dir, level + 1, dist_b,
                                                  skip_b, material_b);
                    if (skip_b == 0.0) {
                        dist = mix(dist_a, dist_b, t);
                        skip_dist = 0.0;
                        material_index = t < 0.5 ? material_a : material_b;
                        return;
                    }
                }
                dist = dist_a;
                skip_dist = skip_a;
                material_index = material_a;
                return;
            }
        }

        sample_chunked_field_at_level(p, ray_dir, level, dist, skip_dist,
                                      material_index);
        return;
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
