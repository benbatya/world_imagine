#pragma once

#include <deque>

class FpsOverlay {
public:
    void draw();  // call once per frame, inside ImGui frame

private:
    struct Sample {
        double timeSeconds;
        float  fps;
    };

    static constexpr double k_windowSec = 10.0;
    static constexpr int    k_plotCount = 200;  // ring-buffer size for the plot

    static constexpr float k_showDist = 80.0f;   // px from overlay edge to show
    static constexpr float k_hideDist = 120.0f;  // px from overlay edge to hide

    bool   m_autoHide{false};
    bool   m_visible{true};  // used only when m_autoHide is true

    double m_lastTime{0.0};

    // Previous-second FPS counter
    double m_secBucketStart{0.0};
    int    m_secFrameCount{0};
    float  m_prevSecFps{0.0f};

    // Rolling samples over the last k_windowSec seconds
    std::deque<Sample> m_samples;

    // Fixed-size float buffer fed to ImGui::PlotLines
    float m_plotBuf[k_plotCount]{};
    int   m_plotOffset{0};
    int   m_plotFilled{0};
};
