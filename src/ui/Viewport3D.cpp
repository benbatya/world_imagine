#include "Viewport3D.hpp"
#include "app/AppState.hpp"
#include "model/GaussianModel.hpp"
#include "render/VulkanContext.hpp"

#include <imgui.h>

void Viewport3D::init(VulkanContext& ctx, uint32_t width, uint32_t height,
                      const std::string& shaderDir) {
  m_vpWidth  = width;
  m_vpHeight = height;
  m_renderer.init(ctx, width, height, shaderDir);
  m_initialized = true;

  // Place camera to see a typical 3DGS scene
  m_camera.azimuth   = 0.f;
  m_camera.elevation = 0.2f;
  m_camera.distance  = 3.f;
  m_camera.target    = {0, 0, 0};
}

void Viewport3D::destroy(VulkanContext& ctx) {
  if (m_initialized) {
    m_renderer.destroy(ctx);
    m_initialized = false;
  }
}

void Viewport3D::renderOffscreen(VulkanContext& ctx, VkCommandBuffer cmd) {
  if (!m_initialized) return;
  m_renderer.render(ctx, cmd, m_camera, m_vpWidth, m_vpHeight);
}

void Viewport3D::draw(VulkanContext& ctx, AppState& state) {
  if (!m_initialized) return;

  ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
  ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar);

  // If model changed, upload to GPU
  {
    std::shared_ptr<GaussianModel> current;
    {
      std::lock_guard lock{state.gaussianMutex};
      current = state.gaussianModel;
    }
    if (current && current != m_lastModel) {
      m_lastModel = current;
      m_renderer.uploadSplats(ctx, *current, m_camera);
      state.setStatus("Splats uploaded: " + std::to_string(current->numSplats()));
    }
  }

  // Resize renderer if panel size changed
  ImVec2 avail = ImGui::GetContentRegionAvail();
  uint32_t w = std::max(1u, static_cast<uint32_t>(avail.x));
  uint32_t h = std::max(1u, static_cast<uint32_t>(avail.y));
  if (w != m_vpWidth || h != m_vpHeight) {
    m_vpWidth  = w;
    m_vpHeight = h;
    m_renderer.resize(ctx, w, h);
  }

  // Display the offscreen image
  ImGui::Image(m_renderer.outputDescriptorSet(), avail);

  // Orbit controls (only when the image is hovered)
  if (ImGui::IsItemHovered()) {
    ImGuiIO& io = ImGui::GetIO();

    if (io.MouseDown[0]) {
      m_camera.orbit(io.MouseDelta.x, io.MouseDelta.y);
    }
    if (io.MouseDown[1] || io.MouseDown[2]) {
      m_camera.pan(io.MouseDelta.x, io.MouseDelta.y);
    }
    if (io.MouseWheel != 0.f) {
      m_camera.dolly(-io.MouseWheel * 80.f);
    }
  }

  ImGui::End();
}
