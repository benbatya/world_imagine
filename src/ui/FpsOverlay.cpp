#include "FpsOverlay.hpp"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui.h>

void FpsOverlay::draw() {
  // ---- Timing ----
  double now = glfwGetTime();
  if (m_lastTime == 0.0) {
    m_lastTime = now;
    return;
  }
  double dt  = now - m_lastTime;
  m_lastTime = now;
  float fps  = (dt > 0.0) ? static_cast<float>(1.0 / dt) : 0.0f;

  // ---- Previous-second FPS ----
  if (m_secBucketStart == 0.0)
    m_secBucketStart = now;
  ++m_secFrameCount;
  if (now - m_secBucketStart >= 1.0) {
    double elapsed   = now - m_secBucketStart;
    m_prevSecFps     = static_cast<float>(m_secFrameCount / elapsed);
    m_secFrameCount  = 0;
    m_secBucketStart = now;
  }

  // ---- Push new sample, drop old ones ----
  m_samples.push_back({now, fps});
  while (!m_samples.empty() && m_samples.front().timeSeconds < now - k_windowSec)
    m_samples.pop_front();

  // ---- Write into the ring buffer for PlotLines ----
  m_plotBuf[m_plotOffset] = fps;
  m_plotOffset            = (m_plotOffset + 1) % k_plotCount;
  if (m_plotFilled < k_plotCount)
    ++m_plotFilled;

  // ---- Compute avg / min over the 10 s window ----
  float sum    = 0.0f;
  float minFps = fps;
  for (auto& s : m_samples) {
    sum += s.fps;
    minFps = std::min(minFps, s.fps);
  }
  float avgFps = m_samples.empty() ? 0.0f : sum / static_cast<float>(m_samples.size());

  // ---- Draw overlay (bottom-right corner) ----
  ImGuiIO& io          = ImGui::GetIO();
  ImVec2 displaySz     = io.DisplaySize;
  const float padding  = 10.0f;
  const float overlayW = 220.0f;

  // Auto-hide: measure distance from mouse to the overlay rect
  if (m_autoHide) {
    ImVec2 mouse   = ImGui::GetMousePos();
    float  oxMin   = displaySz.x - overlayW - padding;
    float  oyMin   = displaySz.y - padding - 90.0f;  // approx overlay height
    // Distance to the nearest edge of the overlay AABB
    float  dx      = std::max(0.0f, std::max(oxMin - mouse.x, mouse.x - (displaySz.x - padding)));
    float  dy      = std::max(0.0f, std::max(oyMin - mouse.y, mouse.y - displaySz.y));
    float  dist    = std::sqrt(dx * dx + dy * dy);

    if (dist < k_showDist)
      m_visible = true;
    else if (dist > k_hideDist)
      m_visible = false;

    if (!m_visible)
      return;
  }

  ImGui::SetNextWindowPos({displaySz.x - overlayW - padding, displaySz.y - padding},
                          ImGuiCond_Always,
                          {0.0f, 1.0f});
  ImGui::SetNextWindowSize({overlayW, 0.0f}, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.6f);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_AlwaysAutoResize;

  if (ImGui::Begin("##fps_overlay", nullptr, flags)) {
    ImGui::Checkbox("Auto", &m_autoHide);
    ImGui::Text("FPS  avg %.1f  min %.1f", avgFps, minFps);

    // Scale the plot between 0 and a rounded-up ceiling so it's readable
    float plotMax = std::max(1.0f, std::ceil(avgFps * 1.5f / 10.0f) * 10.0f);

    // PlotLines expects the oldest-first order; since we use a ring buffer
    // the oldest entry sits at m_plotOffset (when filled) or 0.
    int count  = m_plotFilled;
    int offset = (m_plotFilled < k_plotCount) ? 0 : m_plotOffset;

    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), "%.0f fps", m_prevSecFps);

    ImGui::PlotLines("##fpsplot",
                     m_plotBuf,
                     count,
                     offset,
                     overlay,
                     0.0f,
                     plotMax,
                     {ImGui::GetContentRegionAvail().x, 50.0f});
  }
  ImGui::End();
}
