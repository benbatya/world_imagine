#version 450
// Gaussian splat fragment shader — full implementation in Phase 3.

layout(location = 0) in  vec2 inUV;
layout(location = 1) in  vec4 inColor;
layout(location = 0) out vec4 outColor;

void main() {
    float alpha = inColor.a * exp(-0.5 * dot(inUV, inUV));
    outColor = vec4(inColor.rgb * alpha, alpha);  // premultiplied alpha
}
