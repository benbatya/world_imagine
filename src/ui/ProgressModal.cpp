#include "ProgressModal.hpp"

#include "app/AppState.hpp"
#include "util/AsyncJob.hpp"

#include <imgui.h>
#include <exception>
#include <string>

static constexpr const char* k_popupId = "##progress_modal";

void ProgressModal::draw(AppState& state) {
    // Nothing to show if no job is active
    if (!state.activeJob)
        return;

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

    auto job = state.activeJob;

    // Status text
    std::string txt = job->statusText();
    if (!txt.empty())
        ImGui::TextWrapped("%s", txt.c_str());
    else
        ImGui::TextDisabled("Working…");

    // Progress bar — negative width fills available space
    float p = job->progress();
    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), p > 0.f ? "%.0f%%" : "…", p * 100.f);
    ImGui::ProgressBar(p, {-1.f, 0.f}, overlay);

    ImGui::Spacing();

    // Cancel button — centred
    float btnW = 120.f;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnW) * 0.5f);
    if (ImGui::Button("Cancel", {btnW, 0.f}))
        job->requestCancel();

    // Auto-close once the job reports done
    if (job->isDone()) {
        if (auto ex = job->exception()) {
            try {
                std::rethrow_exception(ex);
            } catch (const std::exception& e) {
                state.setStatus(std::string("Error: ") + e.what());
            } catch (...) {
                state.setStatus("Unknown error during background job");
            }
        }
        state.activeJob.reset();
        state.showProgressModal = false;
        m_opened                = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}
