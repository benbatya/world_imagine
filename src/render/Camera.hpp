#pragma once
#include <array>
#include <cmath>

// Column-major 4x4 matrix (matches GLSL/Vulkan convention).
// Index: m[col][row] == m.data[col*4 + row]
struct Mat4 {
  float data[16]{};
  float& operator()(int col, int row) { return data[col * 4 + row]; }
  float  operator()(int col, int row) const { return data[col * 4 + row]; }

  static Mat4 identity() {
    Mat4 m;
    m(0, 0) = m(1, 1) = m(2, 2) = m(3, 3) = 1.f;
    return m;
  }
};

struct Vec3 {
  float x, y, z;
  Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

inline float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3  normalize3(Vec3 v) {
  float len = std::sqrt(dot3(v, v));
  if (len < 1e-8f) return {0, 0, 1};
  return v * (1.f / len);
}
inline Vec3 cross3(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// GPU-side camera uniform (std140 compatible: two mat4s + vec4).
struct CameraUBO {
  float view[16];
  float proj[16];
  float camPos[3];
  float _pad{0.f};
};

// Orbit camera. Position is derived from azimuth/elevation/distance around target.
class Camera {
public:
  float azimuth{0.f};    // radians, rotation around Y axis
  float elevation{0.3f}; // radians, angle above XZ plane
  float distance{5.f};   // distance from target
  Vec3  target{0, 0, 0};

  float fovY{0.7854f}; // 45° in radians
  float zNear{0.01f};
  float zFar{500.f};

  Vec3 position() const;
  Mat4 view() const;
  Mat4 proj(float aspect) const;

  CameraUBO makeUBO(float aspect) const;

  // Orbit: dx/dy are pixel deltas
  void orbit(float dx, float dy);
  void pan(float dx, float dy);
  void dolly(float delta);
};
