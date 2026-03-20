#pragma once
#include "OrbitCamera.hpp"  // CameraUBO + OrbitCamera for setFromOrbit
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// First-person fly camera. Orientation stored as a quaternion to support roll.
//
// Controls (wired in Viewport3D::draw when panel is hovered):
//   RMB drag / arrow keys  — look (yaw world-Y, pitch local-right)
//   WASD                   — move forward/back/strafe
//   MMB drag               — pan (translate along local right/up)
//   Q / E                  — move up / down
//   R / F                  — roll left / right
//   scroll                 — dolly along forward
//   = / -                  — multiply moveSpeed by 1.2× / 0.83×
class FlyCamera {
public:
    FlyCamera();

    glm::vec3 position() const { return position_; }
    glm::quat orientation() const { return orientation_; }
    float     moveSpeed() const { return moveSpeed_; }

    glm::mat4 view() const;
    glm::mat4 proj(float aspect) const;
    CameraUBO makeUBO(float aspect, float vpWidth = 0.f, float vpHeight = 0.f) const;

    // Yaw around world Y, pitch around camera-local right. dx/dy are pixel deltas.
    void look(float dx, float dy);

    // Roll around camera-local forward. dr is a signed unit step (±1 per frame).
    void roll(float dr);

    // Translate along local axes, scaled by moveSpeed_ * dt.
    void move(float fwd, float right, float up, float dt);

    // Translate along local right/up (MMB pan). dx/dy are pixel deltas.
    void pan(float dx, float dy);

    // Move along local forward (scroll wheel).
    void dolly(float ticks);

    // Multiply move speed by factor.
    void adjustSpeed(float factor) { moveSpeed_ = std::max(0.01f, moveSpeed_ * factor); }

    // Initialise from an OrbitCamera so the view doesn't snap on mode switch.
    void set(const glm::vec3& position, const glm::quat& orientation);

    // Restore default pose: position=(0,0,3), identity orientation, moveSpeed=2.
    void reset();

private:

    float fovY_{0.7854f};  // 45°
    float zNear_{0.01f};
    float zFar_{500.f};

    glm::vec3 position_;
    glm::quat orientation_; 
    float     moveSpeed_{2.0f};

    static constexpr float kLookSens{0.003f};
    static constexpr float kRollSens{0.02f};
    static constexpr float kPanScale{0.005f};
    static constexpr float kDollyScale{0.5f};  // units per scroll tick × moveSpeed_

    glm::vec3 forward() const;
    glm::vec3 right() const;
    glm::vec3 up() const;
};
