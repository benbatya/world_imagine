#version 450

layout(local_size_x = 256) in;

layout(push_constant) uniform PushConstants {
    vec4 camPos;       // xyz = camera position
    vec4 camFwd;       // xyz = camera forward direction
    uint splatCount;   // actual number of splats
    uint paddedCount;  // next power-of-2 >= splatCount
};

// Source splat data: 14 floats per splat (position at offset 0..2)
layout(set = 0, binding = 0) readonly buffer SrcSplats {
    float data[];
} src;

// Output: depth keys for sorting
layout(set = 0, binding = 1) writeonly buffer DepthKeys {
    float keys[];
} depths;

// Output: index array (initialized to identity, then sorted)
layout(set = 0, binding = 2) writeonly buffer Indices {
    uint indices[];
} idx;

void main() {
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= paddedCount) return;

    if (gid < splatCount) {
        vec3 pos = vec3(src.data[gid * 14 + 0],
                        src.data[gid * 14 + 1],
                        src.data[gid * 14 + 2]);
        vec3 diff = pos - camPos.xyz;
        float depth = dot(diff, camFwd.xyz);
        depths.keys[gid] = depth;
        idx.indices[gid] = gid;
    } else {
        // Padding: smallest possible depth so descending sort puts these last
        depths.keys[gid] = -3.402823466e+38; // -FLT_MAX
        idx.indices[gid] = gid;
    }
}
