#include "ProgressOverlay.hpp"

#include "app/AppState.hpp"
#include "io/SplatIO.hpp"
#include "pipeline/VideoImporter.hpp"

#include <imgui.h>
#include <string>

void ProgressOverlay::draw(AppState& state) {
    auto& sio = SplatIO::instance();
    auto& vim = VideoImporter::instance();

    // Pick the active source (at most one should be active at a time)
    bool sioActive = sio.isLoading() || sio.isDone();
    bool vimActive = vim.isLoading() || vim.isDone();

    if (!sioActive && !vimActive)
        return;

    // Aliases to whichever source is active
    float       progress   = vimActive ? vim.progress()   : sio.progress();
    std::string statusTxt  = vimActive ? vim.statusText() : sio.statusText();
    auto        doCancel   = [&]() { vimActive ? vim.requestCancel() : sio.requestCancel(); };
    auto        doFinalize = [&]() { vimActive ? vim.finalize(state) : sio.finalize(state); };

    // Anchor a fixed bar to the bottom-left of the main viewport
    ImGuiViewport* vp        = ImGui::GetMainViewport();
    constexpr float k_width  = 400.f;
    constexpr float k_height = 52.f;
    ImGui::SetNextWindowPos({vp->Pos.x, vp->Pos.y + vp->Size.y - k_height}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({k_width, k_height}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::Begin("##progress_overlay", nullptr, flags);

    // Status text (left column)
    ImGui::SetNextItemWidth(200.f);
    ImGui::AlignTextToFramePadding();
    if (!statusTxt.empty())
        ImGui::TextUnformatted(statusTxt.c_str());
    else
        ImGui::TextDisabled("Working…");

    ImGui::SameLine();

    // Progress bar
    float btnW    = 80.f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float barW    = ImGui::GetContentRegionAvail().x - btnW - spacing;
    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), progress > 0.f ? "%.0f%%" : "…", progress * 100.f);
    ImGui::ProgressBar(progress, {barW, 0.f}, overlay);

    ImGui::SameLine();

    // Cancel button
    if (ImGui::Button("Cancel", {btnW, 0.f}))
        doCancel();

    // Auto-dismiss once the job reports done
    if ((vimActive && vim.isDone()) || (sioActive && sio.isDone()))
        doFinalize();

    ImGui::End();
}
