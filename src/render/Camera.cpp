#include "Camera.hpp"

Vec3 Camera::position() const {
  float cosEl = std::cos(elevation);
  float sinEl = std::sin(elevation);
  float sinAz = std::sin(azimuth);
  float cosAz = std::cos(azimuth);
  return {
    target.x + distance * cosEl * sinAz,
    target.y + distance * sinEl,
    target.z + distance * cosEl * cosAz,
  };
}

// Column-major lookAt (Vulkan/OpenGL convention, camera looks into -Z in view space)
Mat4 Camera::view() const {
  Vec3 eye = position();
  Vec3 f   = normalize3(target - eye);              // forward
  Vec3 s   = normalize3(cross3(f, {0, 1, 0}));      // right
  if (dot3(s, s) < 1e-6f) s = {1, 0, 0};           // degenerate guard (looking straight up)
  Vec3 u = cross3(s, f);                             // corrected up

  Mat4 m  = Mat4::identity();
  // Row 0: right (s)
  m(0, 0) = s.x; m(1, 0) = s.y; m(2, 0) = s.z; m(3, 0) = -dot3(s, eye);
  // Row 1: up (u)
  m(0, 1) = u.x; m(1, 1) = u.y; m(2, 1) = u.z; m(3, 1) = -dot3(u, eye);
  // Row 2: -forward
  m(0, 2) = -f.x; m(1, 2) = -f.y; m(2, 2) = -f.z; m(3, 2) = dot3(f, eye);
  // Row 3: homogeneous
  m(0, 3) = 0; m(1, 3) = 0; m(2, 3) = 0; m(3, 3) = 1;
  return m;
}

// Perspective projection: Vulkan convention (Y-flipped, depth [0,1]).
// proj[1][1] is negative to flip Y, depth mapping: near→0, far→1.
Mat4 Camera::proj(float aspect) const {
  float f = 1.f / std::tan(fovY * 0.5f);
  float n = zNear, r = zFar;

  Mat4 m{};
  m(0, 0) = f / aspect;
  m(1, 1) = -f; // Y flip
  m(2, 2) = r / (n - r);
  m(2, 3) = -1.f; // sets clip.w = -posView.z
  m(3, 2) = (n * r) / (n - r);
  return m;
}

CameraUBO Camera::makeUBO(float aspect) const {
  CameraUBO ubo;
  Mat4 v = view();
  Mat4 p = proj(aspect);
  for (int i = 0; i < 16; ++i) {
    ubo.view[i] = v.data[i];
    ubo.proj[i] = p.data[i];
  }
  Vec3 pos   = position();
  ubo.camPos[0] = pos.x;
  ubo.camPos[1] = pos.y;
  ubo.camPos[2] = pos.z;
  return ubo;
}

void Camera::orbit(float dx, float dy) {
  azimuth   += dx * 0.005f;
  elevation += dy * 0.005f;
  // Clamp elevation to avoid gimbal at poles
  const float limit = 1.55f; // ~89°
  if (elevation > limit) elevation = limit;
  if (elevation < -limit) elevation = -limit;
}

void Camera::pan(float dx, float dy) {
  Vec3 eye = position();
  Vec3 f   = normalize3(target - eye);
  Vec3 s   = normalize3(cross3(f, {0, 1, 0}));
  if (dot3(s, s) < 1e-6f) s = {1, 0, 0};
  Vec3 u = cross3(s, f);

  float scale = distance * 0.001f;
  target = target + (s * (-dx * scale)) + (u * (dy * scale));
}

void Camera::dolly(float delta) {
  distance *= (1.f - delta * 0.001f);
  if (distance < 0.01f) distance = 0.01f;
}
