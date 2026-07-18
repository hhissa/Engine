#version 450
#extension GL_GOOGLE_include_directive : require

// Pass 1: evaluates the analytic scene SDF once per coarse cell and bakes
// it into a sparse voxel field. A coarse cell only gets a "brick" of fine
// SDF samples allocated if the surface might actually pass through it --
// most cells (deep inside or far outside every shape) get no brick at all,
// which is what makes this sparse rather than a dense grid.
//
// The scene SDF is built from layers (see GeometrySystem::SceneLayer,
// engine-side, and the .sdf scene file format): every primitive in every
// layer is folded into the running scene, in layer order, using *that
// primitive's layer's* operation (union or subtraction) and smoothness
// (0 = hard edge, > 0 = a smooth/rounded blend of that radius) -- a layer
// can hold many primitives, and the operation applies to all of them, not
// just once per layer. This is what makes a subtraction layer only ever
// cut using shapes on its own layer -- see map() below.
//
// This bakes every primitive currently registered with GeometrySystem,
// minus whichever one VulkanRaymarchShader has chosen to animate live every
// frame instead (see PrimitiveBuffer below) -- this pass only re-runs when
// that registered set actually changes, not every frame, so a moving
// primitive can't be baked here.
//
// One invocation per coarse cell (COARSE_DIM^3 total).
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

// COARSE_DIM/BOUNDS scaled up together (16/2.0 -> 128/16.0, 8x) so
// COARSE_CELL_SIZE -- and therefore voxel resolution -- is unchanged from
// before; only the world volume actually baked grows. Must match
// Builtin.RaymarchShader.comp.glsl and kCoarseDim in
// vulkan_raymarch_shader.cpp exactly.
const int COARSE_DIM = 128;
const int BRICK_DIM = 8;
// Each brick stores a 1-voxel apron on every side, evaluated directly from
// the SDF at each apron voxel's true world position (not copied from a
// neighbor). Two adjacent bricks independently evaluating the same
// analytic function at the same shared boundary position always agree, so
// this gives seamless trilinear sampling across brick boundaries without
// any cross-brick communication.
const int BRICK_APRON_DIM = BRICK_DIM + 2;
const int BRICK_VOXEL_COUNT = BRICK_APRON_DIM * BRICK_APRON_DIM * BRICK_APRON_DIM;
// Must match kMaxBricks in vulkan_raymarch_shader.cpp (which owns the
// sizing rationale). Cells past this cap silently get no brick, which
// renders as nondeterministic missing/stray chunks of surface -- exactly
// what happened when this was still 2048 (sized for the old 16^3 grid and
// never scaled with the 8x COARSE_DIM bump above).
const int MAX_BRICKS = 262144;
const float BOUNDS = 16.0;
const float COARSE_CELL_SIZE = (2.0 * BOUNDS) / float(COARSE_DIM);

layout(binding = 0) buffer IndirectionBuffer {
    int indirection[COARSE_DIM * COARSE_DIM * COARSE_DIM];
};

layout(binding = 1) buffer BrickPoolBuffer {
    float bricks[];
};

layout(binding = 2) buffer BrickCounterBuffer {
    uint brick_counter;
};

// The analytic scene SDF itself (primitive/layer/param-expr buffers, every
// per-type SDF function, the smooth combine ops, and scene_map()) lives in
// the shared include below -- the render pass evaluates the exact same
// code per-pixel for material provenance, and sharing one copy is what
// keeps the two from drifting apart.
#define SDF_PRIMITIVE_BUFFER_BINDING 3
#define SDF_LAYER_BUFFER_BINDING 5
#define SDF_PARAM_EXPR_BUFFER_BINDING 6
#include "Builtin.SdfSceneCommon.inc.glsl"

// Which primitive ended up nearest at each brick's center -- baked here so
// the render pass can pick a material for a hit without re-evaluating every
// primitive itself (the baked field only stores a distance, not
// provenance).
layout(binding = 4) buffer BrickPrimitiveBuffer {
    int brick_primitive[];
};

layout(push_constant) uniform PushConstants {
    int layer_count;
} push;


void main() {
    ivec3 cell = ivec3(gl_GlobalInvocationID);
    if (cell.x >= COARSE_DIM || cell.y >= COARSE_DIM || cell.z >= COARSE_DIM) {
        return;
    }

    int cell_index = cell.x + cell.y * COARSE_DIM + cell.z * COARSE_DIM * COARSE_DIM;

    vec3 cell_min = vec3(-BOUNDS) + vec3(cell) * COARSE_CELL_SIZE;
    vec3 cell_center = cell_min + vec3(COARSE_CELL_SIZE * 0.5);

    int nearest_primitive;
    float center_dist = scene_map(cell_center, push.layer_count, nearest_primitive);

    // Conservative test: the surface might cross this cell if the SDF at
    // its center is within the cell's half-diagonal (sqrt(3)/2 * size) of
    // zero. Cells further from zero than that cannot contain the surface,
    // and get no brick.
    float half_diagonal = COARSE_CELL_SIZE * 0.8660254;

    indirection[cell_index] = -1;

    if (nearest_primitive >= 0 && abs(center_dist) <= half_diagonal) {
        uint brick_index = atomicAdd(brick_counter, 1u);
        if (brick_index < uint(MAX_BRICKS)) {
            indirection[cell_index] = int(brick_index);
            brick_primitive[int(brick_index)] = nearest_primitive;

            float voxel_size = COARSE_CELL_SIZE / float(BRICK_DIM);
            // Local index range is -1..BRICK_DIM inclusive (the apron),
            // mapped to storage index range 0..BRICK_APRON_DIM-1 via +1.
            for (int vz = -1; vz <= BRICK_DIM; ++vz) {
                for (int vy = -1; vy <= BRICK_DIM; ++vy) {
                    for (int vx = -1; vx <= BRICK_DIM; ++vx) {
                        vec3 voxel_pos = cell_min + (vec3(vx, vy, vz) + 0.5) * voxel_size;
                        int unused_nearest;
                        float d = scene_map(voxel_pos, push.layer_count, unused_nearest);
                        ivec3 store = ivec3(vx, vy, vz) + ivec3(1);
                        int local_index = store.x + store.y * BRICK_APRON_DIM + store.z * BRICK_APRON_DIM * BRICK_APRON_DIM;
                        bricks[int(brick_index) * BRICK_VOXEL_COUNT + local_index] = d;
                    }
                }
            }
        }
    }
}
