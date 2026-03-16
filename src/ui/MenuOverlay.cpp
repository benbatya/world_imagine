#include "MenuOverlay.hpp"

#include "app/AppState.hpp"
#include "io/SplatIO.hpp"
#include "model/GaussianModel.hpp"

#include <imgui.h>
#include <nfd.h>
#include <stdexcept>
#include <string>

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
                nfdchar_t* outPath = nullptr;
                nfdfilteritem_t filters[1] = {{"PLY files", "ply"}};
                nfdresult_t result = NFD_OpenDialog(&outPath, filters, 1, nullptr);
                if (result == NFD_OKAY) {
                    try {
                        SplatIO io;
                        auto model = io.importPLY(outPath);
                        {
                            std::lock_guard lock{state.gaussianMutex};
                            state.gaussianModel = std::move(model);
                        }
                        state.setStatus("Loaded " +
                                        std::to_string(state.gaussianModel->numSplats()) +
                                        " splats");
                    } catch (const std::exception& ex) {
                        state.setStatus(std::string("Import failed: ") + ex.what());
                    }
                    NFD_FreePath(outPath);
                }
            }

            {
                std::lock_guard lock{state.gaussianMutex};
                bool hasModel = state.gaussianModel != nullptr;

                if (!hasModel)
                    ImGui::BeginDisabled();
                if (ImGui::MenuItem("Export Splats")) {
                    nfdchar_t* outPath = nullptr;
                    nfdfilteritem_t filters[1] = {{"PLY files", "ply"}};
                    nfdresult_t result = NFD_SaveDialog(&outPath, filters, 1, nullptr, "export.ply");
                    if (result == NFD_OKAY) {
                        try {
                            if (!state.gaussianModel)
                                throw std::runtime_error("No model loaded");
                            SplatIO io;
                            io.exportPLY(*state.gaussianModel, outPath);
                            state.setStatus("Exported to " + std::string(outPath));
                        } catch (const std::exception& ex) {
                            state.setStatus(std::string("Export failed: ") + ex.what());
                        }
                        NFD_FreePath(outPath);
                    }
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
}
