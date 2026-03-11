#pragma once
#include "AppState.hpp"
#include "ui/MainWindow.hpp"
#include "ui/MenuOverlay.hpp"

class Application {
  public:
    Application();
    ~Application();

    // Not copyable or movable
    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    void run();

  private:
    void renderFrame(uint32_t imageIndex);
    bool handleSwapchainResult(VkResult result);

    AppState m_state;
    MainWindow m_window;
    MenuOverlay m_menuOverlay;
};
