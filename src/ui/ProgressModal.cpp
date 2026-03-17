#include "ProgressModal.hpp"

#include "app/AppState.hpp"
#include "io/SplatIO.hpp"

#include <imgui.h>
#include <string>

static constexpr const char* k_popupId = "##progress_modal";

void ProgressModal::draw(AppState& state) {
    auto& io = SplatIO::instance();

    // Nothing to show if no job is active or pending finalization
    if (!io.isLoading() && !io.isDone()) {
        m_opened = false;
        return;
    }

    // Trigger the popup the first frame a job appears
    if (!m_opened) {
        ImGui::OpenPopup(k_popupId);
        m_opened = true;
    }

    // Centre it on the main viewport
    ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({420.f, 0.f}, ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove;

    if (!ImGui::BeginPopupModal(k_popupId, nullptr, flags))
        return;

    // Status text
    std::string txt = io.statusText();
    if (!txt.empty())
        ImGui::TextWrapped("%s", txt.c_str());
    else
        ImGui::TextDisabled("Working…");

    // Progress bar — negative width fills available space
    float p = io.progress();
    char  overlay[32];
    std::snprintf(overlay, sizeof(overlay), p > 0.f ? "%.0f%%" : "…", p * 100.f);
    ImGui::ProgressBar(p, {-1.f, 0.f}, overlay);

    ImGui::Spacing();

    // Cancel button — centred
    float btnW = 120.f;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnW) * 0.5f);
    if (ImGui::Button("Cancel", {btnW, 0.f}))
        io.requestCancel();

    // Auto-close once the job reports done
    if (io.isDone()) {
        io.finalize(state);
        m_opened = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}
