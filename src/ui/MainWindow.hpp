#pragma once
#include "render/VulkanContext.hpp"

struct GLFWwindow;

class MainWindow {
public:
  void init(const char* title, int width, int height);
  void destroy();

  bool shouldClose() const;
  bool wasResized() const { return m_resized; }
  void clearResized() { m_resized = false; }

  GLFWwindow* glfwHandle() const { return m_window; }
  VulkanContext& vkCtx() { return m_ctx; }

private:
  static void framebufferResizeCallback(GLFWwindow* window, int w, int h);

  GLFWwindow* m_window{nullptr};
  VulkanContext m_ctx;
  bool m_resized{false};
};
