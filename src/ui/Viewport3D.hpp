#pragma once

struct AppState;

// Phase 3: Vulkan offscreen render pass panel.
// Displays the gaussian splat scene via ImGui::Image backed by a VkImage.
class Viewport3D {
  public:
    void draw(AppState& state);
};
