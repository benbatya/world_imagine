#version 450
// Stub vertex shader — Phase 3 will implement full Gaussian billboard projection.
// For now, emits nothing (zero splats means no draw calls issued).

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;

void main() {
    outUV    = vec2(0.0);
    outColor = vec4(1.0);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
