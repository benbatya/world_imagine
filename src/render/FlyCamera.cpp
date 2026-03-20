#include "FlyCamera.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

// ---------------------------------------------------------------------------
// Local axis helpers
// ---------------------------------------------------------------------------
glm::vec3 FlyCamera::forward() const {
    return glm::normalize(orientation_ * glm::vec3(0.f, 0.f, -1.f));
}

glm::vec3 FlyCamera::right() const {
    return glm::normalize(orientation_ * glm::vec3(1.f, 0.f, 0.f));
}

glm::vec3 FlyCamera::up() const {
    return glm::normalize(orientation_ * glm::vec3(0.f, 1.f, 0.f));
}

// ---------------------------------------------------------------------------
// Matrices
// ---------------------------------------------------------------------------
glm::mat4 FlyCamera::view() const {
    glm::mat4 rot   = glm::mat4_cast(glm::inverse(orientation_));
    glm::mat4 trans = glm::translate(glm::mat4(1.f), -position_);
    return rot * trans;
}

glm::mat4 FlyCamera::proj(float aspect) const {
    glm::mat4 p  = glm::perspective(fovY, aspect, zNear, zFar);
    p[1][1] *= -1.f;  // Vulkan Y-flip
    return p;
}

CameraUBO FlyCamera::makeUBO(float aspect, float vpWidth, float vpHeight) const {
    CameraUBO ubo;
    ubo.view     = view();
    ubo.proj     = proj(aspect);
    ubo.camPos   = glm::vec4(position_, 1.f);
    ubo.viewport = glm::vec4(vpWidth, vpHeight, 0.f, 0.f);
    return ubo;
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------
void FlyCamera::look(float dx, float dy) {
    glm::quat yawQ   = glm::angleAxis(-dx * kLookSens, glm::vec3{0.f, 1.f, 0.f});
    glm::quat pitchQ = glm::angleAxis(-dy * kLookSens, right());
    orientation_ = glm::normalize(pitchQ * yawQ * orientation_);
}

void FlyCamera::roll(float dr) {
    glm::quat rollQ = glm::angleAxis(dr * kRollSens, forward());
    orientation_ = glm::normalize(rollQ * orientation_);
}

void FlyCamera::move(float fwd, float rgt, float upv, float dt) {
    position_ += forward()               * (fwd * moveSpeed_ * dt);
    position_ += right()                 * (rgt * moveSpeed_ * dt);
    position_ += glm::vec3{0.f, 1.f, 0.f} * (upv * moveSpeed_ * dt);
}

void FlyCamera::pan(float dx, float dy) {
    position_ -= right() * (dx * kPanScale * moveSpeed_);
    position_ += up()    * (dy * kPanScale * moveSpeed_);
}

void FlyCamera::dolly(float ticks) {
    position_ += forward() * (ticks * kDollyScale * moveSpeed_);
}

// ---------------------------------------------------------------------------
// Sync from orbit
// ---------------------------------------------------------------------------
void FlyCamera::setFromOrbit(const OrbitCamera& orbit) {
    position_    = orbit.position();
    orientation_ = orbit.orientation();
}
