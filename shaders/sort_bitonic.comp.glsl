#version 450

layout(local_size_x = 256) in;

layout(push_constant) uniform PushConstants {
    uint k;            // bitonic block size (power of 2)
    uint j;            // comparison distance (power of 2)
    uint paddedCount;  // total element count (power of 2)
};

// Depth keys: sorted in descending order (back-to-front)
layout(set = 0, binding = 0) buffer DepthKeys {
    float keys[];
} depths;

// Indices: permuted alongside keys
layout(set = 0, binding = 1) buffer Indices {
    uint indices[];
} idx;

void main() {
    uint gid = gl_GlobalInvocationID.x;
    uint i   = gid;

    // Each thread handles one compare-and-swap for the pair (i, ixj)
    uint ixj = i ^ j;

    // Only the lower-index thread in each pair does the swap
    if (ixj <= i) return;
    if (i >= paddedCount) return;

    float ki = depths.keys[i];
    float kj = depths.keys[ixj];

    // Descending sort: within a block of size 2k,
    // the first half sorts descending, determined by (i & k) == 0
    bool descending = ((i & k) == 0);

    bool doSwap = descending ? (ki < kj) : (ki > kj);

    if (doSwap) {
        depths.keys[i]   = kj;
        depths.keys[ixj] = ki;

        uint ti = idx.indices[i];
        idx.indices[i]   = idx.indices[ixj];
        idx.indices[ixj] = ti;
    }
}
