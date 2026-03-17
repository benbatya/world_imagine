#include "Camera.hpp"

// Q = Ry(azimuth) * Rx(-elevation)
// Rotating local +X/+Y/+Z by Q yields the camera's right/up/back axes in world space.
Quat Camera::orientation() const {
  return Quat::fromAxisAngle({0.f, 1.f, 0.f}, azimuth) *
         Quat::fromAxisAngle({1.f, 0.f, 0.f}, -elevation);
}

Vec3 Camera::position() const {
  return target + orientation().rotate({0.f, 0.f, distance});
}

// Column-major view matrix (Vulkan/OpenGL convention, camera looks into -Z in view space).
// Basis vectors extracted directly from the orientation quaternion — no cross-product
// degeneracy at the poles.
Mat4 Camera::view() const {
  Quat q     = orientation();
  Vec3 right = q.rotate({1.f, 0.f, 0.f});
  Vec3 up    = q.rotate({0.f, 1.f, 0.f});
  Vec3 back  = q.rotate({0.f, 0.f, 1.f}); // target→eye direction (+Z camera-local)
  Vec3 eye   = target + back * distance;

  Mat4 m = Mat4::identity();
  // Row 0: right
  m(0, 0) = right.x; m(1, 0) = right.y; m(2, 0) = right.z; m(3, 0) = -dot3(right, eye);
  // Row 1: up
  m(0, 1) = up.x;    m(1, 1) = up.y;    m(2, 1) = up.z;    m(3, 1) = -dot3(up,    eye);
  // Row 2: back (+Z local = -forward; no sign flip needed since back already points away)
  m(0, 2) = back.x;  m(1, 2) = back.y;  m(2, 2) = back.z;  m(3, 2) = -dot3(back,  eye);
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

CameraUBO Camera::makeUBO(float aspect, float vpWidth, float vpHeight) const {
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
  ubo.viewport[0] = vpWidth;
  ubo.viewport[1] = vpHeight;
  return ubo;
}

void Camera::orbit(float dx, float dy) {
  azimuth   += dx * 0.005f;
  elevation += dy * 0.005f;
}

void Camera::pan(float dx, float dy) {
  Quat  q     = orientation();
  Vec3  right = q.rotate({1.f, 0.f, 0.f});
  Vec3  up    = q.rotate({0.f, 1.f, 0.f});
  float scale = distance * 0.001f;
  target = target + (right * (-dx * scale)) + (up * (dy * scale));
}

void Camera::dolly(float delta) {
  distance *= (1.f - delta * 0.001f);
  if (distance < 0.01f) distance = 0.01f;
}

void Camera::fitToBounds(Vec3 center, float radius) {
  if (radius < 1e-4f) radius = 1.f;
  target   = center;
  distance = radius * 2.5f;
  zNear    = radius * 0.001f;
  zFar     = radius * 20.f;
  // Keep current azimuth/elevation so the user's orientation is preserved on re-load
}
