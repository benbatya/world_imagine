#pragma once
#include "AppState.hpp"
#include "ui/MainWindow.hpp"
#include "ui/MenuOverlay.hpp"
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

  AppState    m_state;
  MainWindow  m_window;
  MenuOverlay m_menuOverlay;
  Viewport3D  m_viewport;
};
