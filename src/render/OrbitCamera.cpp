#include "OrbitCamera.hpp"
#include <cmath>

glm::vec3 OrbitCamera::position() const {
  return target + orientation() * glm::vec3{0.f, 0.f, distance};
}

// Column-major view matrix (Vulkan/OpenGL convention, camera looks into -Z in view space).
// Basis vectors extracted directly from the orientation quaternion — no cross-product
// degeneracy at the poles.
glm::mat4 OrbitCamera::view() const {
  glm::quat q     = orientation();
  glm::vec3 right = q * glm::vec3{1.f, 0.f, 0.f};
  glm::vec3 up    = q * glm::vec3{0.f, 1.f, 0.f};
  glm::vec3 back  = q * glm::vec3{0.f, 0.f, 1.f}; // target→eye direction (+Z camera-local)
  glm::vec3 eye   = target + back * distance;

  glm::mat4 m{1.f};
  // Row 0: right
  m[0][0] = right.x; m[1][0] = right.y; m[2][0] = right.z; m[3][0] = -glm::dot(right, eye);
  // Row 1: up
  m[0][1] = up.x;    m[1][1] = up.y;    m[2][1] = up.z;    m[3][1] = -glm::dot(up,    eye);
  // Row 2: back (+Z local = -forward; no sign flip needed since back already points away)
  m[0][2] = back.x;  m[1][2] = back.y;  m[2][2] = back.z;  m[3][2] = -glm::dot(back,  eye);
  // Row 3: homogeneous
  m[0][3] = 0.f; m[1][3] = 0.f; m[2][3] = 0.f; m[3][3] = 1.f;
  return m;
}

// Perspective projection: Vulkan convention (Y-flipped, depth [0,1]).
// proj[1][1] is negative to flip Y, depth mapping: near→0, far→1.
glm::mat4 OrbitCamera::proj(float aspect) const {
  float f = 1.f / std::tan(fovY * 0.5f);
  float n = zNear, r = zFar;

  glm::mat4 m{0.f};
  m[0][0] = f / aspect;
  m[1][1] = -f;               // Y flip
  m[2][2] = r / (n - r);
  m[2][3] = -1.f;             // sets clip.w = -posView.z
  m[3][2] = (n * r) / (n - r);
  return m;
}

CameraUBO OrbitCamera::makeUBO(float aspect, float vpWidth, float vpHeight) const {
  CameraUBO ubo;
  ubo.view     = view();
  ubo.proj     = proj(aspect);
  ubo.camPos   = glm::vec4(position(), 0.f);
  ubo.viewport = glm::vec4(vpWidth, vpHeight, 0.f, 0.f);
  return ubo;
}

void OrbitCamera::orbit(float dx, float dy) {
  // Horizontal drag: rotate around world Y (pre-multiply).
  // Vertical drag: rotate around camera-local X (post-multiply).
  glm::quat dAz = glm::angleAxis( dx * 0.005f, glm::vec3{0.f, 1.f, 0.f});
  glm::quat dEl = glm::angleAxis(-dy * 0.005f, glm::vec3{1.f, 0.f, 0.f});
  orientation_ = glm::normalize(dAz * orientation_ * dEl);
}

void OrbitCamera::pan(float dx, float dy) {
  glm::quat q     = orientation();
  glm::vec3 right = q * glm::vec3{1.f, 0.f, 0.f};
  glm::vec3 up    = q * glm::vec3{0.f, 1.f, 0.f};
  float     scale = distance * 0.001f;
  target += right * (-dx * scale) + up * (dy * scale);
}

void OrbitCamera::dolly(float delta) {
  distance *= (1.f - delta * 0.001f);
  if (distance < 0.01f) distance = 0.01f;
}

void OrbitCamera::resetOrientation(float azimuth, float elevation) {
  orientation_ = glm::normalize(glm::angleAxis(azimuth,    glm::vec3{0.f, 1.f, 0.f}) *
                                glm::angleAxis(-elevation, glm::vec3{1.f, 0.f, 0.f}));
}

void OrbitCamera::fitToBounds(glm::vec3 center, float radius) {
  if (radius < 1e-4f) radius = 1.f;
  target   = center;
  distance = radius * 2.5f;
  zNear    = radius * 0.001f;
  zFar     = radius * 20.f;
  // Keep current orientation_ so the user's view direction is preserved on re-load
}
