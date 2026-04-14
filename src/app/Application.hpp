#pragma once
#include "AppState.hpp"
#include "ui/ControlsOverlay.hpp"
#include "ui/FpsOverlay.hpp"
#include "ui/MainWindow.hpp"
#include "ui/MenuOverlay.hpp"
#include "ui/ProgressOverlay.hpp"
#include "ui/Viewport3D.hpp"

#include <chrono>
#include <filesystem>
#include <string>

class Application {
public:
    Application(int argc, char* argv[]);
    ~Application();

    // Not copyable or movable
    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    void run();

private:
    void renderFrame(uint32_t imageIndex);
    bool handleSwapchainResult(VkResult result);

    AppState        m_state;
    MainWindow      m_window;
    ControlsOverlay m_controlsOverlay;
    FpsOverlay      m_fpsOverlay;
    MenuOverlay     m_menuOverlay;
    ProgressOverlay m_progressOverlay;
    Viewport3D      m_viewport;

    // CLI automation (--import-run, --screenshot, etc.)
    std::filesystem::path m_autoImportDir;
    std::filesystem::path m_screenshotPath;
    float m_screenshotDelay{2.0f};
    bool  m_skipExtract{false};
    bool  m_skipInstantSplat{false};
    bool  m_autoMode{false};

    enum class AutoState { WaitingImport, WaitingDelay, Done };
    AutoState m_autoState{AutoState::WaitingImport};
    std::chrono::steady_clock::time_point m_importDoneTime;
    int m_idleFrames{0};
};
