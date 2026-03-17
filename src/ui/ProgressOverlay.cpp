#include "ProgressOverlay.hpp"

#include "app/AppState.hpp"
#include "io/SplatIO.hpp"

#include <imgui.h>
#include <string>

void ProgressOverlay::draw(AppState& state) {
  auto& io = SplatIO::instance();

  if (!io.isLoading() && !io.isDone())
    return;

  // Anchor a full-width bar to the bottom of the main viewport
  ImGuiViewport* vp        = ImGui::GetMainViewport();
  constexpr float k_width = 400.f;
  constexpr float k_height = 52.f;
  ImGui::SetNextWindowPos({vp->Pos.x, vp->Pos.y + vp->Size.y - k_height}, ImGuiCond_Always);
  ImGui::SetNextWindowSize({k_width, k_height}, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.82f);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_AlwaysAutoResize;

  ImGui::Begin("##progress_overlay", nullptr, flags);

  // Status text (left column)
  std::string txt = io.statusText();
  ImGui::SetNextItemWidth(200.f);
  ImGui::AlignTextToFramePadding();
  if (!txt.empty())
    ImGui::TextUnformatted(txt.c_str());
  else
    ImGui::TextDisabled("Working…");

  ImGui::SameLine();

  // Progress bar (stretches to fill space before the cancel button)
  float btnW    = 80.f;
  float spacing = ImGui::GetStyle().ItemSpacing.x;
  float barW    = ImGui::GetContentRegionAvail().x - btnW - spacing;
  float p       = io.progress();
  char overlay[32];
  std::snprintf(overlay, sizeof(overlay), p > 0.f ? "%.0f%%" : "…", p * 100.f);
  ImGui::ProgressBar(p, {barW, 0.f}, overlay);

  ImGui::SameLine();

  // Cancel button
  if (ImGui::Button("Cancel", {btnW, 0.f}))
    io.requestCancel();

  // Auto-dismiss once the job reports done
  if (io.isDone())
    io.finalize(state);

  ImGui::End();
}
