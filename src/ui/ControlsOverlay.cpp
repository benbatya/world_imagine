#include "ControlsOverlay.hpp"

#include "Viewport3D.hpp"
#include "app/AppState.hpp"
#include <algorithm>
#include <imgui.h>

// Show when mouse enters the top-right 150×150 px hot-zone; hide beyond 200 px from that corner.
static constexpr float k_showDist = 200.0f;
static constexpr float k_hideDist = 250.0f;

void ControlsOverlay::draw(AppState& state, Viewport3D& viewport) {
    ImGuiIO&     io        = ImGui::GetIO();
    ImVec2       displaySz = io.DisplaySize;
    const float  padding   = 10.0f;
    const float  overlayW  = 220.0f;

    // Distance from the top-right corner (mirror of MenuOverlay's top-left logic).
    ImVec2 mouse   = ImGui::GetMousePos();
    float  fromRight = displaySz.x - mouse.x;
    float  dist      = std::max(fromRight, mouse.y);

    if (dist < k_showDist)
        m_visible = true;
    else if (dist > k_hideDist)
        m_visible = false;

    if (!m_visible)
        return;

    ImGui::SetNextWindowPos({displaySz.x - overlayW - padding, padding},
                            ImGuiCond_Always,
                            {0.0f, 0.0f});
    ImGui::SetNextWindowSize({overlayW, 0.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;

    if (!ImGui::Begin("##controls_overlay", nullptr, flags)) {
        ImGui::End();
        return;
    }

    CameraMode cm     = state.cameraMode.load(std::memory_order_relaxed);
    bool       isOrbit = (cm == CameraMode::Orbit);

    if (ImGui::RadioButton("Orbit", isOrbit))
        state.cameraMode.store(CameraMode::Orbit, std::memory_order_relaxed);
    ImGui::SameLine();
    if (ImGui::RadioButton("Fly", !isOrbit))
        state.cameraMode.store(CameraMode::Fly, std::memory_order_relaxed);

    ImGui::Separator();
    ImGui::TextDisabled("Controls");
    ImGui::Separator();

    // Column widths
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4.0f, 2.0f});
    if (ImGui::BeginTable("##ctrl_tbl", 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("Input",  ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);

        auto row = [](const char* input, const char* action) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(input);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", action);
        };

        if (isOrbit) {
            row("RMB drag",   "Orbit");
            row("Arrow keys", "Orbit");
            row("R / F",      "Roll left / right");
            row("MMB drag",   "Pan");
            row("Scroll",     "Dolly");
            row("WASD",       "Dolly / strafe");
            row("Q / E",      "Move up / down");
            row("= / -",      "Speed x1.2 / /1.2");
        } else {
            row("RMB drag",   "Look");
            row("Arrow keys", "Look");
            row("R / F",      "Roll left / right");
            row("MMB drag",   "Pan");
            row("Scroll",     "Dolly");
            row("WASD",       "Dolly / strafe");
            row("Q / E",      "Move up / down");
            row("= / -",      "Speed x1.2 / /1.2");
        }

        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    ImGui::Separator();
    if (ImGui::Button("Reset Camera", {ImGui::GetContentRegionAvail().x, 0.f}))
        viewport.resetCameras();

    ImGui::End();
}
