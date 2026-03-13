#version 450

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------
layout(set = 0, binding = 0) uniform CameraUBO {
  mat4 view;
  mat4 proj;
  vec4 camPos; // w unused
} cam;

// 14 floats per splat:
//   [0-2]  position xyz
//   [3-5]  log-scale xyz
//   [6-9]  quaternion (w, x, y, z)
//   [10]   pre-sigmoid opacity
//   [11-13] DC SH (r, g, b)
layout(set = 0, binding = 1) readonly buffer SplatSSBO {
  float data[];
} splats;

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------
layout(location = 0) out vec2 outUV;    // ellipse-space UV (±3 sigma at corners)
layout(location = 1) out vec4 outColor; // premultiplied-ready rgba

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
float sigmoid(float x) { return 1.0 / (1.0 + exp(-x)); }

// Build rotation matrix from quaternion stored as (w, x, y, z)
mat3 quatToMat(vec4 q) {
  float w = q.x, x = q.y, y = q.z, z = q.w;
  return mat3(
    1.0 - 2.0*(y*y + z*z),       2.0*(x*y + w*z),       2.0*(x*z - w*y),
          2.0*(x*y - w*z), 1.0 - 2.0*(x*x + z*z),       2.0*(y*z + w*x),
          2.0*(x*z + w*y),       2.0*(y*z - w*x), 1.0 - 2.0*(x*x + y*y)
  );
}

// ---------------------------------------------------------------------------
// Quad corner table (2 triangles = 6 vertices)
// ---------------------------------------------------------------------------
const vec2 kCorners[6] = vec2[6](
  vec2(-1.0, -1.0),
  vec2( 1.0, -1.0),
  vec2( 1.0,  1.0),
  vec2(-1.0, -1.0),
  vec2( 1.0,  1.0),
  vec2(-1.0,  1.0)
);

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
void main() {
  int splatIdx = gl_VertexIndex / 6;
  int corner   = gl_VertexIndex % 6;

  // Unpack splat data
  int   base    = splatIdx * 14;
  vec3  pos     = vec3(splats.data[base + 0],  splats.data[base + 1],  splats.data[base + 2]);
  vec3  logSc   = vec3(splats.data[base + 3],  splats.data[base + 4],  splats.data[base + 5]);
  vec4  quat    = vec4(splats.data[base + 6],  splats.data[base + 7],
                       splats.data[base + 8],  splats.data[base + 9]);
  float opacity = splats.data[base + 10];
  vec3  dcSH    = vec3(splats.data[base + 11], splats.data[base + 12], splats.data[base + 13]);

  // -------------------------------------------------------------------------
  // Transform center to view space
  // -------------------------------------------------------------------------
  vec4 posView = cam.view * vec4(pos, 1.0);
  float tz = posView.z; // negative when in front of camera

  // Cull splats behind the near plane or at the camera
  if (tz >= -0.001) {
    gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // behind clip volume, discarded
    outUV    = vec2(0.0);
    outColor = vec4(0.0);
    return;
  }

  // -------------------------------------------------------------------------
  // 3D covariance: Sigma3D = R * S^2 * R^T
  // -------------------------------------------------------------------------
  vec3 scale = exp(logSc);
  mat3 R     = quatToMat(quat);
  mat3 RS    = mat3(R[0] * scale.x, R[1] * scale.y, R[2] * scale.z);
  mat3 sig3D = RS * transpose(RS); // = R * diag(s^2) * R^T

  // -------------------------------------------------------------------------
  // Project covariance to NDC-space 2x2: Sigma_ndc = J * W * Sigma3D * W^T * J^T
  // W = upper-left 3x3 of view matrix (rotation part)
  // J = perspective Jacobian (2x3)
  // -------------------------------------------------------------------------
  mat3 W    = mat3(cam.view);          // column-major: W[col][row]
  mat3 sigV = W * sig3D * transpose(W); // view-space covariance

  float w    = -tz;                    // positive view-space depth
  float p00  = cam.proj[0][0];         // f/aspect
  float p11  = cam.proj[1][1];         // -f (Vulkan Y-flip)
  float tx   = posView.x;
  float ty   = posView.y;

  // J row 0: [p00/w, 0, p00*tx/w^2]   J row 1: [0, p11/w, p11*ty/w^2]
  float a00 = p00 / w;
  float a02 = p00 * tx / (w * w);
  float a11 = p11 / w;
  float a12 = p11 * ty / (w * w);

  // Read symmetric view-space covariance (sigV[col][row] in GLSL)
  float sv00 = sigV[0][0];
  float sv01 = sigV[1][0]; // = sigV[0][1] by symmetry
  float sv02 = sigV[2][0]; // = sigV[0][2]
  float sv11 = sigV[1][1];
  float sv12 = sigV[2][1]; // = sigV[1][2]
  float sv22 = sigV[2][2];

  // T = J * sigV  (2x3 result, row 0 and row 1)
  float t00 = a00 * sv00 + a02 * sv02;
  float t01 = a00 * sv01 + a02 * sv12;
  float t02 = a00 * sv02 + a02 * sv22;
  float t10 = a11 * sv01 + a12 * sv02;
  float t11 = a11 * sv11 + a12 * sv12;
  float t12 = a11 * sv12 + a12 * sv22;

  // Sigma_ndc = T * J^T  (2x2 symmetric result + regularization)
  float sig_a = t00 * a00 + t02 * a02 + 0.3;
  float sig_b = t01 * a11 + t02 * a12;
  float sig_c = t11 * a11 + t12 * a12 + 0.3;

  // -------------------------------------------------------------------------
  // Eigendecomposition of 2x2 symmetric Sigma_ndc
  // -------------------------------------------------------------------------
  float mid  = 0.5 * (sig_a + sig_c);
  float diff = 0.5 * (sig_a - sig_c);
  float disc = sqrt(max(0.0, diff * diff + sig_b * sig_b));

  float lambda1 = mid + disc; // larger eigenvalue
  float lambda2 = mid - disc;

  float r1 = 3.0 * sqrt(max(0.0, lambda1)); // NDC radius along major axis
  float r2 = 3.0 * sqrt(max(0.0, lambda2)); // NDC radius along minor axis

  // Eigenvector for lambda1 (major axis direction in NDC)
  vec2 axis1;
  float bLen = length(vec2(sig_b, lambda1 - sig_a));
  if (bLen < 1e-6) {
    axis1 = (sig_a >= sig_c) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
  } else {
    axis1 = normalize(vec2(sig_b, lambda1 - sig_a));
  }
  vec2 axis2 = vec2(-axis1.y, axis1.x); // perpendicular

  // -------------------------------------------------------------------------
  // Emit billboard vertex
  // -------------------------------------------------------------------------
  vec2 uv         = kCorners[corner];
  vec2 ndcOffset  = uv.x * r1 * axis1 + uv.y * r2 * axis2;

  vec4 clipCenter = cam.proj * posView;
  clipCenter.xy  += ndcOffset * clipCenter.w; // NDC offset → clip space
  gl_Position     = clipCenter;

  // UV passed to fragment shader in 3-sigma space (exp(-0.5 * 9) ≈ 0 at corners)
  outUV = uv * 3.0;

  // Color: DC SH coefficient → RGB via standard 3DGS formula
  // rgb = clamp(C0 * dcSH + 0.5, 0, 1)   where C0 = 0.28209479177
  const float C0 = 0.28209479177387814;
  vec3 rgb = clamp(C0 * dcSH + 0.5, 0.0, 1.0);

  float alpha = sigmoid(opacity);
  outColor = vec4(rgb, alpha);
}
