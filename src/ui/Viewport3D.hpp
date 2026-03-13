#pragma once
#include "render/Camera.hpp"
#include "render/SplatRenderer.hpp"
#include <memory>
#include <string>

struct AppState;
class GaussianModel;
struct VulkanContext;

// ImGui panel that shows the 3D Gaussian splat scene.
// Owns a SplatRenderer (offscreen Vulkan FBO) and a Camera (orbit controls).
class Viewport3D {
public:
  void init(VulkanContext& ctx, uint32_t width, uint32_t height,
            const std::string& shaderDir);
  void destroy(VulkanContext& ctx);

  // Call before the main ImGui render pass to record offscreen commands.
  void renderOffscreen(VulkanContext& ctx, VkCommandBuffer cmd);

  // Draw the ImGui window (call inside ImGui frame).
  void draw(VulkanContext& ctx, AppState& state);

private:
  SplatRenderer m_renderer;
  Camera        m_camera;

  // Track last model pointer to detect changes
  std::shared_ptr<GaussianModel> m_lastModel;

  uint32_t m_vpWidth{800};
  uint32_t m_vpHeight{600};
  bool     m_initialized{false};
};
