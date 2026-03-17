#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// GPU-side camera uniform (std140 compatible: two mat4s + two vec4s).
struct CameraUBO {
  glm::mat4 view{1.f};
  glm::mat4 proj{1.f};
  glm::vec4 camPos{0.f};  // w unused
  glm::vec4 viewport{0.f}; // xy = width, height in pixels; zw unused
};

// Orbit camera. Rotation stored as azimuth/elevation; a quaternion is derived
// per-frame for gimbal-free basis vector extraction near the poles.
class Camera {
public:
  float     azimuth{0.f};    // radians, rotation around Y axis
  float     elevation{0.3f}; // radians, angle above XZ plane
  float     distance{5.f};   // distance from target
  glm::vec3 target{0.f};

  float fovY{0.7854f}; // 45° in radians
  float zNear{0.01f};
  float zFar{500.f};

  glm::vec3 position() const;
  glm::mat4 view() const;
  glm::mat4 proj(float aspect) const;

  CameraUBO makeUBO(float aspect, float vpWidth = 0.f, float vpHeight = 0.f) const;

  // Orbit: dx/dy are pixel deltas
  void orbit(float dx, float dy);
  void pan(float dx, float dy);
  void dolly(float delta);

  // Reset camera to frame a bounding box defined by its center and radius.
  // Positions the camera at 2.5× the radius away, facing the center.
  void fitToBounds(glm::vec3 center, float radius);

private:
  // Q = Ry(azimuth) * Rx(-elevation); rotates local axes into world space.
  glm::quat orientation() const;
};
