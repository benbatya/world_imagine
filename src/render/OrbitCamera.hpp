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

// Orbit camera. Rotation stored directly as a quaternion (world→camera orientation).
class OrbitCamera {
public:
  OrbitCamera();

  glm::vec3 position() const;
  glm::mat4 view() const;
  glm::mat4 proj(float aspect) const;

  CameraUBO makeUBO(float aspect, float vpWidth = 0.f, float vpHeight = 0.f) const;

  float moveSpeed{2.0f};

  // Orbit: dx/dy are pixel deltas
  void orbit(float dx, float dy);
  void pan(float dx, float dy);
  void dolly(float delta);

  // Roll around the camera-local back axis (target→eye).
  void roll(float dr);

  // Keyboard movement: fwd dolly, rgt/upv pan target. Scaled by moveSpeed * dt.
  void moveKeyboard(float fwd, float rgt, float upv, float dt);

  void adjustSpeed(float factor) { moveSpeed = std::max(0.01f, moveSpeed * factor); }

  // Set the pose of the camera
  void set(const glm::vec3& position, const glm::quat& orientation);

  // Restore default pose: target=origin, distance=5, elevation=0.3 rad, moveSpeed=2.
  void reset();

  // Reset camera to frame a bounding box defined by its center and radius.
  // Positions the camera at 2.5× the radius away, facing the center.
  void fitToBounds(glm::vec3 center, float radius);

  // World-space orientation quaternion (for initialising FlyCamera).
  glm::quat orientation() const { return orientation_; }

private:

  float     distance_;   // distance from target
  glm::vec3 target_;

  float fovY_{0.7854f}; // 45° in radians
  float zNear_{0.01f};
  float zFar_{500.f};

  // Q = Ry(azimuth) * Rx(-elevation) at construction (azimuth=0, elevation=0.3 rad).
  // Updated incrementally by orbit(); world-space orientation of the camera.
  glm::quat orientation_;
};
