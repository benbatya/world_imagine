#include "Viewport3D.hpp"
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

    // Place camera to see a typical 3DGS scene (azimuth=0, elevation≈0.2 rad)
    m_camera.resetOrientation(0.f, 0.2f);
    m_camera.distance = 3.f;
    m_camera.target   = {0, 0, 0};
}

void Viewport3D::destroy(VulkanContext& ctx) {
    if (m_initialized) {
        m_renderer.destroy(ctx);
        m_initialized = false;
    }
}

void Viewport3D::renderOffscreen(VulkanContext& ctx, VkCommandBuffer cmd) {
    if (!m_initialized) return;
    m_renderer.render(ctx, cmd, m_currentUBO, m_vpWidth, m_vpHeight);
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

    const CameraMode mode = state.cameraMode.load(std::memory_order_relaxed);

    // Detect camera mode switch and sync fly camera from orbit on Orbit→Fly transition.
    if (mode != m_prevMode) {
        if (mode == CameraMode::Fly)
            m_flyCamera.setFromOrbit(m_camera);
        m_prevMode = mode;
    }

    // Upload to GPU when model is new or has grown (incremental load)
    {
        // Cheap atomic read — no lock needed for the count check.
        size_t committedCount = state.committedSplatCount.load(std::memory_order_acquire);

        std::shared_ptr<GaussianModel> current;
        {
            std::lock_guard lock{state.gaussianMutex};
            current = state.gaussianModel;
        }

        bool isNewModel = current && current != m_lastModel;
        bool hasGrown   = current && current == m_lastModel && committedCount > m_lastSplatCount;

        if (isNewModel || hasGrown) {
            if (isNewModel) {
                m_lastModel = current;

                // Auto-fit orbit camera once on first load.
                if (current->numSplats() > 0 && current->positions.defined()) {
                    std::lock_guard posLock{current->mutex};
                    auto pos = current->positions.cpu(); // [N, 3]

                    auto q05 = torch::quantile(pos, 0.05, 0);
                    auto q95 = torch::quantile(pos, 0.95, 0);

                    glm::vec3 bmin{q05[0].item<float>(), q05[1].item<float>(),
                                   q05[2].item<float>()};
                    glm::vec3 bmax{q95[0].item<float>(), q95[1].item<float>(),
                                   q95[2].item<float>()};

                    glm::vec3 center = (bmin + bmax) * 0.5f;
                    glm::vec3 ext    = bmax - bmin;
                    float     radius = glm::length(ext) * 0.5f;
                    m_camera.fitToBounds(center, radius);
                }
            }

            m_lastSplatCount = committedCount;

            glm::vec3 camPos = (mode == CameraMode::Orbit) ? m_camera.position()
                                                            : m_flyCamera.position();
            m_renderer.uploadSplats(ctx, *current, camPos);
            state.setStatus("Splats on GPU: " + std::to_string(committedCount));
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

    // Camera controls (only when the image is hovered)
    if (ImGui::IsItemHovered()) {
        float dt = io.DeltaTime;

        if (mode == CameraMode::Orbit) {
            // Mouse controls — LMB reserved for selection
            if (io.MouseDown[1])
                m_camera.orbit(-io.MouseDelta.x, -io.MouseDelta.y);
            if (io.MouseDown[2])
                m_camera.pan(-io.MouseDelta.x, io.MouseDelta.y);
            if (io.MouseWheel != 0.f)
                m_camera.dolly(io.MouseWheel * 80.f);

            // Arrow keys — orbit azimuth / elevation
            {
                float kArrow = 200.f * dt;
                float adx = 0.f, ady = 0.f;
                if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))  adx += kArrow;
                if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) adx -= kArrow;
                if (ImGui::IsKeyDown(ImGuiKey_UpArrow))    ady += kArrow;
                if (ImGui::IsKeyDown(ImGuiKey_DownArrow))  ady -= kArrow;
                if (adx != 0.f || ady != 0.f)
                    m_camera.orbit(adx, ady);
            }

            // WASD — dolly / strafe; Q/E — up / down
            {
                float fwd = 0.f, rgt = 0.f, upv = 0.f;
                if (ImGui::IsKeyDown(ImGuiKey_W)) fwd -= 1.f;
                if (ImGui::IsKeyDown(ImGuiKey_S)) fwd += 1.f;
                if (ImGui::IsKeyDown(ImGuiKey_A)) rgt -= 1.f;
                if (ImGui::IsKeyDown(ImGuiKey_D)) rgt += 1.f;
                if (ImGui::IsKeyDown(ImGuiKey_Q)) upv += 1.f;
                if (ImGui::IsKeyDown(ImGuiKey_E)) upv -= 1.f;
                if (fwd != 0.f || rgt != 0.f || upv != 0.f)
                    m_camera.moveKeyboard(fwd, rgt, upv, dt);
            }

            // R / F — roll
            {
                float rollDir = 0.f;
                if (ImGui::IsKeyDown(ImGuiKey_R)) rollDir += 1.f;
                if (ImGui::IsKeyDown(ImGuiKey_F)) rollDir -= 1.f;
                if (rollDir != 0.f)
                    m_camera.roll(rollDir);
            }

            // = / - — adjust move speed
            if (ImGui::IsKeyPressed(ImGuiKey_Equal))
                m_camera.adjustSpeed(1.2f);
            if (ImGui::IsKeyPressed(ImGuiKey_Minus))
                m_camera.adjustSpeed(1.f / 1.2f);

        } else {
            // ---- Fly camera ----

            // RMB drag — look (yaw + pitch)
            if (io.MouseDown[1])
                m_flyCamera.look(io.MouseDelta.x, io.MouseDelta.y);

            // MMB drag — pan (lateral translate)
            if (io.MouseDown[2])
                m_flyCamera.pan(-io.MouseDelta.x, io.MouseDelta.y);

            // Arrow keys — look
            {
                float kArrow = 200.f * dt;  // pixel-equivalent per frame
                float ldx = 0.f, ldy = 0.f;
                if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))  ldx -= kArrow;
                if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) ldx += kArrow;
                if (ImGui::IsKeyDown(ImGuiKey_UpArrow))    ldy -= kArrow;
                if (ImGui::IsKeyDown(ImGuiKey_DownArrow))  ldy += kArrow;
                if (ldx != 0.f || ldy != 0.f)
                    m_flyCamera.look(ldx, ldy);
            }

            // WASD — move
            {
                float fwd = 0.f, rgt = 0.f, upv = 0.f;
                if (ImGui::IsKeyDown(ImGuiKey_W)) fwd += 1.f;
                if (ImGui::IsKeyDown(ImGuiKey_S)) fwd -= 1.f;
                if (ImGui::IsKeyDown(ImGuiKey_A)) rgt -= 1.f;
                if (ImGui::IsKeyDown(ImGuiKey_D)) rgt += 1.f;
                if (ImGui::IsKeyDown(ImGuiKey_Q)) upv += 1.f;
                if (ImGui::IsKeyDown(ImGuiKey_E)) upv -= 1.f;
                if (fwd != 0.f || rgt != 0.f || upv != 0.f)
                    m_flyCamera.move(fwd, rgt, upv, dt);
            }

            // R / F — roll
            {
                float rollDir = 0.f;
                if (ImGui::IsKeyDown(ImGuiKey_R)) rollDir += 1.f;
                if (ImGui::IsKeyDown(ImGuiKey_F)) rollDir -= 1.f;
                if (rollDir != 0.f)
                    m_flyCamera.roll(rollDir);
            }

            // Scroll — dolly along forward
            if (io.MouseWheel != 0.f)
                m_flyCamera.dolly(io.MouseWheel);

            // = / - — adjust move speed
            if (ImGui::IsKeyPressed(ImGuiKey_Equal))
                m_flyCamera.adjustSpeed(1.2f);
            if (ImGui::IsKeyPressed(ImGuiKey_Minus))
                m_flyCamera.adjustSpeed(1.f / 1.2f);
        }
    }

    // Compute current UBO (used by renderOffscreen later in this frame)
    float aspect = (h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.f;
    if (mode == CameraMode::Orbit)
        m_currentUBO = m_camera.makeUBO(aspect, static_cast<float>(w), static_cast<float>(h));
    else
        m_currentUBO = m_flyCamera.makeUBO(aspect, static_cast<float>(w), static_cast<float>(h));

    ImGui::End();
}
