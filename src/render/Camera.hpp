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

struct Quat {
  float x{0}, y{0}, z{0}, w{1};

  static Quat fromAxisAngle(Vec3 axis, float angle) {
    float s = std::sin(angle * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f)};
  }

  // Hamilton product
  Quat operator*(const Quat& o) const {
    return {
        w * o.x + x * o.w + y * o.z - z * o.y,
        w * o.y - x * o.z + y * o.w + z * o.x,
        w * o.z + x * o.y - y * o.x + z * o.w,
        w * o.w - x * o.x - y * o.y - z * o.z,
    };
  }

  // Rotate a vector: v' = q * (0,v) * q^-1  (Rodrigues, unit-quat form)
  Vec3 rotate(Vec3 v) const {
    Vec3 qv{x, y, z};
    Vec3 t{
        2.f * (qv.y * v.z - qv.z * v.y),
        2.f * (qv.z * v.x - qv.x * v.z),
        2.f * (qv.x * v.y - qv.y * v.x),
    };
    return {
        v.x + w * t.x + qv.y * t.z - qv.z * t.y,
        v.y + w * t.y + qv.z * t.x - qv.x * t.z,
        v.z + w * t.z + qv.x * t.y - qv.y * t.x,
    };
  }
};

// GPU-side camera uniform (std140 compatible: two mat4s + two vec4s).
struct CameraUBO {
  float view[16];
  float proj[16];
  float camPos[3];
  float _pad0{0.f};
  float viewport[2]; // width, height in pixels
  float _pad1[2]{0.f, 0.f};
};

// Orbit camera. Rotation stored as azimuth/elevation; a quaternion is derived
// per-frame for gimbal-free basis vector extraction near the poles.
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

  CameraUBO makeUBO(float aspect, float vpWidth = 0.f, float vpHeight = 0.f) const;

  // Orbit: dx/dy are pixel deltas
  void orbit(float dx, float dy);
  void pan(float dx, float dy);
  void dolly(float delta);

  // Reset camera to frame a bounding box defined by its center and radius.
  // Positions the camera at 2.5× the radius away, facing the center.
  void fitToBounds(Vec3 center, float radius);

private:
  // Q = Ry(azimuth) * Rx(-elevation); rotates local axes into world space.
  Quat orientation() const;
};
