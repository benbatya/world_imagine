#pragma once
#include "render/OrbitCamera.hpp"
#include "render/FlyCamera.hpp"
#include "render/SplatRenderer.hpp"
#include "app/AppState.hpp"
#include <memory>
#include <string>

class GaussianModel;
struct VulkanContext;

// ImGui panel that shows the 3D Gaussian splat scene.
// Owns a SplatRenderer (offscreen Vulkan FBO) and both an OrbitCamera and FlyCamera.
// The active camera is selected via AppState::cameraMode.
class Viewport3D {
public:
  void init(VulkanContext& ctx, uint32_t width, uint32_t height,
            const std::string& shaderDir);
  void destroy(VulkanContext& ctx);

  // Call before the main ImGui render pass to record offscreen commands.
  void renderOffscreen(VulkanContext& ctx, VkCommandBuffer cmd);

  // Draw the ImGui window (call inside ImGui frame).
  void draw(VulkanContext& ctx, AppState& state);

  // Reset both cameras to their default poses.
  void resetCameras();

private:
  SplatRenderer m_renderer;
  OrbitCamera   m_camera;
  FlyCamera     m_flyCamera;

  // Last UBO computed in draw() — used by renderOffscreen() in the same frame.
  CameraUBO m_currentUBO;

  // Previous camera mode — used to detect mode switches in draw().
  CameraMode m_prevMode{CameraMode::Orbit};

  // Track last model pointer and splat count to detect new model or mid-load growth
  std::shared_ptr<GaussianModel> m_lastModel;
  size_t m_lastSplatCount{0};

  uint32_t m_vpWidth{800};
  uint32_t m_vpHeight{600};
  bool     m_initialized{false};
};
