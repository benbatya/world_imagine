#include "Viewport3D.hpp"
#include "app/AppState.hpp"
#include "model/GaussianModel.hpp"
#include "render/VulkanContext.hpp"

#include <imgui.h>
#include <cmath>

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

  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  constexpr ImGuiWindowFlags kFullscreenFlags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus |
      ImGuiWindowFlags_NoNav;
  ImGui::Begin("Viewport", nullptr, kFullscreenFlags);

  // If model changed, upload to GPU
  {
    std::shared_ptr<GaussianModel> current;
    {
      std::lock_guard lock{state.gaussianMutex};
      current = state.gaussianModel;
    }
    if (current && current != m_lastModel) {
      m_lastModel = current;

      // Auto-fit camera using 5th/95th percentile splat positions to ignore
      // outliers that would otherwise inflate a pure min/max bounding box.
      if (current->numSplats() > 0 && current->positions.defined()) {
        std::lock_guard posLock{current->mutex};
        auto pos = current->positions.cpu(); // [N, 3]

        // torch::quantile(input, q, dim) → shape [3] (one value per axis)
        auto q05 = torch::quantile(pos, 0.05, 0); // [3] 5th-percentile per axis
        auto q95 = torch::quantile(pos, 0.95, 0); // [3] 95th-percentile per axis

        glm::vec3 bmin{q05[0].item<float>(), q05[1].item<float>(), q05[2].item<float>()};
        glm::vec3 bmax{q95[0].item<float>(), q95[1].item<float>(), q95[2].item<float>()};

        // Center = midpoint of the percentile box
        glm::vec3 center = (bmin + bmax) * 0.5f;

        // Radius = half the diagonal of the percentile box
        glm::vec3 ext    = bmax - bmin;
        float     radius = glm::length(ext) * 0.5f;
        m_camera.fitToBounds(center, radius);
      }

      m_renderer.uploadSplats(ctx, *current, m_camera);
      state.setStatus("Splats uploaded: " + std::to_string(current->numSplats()));
    }
  }

  // Resize renderer if panel size changed
  ImVec2 avail = ImGui::GetContentRegionAvail();
  uint32_t w = static_cast<uint32_t>(std::max(1.f, avail.x));
  uint32_t h = static_cast<uint32_t>(std::max(1.f, avail.y));
  if (w != m_vpWidth || h != m_vpHeight) {
    m_vpWidth  = w;
    m_vpHeight = h;
    m_renderer.resize(ctx, w, h);
  }

  // Display the offscreen image
  ImGui::Image(m_renderer.outputDescriptorSet(), avail);

  // Orbit controls (only when the image is hovered)
  if (ImGui::IsItemHovered()) {
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
