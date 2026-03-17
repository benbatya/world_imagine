#pragma once
#include "AppState.hpp"
#include "ui/FpsOverlay.hpp"
#include "ui/MainWindow.hpp"
#include "ui/MenuOverlay.hpp"
#include "ui/ProgressOverlay.hpp"
#include "ui/Viewport3D.hpp"

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

    AppState      m_state;
    MainWindow    m_window;
    FpsOverlay    m_fpsOverlay;
    MenuOverlay   m_menuOverlay;
    ProgressOverlay m_progressOverlay;
    Viewport3D    m_viewport;
};
