#version 450

// Pass 1: evaluates the analytic SDF once per coarse cell and bakes it into
// a sparse voxel field. A coarse cell only gets a "brick" of fine SDF
// samples allocated if the surface might actually pass through it -- most
// cells (deep inside or far outside the shape) get no brick at all, which
// is what makes this sparse rather than a dense grid.
//
// One invocation per coarse cell (COARSE_DIM^3 total).
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

const int COARSE_DIM = 16;
const int BRICK_DIM = 8;
// Each brick stores a 1-voxel apron on every side, evaluated directly from
// the SDF at each apron voxel's true world position (not copied from a
// neighbor). Two adjacent bricks independently evaluating the same
// analytic function at the same shared boundary position always agree, so
// this gives seamless trilinear sampling across brick boundaries without
// any cross-brick communication.
const int BRICK_APRON_DIM = BRICK_DIM + 2;
const int BRICK_VOXEL_COUNT = BRICK_APRON_DIM * BRICK_APRON_DIM * BRICK_APRON_DIM;
const int MAX_BRICKS = 2048;
const float BOUNDS = 2.0;
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

float sphere_sdf(vec3 p, float radius) {
    return length(p) - radius;
}

float map(vec3 p) {
    return sphere_sdf(p, 1.0);
}

void main() {
    ivec3 cell = ivec3(gl_GlobalInvocationID);
    if (cell.x >= COARSE_DIM || cell.y >= COARSE_DIM || cell.z >= COARSE_DIM) {
        return;
    }

    int cell_index = cell.x + cell.y * COARSE_DIM + cell.z * COARSE_DIM * COARSE_DIM;

    vec3 cell_min = vec3(-BOUNDS) + vec3(cell) * COARSE_CELL_SIZE;
    vec3 cell_center = cell_min + vec3(COARSE_CELL_SIZE * 0.5);

    float center_dist = map(cell_center);

    // Conservative test: the surface might cross this cell if the SDF at
    // its center is within the cell's half-diagonal (sqrt(3)/2 * size) of
    // zero. Cells further from zero than that cannot contain the surface,
    // and get no brick.
    float half_diagonal = COARSE_CELL_SIZE * 0.8660254;

    indirection[cell_index] = -1;

    if (abs(center_dist) <= half_diagonal) {
        uint brick_index = atomicAdd(brick_counter, 1u);
        if (brick_index < uint(MAX_BRICKS)) {
            indirection[cell_index] = int(brick_index);

            float voxel_size = COARSE_CELL_SIZE / float(BRICK_DIM);
            // Local index range is -1..BRICK_DIM inclusive (the apron),
            // mapped to storage index range 0..BRICK_APRON_DIM-1 via +1.
            for (int vz = -1; vz <= BRICK_DIM; ++vz) {
                for (int vy = -1; vy <= BRICK_DIM; ++vy) {
                    for (int vx = -1; vx <= BRICK_DIM; ++vx) {
                        vec3 voxel_pos = cell_min + (vec3(vx, vy, vz) + 0.5) * voxel_size;
                        float d = map(voxel_pos);
                        ivec3 store = ivec3(vx, vy, vz) + ivec3(1);
                        int local_index = store.x + store.y * BRICK_APRON_DIM + store.z * BRICK_APRON_DIM * BRICK_APRON_DIM;
                        bricks[int(brick_index) * BRICK_VOXEL_COUNT + local_index] = d;
                    }
                }
            }
        }
    }
}
