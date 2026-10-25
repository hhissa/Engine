// The DRAG INSTANCE: a sculpture being dragged, rotated or placed is built
// once into chunk slots of its own (not in the chunk table, so the world
// field no longer holds it) and drawn through a transform, so moving it
// re-sculpts nothing (VulkanRaymarchShader::begin_drag_instance()).
//
// Its bricks are in the sculpture's pose when the drag began ("local"
// space); the transform takes that pose to the current one, as a uniform
// scale, rotation and translation: world = M * local + t, M = s * R.
//
// The block is written by the CPU at the end of two buffers that the
// readers already bind: the chunk table (field lookups, the point splat)
// and the slot-published flags (the cluster cull). Its slot bitmap says
// which clusters are the instance's (a cluster's owner slot is in
// ChunkCluster.meta.w's high 16 bits).
//
// The includer defines SCULPT_INSTANCE_WORD(i): word i of the block as an
// int.
//
// Layout (32-bit words; floats as raw bits):
//   0      chunk count (0 = no instance)
//   1-3    first chunk (level-0 chunk coordinates, local space)
//   4-6    chunks per axis
//   8-16   M, column major
//   17-19  t
//   20-28  M^-1, column major
//   29-31  -M^-1 * t
//   32     s
//   34-36  world-space box min (the instance's whole chunk range, moved)
//   37-39  world-space box max
//   40-119 bitmap of the chunk slots that are the instance's (2560 slots)
//   120... chunk slot per chunk (x fastest), -1 = none
const int SCULPT_INSTANCE_SLOT_BITS_AT = 40;
const int SCULPT_INSTANCE_SLOT_BITS = 2560;
const int SCULPT_INSTANCE_SLOTS_AT = 120;
const int SCULPT_INSTANCE_MAX_CHUNKS = 4096;
const int SCULPT_INSTANCE_WORDS = SCULPT_INSTANCE_SLOTS_AT + SCULPT_INSTANCE_MAX_CHUNKS;

bool sculpt_instance_active() {
    return SCULPT_INSTANCE_WORD(0) > 0;
}

float sculpt_instance_float(int i) {
    return intBitsToFloat(SCULPT_INSTANCE_WORD(i));
}

vec3 sculpt_instance_vec3(int i) {
    return vec3(sculpt_instance_float(i), sculpt_instance_float(i + 1),
                sculpt_instance_float(i + 2));
}

mat3 sculpt_instance_mat3(int i) {
    return mat3(sculpt_instance_vec3(i), sculpt_instance_vec3(i + 3),
                sculpt_instance_vec3(i + 6));
}

// Whether a cluster (by its meta.w) is one of the instance's.
bool sculpt_instance_owns_cluster(uint meta_w) {
    if (!sculpt_instance_active()) {
        return false;
    }
    int slot = int(meta_w >> 16);
    if (slot >= SCULPT_INSTANCE_SLOT_BITS) {
        return false;
    }
    uint bits = uint(SCULPT_INSTANCE_WORD(SCULPT_INSTANCE_SLOT_BITS_AT + slot / 32));
    return (bits & (1u << uint(slot % 32))) != 0u;
}

// Local (drag-start pose) to world (current pose), and back.
vec3 sculpt_instance_to_world(vec3 local) {
    return sculpt_instance_mat3(8) * local + sculpt_instance_vec3(17);
}
vec3 sculpt_instance_to_local(vec3 world) {
    return sculpt_instance_mat3(20) * world + sculpt_instance_vec3(29);
}
// A direction (normal) -- uniform scale, so the rotation alone.
vec3 sculpt_instance_dir_to_world(vec3 dir) {
    return normalize(sculpt_instance_mat3(8) * dir);
}
vec3 sculpt_instance_dir_to_local(vec3 dir) {
    return normalize(sculpt_instance_mat3(20) * dir);
}
// World lengths per local length.
float sculpt_instance_scale() {
    return sculpt_instance_float(32);
}

vec3 sculpt_instance_box_min() {
    return sculpt_instance_vec3(34);
}
vec3 sculpt_instance_box_max() {
    return sculpt_instance_vec3(37);
}

// The instance's chunk slot holding local point q: -1 if none (an empty
// chunk inside its range), -2 if q is outside its range altogether.
int sculpt_instance_slot_at(vec3 q) {
    ivec3 chunk = ivec3(floor(q / CHUNK_WORLD_SIZE)) -
        ivec3(SCULPT_INSTANCE_WORD(1), SCULPT_INSTANCE_WORD(2), SCULPT_INSTANCE_WORD(3));
    ivec3 dims = ivec3(SCULPT_INSTANCE_WORD(4), SCULPT_INSTANCE_WORD(5),
                       SCULPT_INSTANCE_WORD(6));
    if (any(lessThan(chunk, ivec3(0))) || any(greaterThanEqual(chunk, dims))) {
        return -2;
    }
    int index = chunk.x + chunk.y * dims.x + chunk.z * dims.x * dims.y;
    if (index >= SCULPT_INSTANCE_MAX_CHUNKS) {
        return -2;
    }
    return SCULPT_INSTANCE_WORD(SCULPT_INSTANCE_SLOTS_AT + index);
}

// A world-space box moved by the instance transform, as the world box
// around it.
void sculpt_instance_box_to_world(vec3 lo, vec3 hi, out vec3 out_lo, out vec3 out_hi) {
    mat3 m = sculpt_instance_mat3(8);
    vec3 centre = sculpt_instance_to_world((lo + hi) * 0.5);
    vec3 half_extent = (hi - lo) * 0.5;
    vec3 reach = abs(m[0]) * half_extent.x + abs(m[1]) * half_extent.y +
                 abs(m[2]) * half_extent.z;
    out_lo = centre - reach;
    out_hi = centre + reach;
}
