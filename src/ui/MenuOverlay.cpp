#include "MenuOverlay.hpp"
#include "app/AppState.hpp"

#include <imgui.h>
#include <cstdio>

// The menu appears when the mouse enters the top-left 150x150 px hot-zone
// and stays visible until the mouse moves beyond 200px from that corner.
static constexpr float k_showDist = 150.0f;
static constexpr float k_hideDist = 200.0f;

void MenuOverlay::draw(AppState& state) {
    ImVec2 mouse = ImGui::GetMousePos();
    float  dist  = std::max(mouse.x, mouse.y);

    if (dist < k_showDist)
        m_visible = true;
    else if (dist > k_hideDist)
        m_visible = false;

    if (!m_visible)
        return;

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar     |
        ImGuiWindowFlags_NoResize       |
        ImGuiWindowFlags_NoMove         |
        ImGuiWindowFlags_NoScrollbar    |
        ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowBgAlpha(0.85f);

    if (ImGui::Begin("##menu", nullptr, flags)) {
        ImGui::TextDisabled("  World Imagine  ");
        ImGui::Separator();

        if (ImGui::MenuItem("Import Video")) {
            printf("[Menu] Import Video — (file dialog coming in Phase 5)\n");
            state.setStatus("Import Video: not yet implemented");
        }

        if (ImGui::MenuItem("Import Splats")) {
            printf("[Menu] Import Splats — (file dialog coming in Phase 2)\n");
            state.setStatus("Import Splats: not yet implemented");
        }

        if (ImGui::MenuItem("Export Splats")) {
            printf("[Menu] Export Splats — (file dialog coming in Phase 6)\n");
            state.setStatus("Export Splats: not yet implemented");
        }

        ImGui::Separator();
        ImGui::TextDisabled("%s", state.getStatus().c_str());
    }
    ImGui::End();
}
