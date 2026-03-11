#include "MenuOverlay.hpp"

#include "app/AppState.hpp"
#include "io/SplatIO.hpp"
#include "model/GaussianModel.hpp"

#include <cstdio>
#include <imgui.h>
#include <stdexcept>

// The menu appears when the mouse enters the top-left 150x150 px hot-zone
// and stays visible until the mouse moves beyond 200px from that corner.
static constexpr float k_showDist = 150.0f;
static constexpr float k_hideDist = 200.0f;

void MenuOverlay::draw(AppState& state) {
  ImVec2 mouse = ImGui::GetMousePos();
  float dist   = std::max(mouse.x, mouse.y);

  if (dist < k_showDist)
    m_visible = true;
  else if (dist > k_hideDist)
    m_visible = false;

  if (m_visible) {

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowBgAlpha(0.85f);

    if (ImGui::Begin("##menu", nullptr, flags)) {
      ImGui::TextDisabled("  World Imagine  ");
      ImGui::Separator();

      if (ImGui::MenuItem("Import Video")) {
        state.setStatus("Import Video: not yet implemented (Phase 5)");
      }

      if (ImGui::MenuItem("Import Splats")) {
        m_showImport = true;
        m_importPath.fill(0);
      }

      {
        std::lock_guard lock{state.gaussianMutex};
        bool hasModel = state.gaussianModel != nullptr;

        if (!hasModel)
          ImGui::BeginDisabled();
        if (ImGui::MenuItem("Export Splats")) {
          m_showExport = true;
          m_exportPath.fill(0);
        }
        if (!hasModel)
          ImGui::EndDisabled();

        ImGui::Separator();

        if (hasModel) {
          size_t n = state.gaussianModel->numSplats();
          ImGui::TextDisabled("%zu splats loaded", n);
        } else {
          ImGui::TextDisabled("No model loaded");
        }
      }

      ImGui::TextDisabled("%s", state.getStatus().c_str());
    }
    ImGui::End();
  }

  // -----------------------------------------------------------------------
  // Import dialog
  // -----------------------------------------------------------------------
  if (m_showImport) {
    ImGui::OpenPopup("Import Splats##dlg");
    m_showImport = false;
  }

  bool importOpen = true;
  if (ImGui::BeginPopupModal("Import Splats##dlg",
                             &importOpen,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Path to .ply file:");
    ImGui::SetNextItemWidth(420.f);
    ImGui::InputText("##importpath", m_importPath.data(), m_importPath.size());

    if (ImGui::Button("Import", {120, 0})) {
      try {
        SplatIO io;
        auto model = io.importPLY(m_importPath.data());
        {
          std::lock_guard lock{state.gaussianMutex};
          state.gaussianModel = std::move(model);
        }
        state.setStatus("Loaded " + std::to_string(state.gaussianModel->numSplats()) + " splats");
        ImGui::CloseCurrentPopup();
      } catch (const std::exception& ex) {
        state.setStatus(std::string("Import failed: ") + ex.what());
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {120, 0}))
      ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
  }

  // -----------------------------------------------------------------------
  // Export dialog
  // -----------------------------------------------------------------------
  if (m_showExport) {
    ImGui::OpenPopup("Export Splats##dlg");
    m_showExport = false;
  }

  bool exportOpen = true;
  if (ImGui::BeginPopupModal("Export Splats##dlg",
                             &exportOpen,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Output .ply path:");
    ImGui::SetNextItemWidth(420.f);
    ImGui::InputText("##exportpath", m_exportPath.data(), m_exportPath.size());

    if (ImGui::Button("Export", {120, 0})) {
      try {
        std::lock_guard lock{state.gaussianMutex};
        if (!state.gaussianModel)
          throw std::runtime_error("No model loaded");
        SplatIO io;
        io.exportPLY(*state.gaussianModel, m_exportPath.data());
        state.setStatus("Exported to " + std::string(m_exportPath.data()));
        ImGui::CloseCurrentPopup();
      } catch (const std::exception& ex) {
        state.setStatus(std::string("Export failed: ") + ex.what());
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {120, 0}))
      ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
  }
}
