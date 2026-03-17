#include "Application.hpp"

#include "render/VulkanContext.hpp"
#include "io/SplatIO.hpp"

#include <GLFW/glfw3.h>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <chrono>
#include <stdexcept>
#include <filesystem>
#include <thread>
#include <string>
#include <unistd.h>
#include <nfd.h>
#include <vulkan/vulkan.h>

static std::string exeDir() {
  char buf[4096]{};
  ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len <= 0) return ".";
  return std::filesystem::path(buf).parent_path().string();
}

Application::Application(int argc, char* argv[]) {
  NFD_Init();
  m_window.init("World Imagine", 1280, 720);

  auto& ctx             = m_window.vkCtx();
  std::string shaderDir = exeDir() + "/shaders";
  m_viewport.init(ctx, 800, 600, shaderDir);

  // Load a PLY file if one was passed as the first positional argument.
  // The progress modal will handle display during run().
  if (argc >= 2) {
    SplatIO::instance().loadAsync(argv[1], m_state);
  }
}

Application::~Application() {
  auto& ctx = m_window.vkCtx();
  vkDeviceWaitIdle(ctx.device);
  m_viewport.destroy(ctx);
  m_window.destroy();
  NFD_Quit();
}

void Application::run() {
  using Clock    = std::chrono::steady_clock;
  using Duration = std::chrono::duration<double>;
  static constexpr Duration k_frameBudget{1.0 / 60.0};

  auto& ctx        = m_window.vkCtx();
  auto  frameStart = Clock::now();

  while (!m_window.shouldClose()) {
    glfwPollEvents();

    // Handle window resize
    if (m_window.wasResized()) {
      int w, h;
      glfwGetFramebufferSize(m_window.glfwHandle(), &w, &h);
      while (w == 0 || h == 0) {
        glfwGetFramebufferSize(m_window.glfwHandle(), &w, &h);
        glfwWaitEvents();
      }
      vkDeviceWaitIdle(ctx.device);
      ctx.recreateSwapchain(w, h);
      m_window.clearResized();
    }

    // --- Acquire swapchain image ---
    FrameData& frame = ctx.frames[ctx.currentFrame];
    vkWaitForFences(ctx.device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex{};
    VkResult result = vkAcquireNextImageKHR(ctx.device,
                                            ctx.swapchain,
                                            UINT64_MAX,
                                            frame.imageAvailableSemaphore,
                                            VK_NULL_HANDLE,
                                            &imageIndex);

    if (!handleSwapchainResult(result))
      continue;

    vkResetFences(ctx.device, 1, &frame.inFlightFence);
    vkResetCommandBuffer(frame.commandBuffer, 0);

    // --- ImGui new frame ---
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // --- Draw UI ---
    m_viewport.draw(ctx, m_state);
    m_menuOverlay.draw(m_state);
    m_progressOverlay.draw(m_state);
    m_fpsOverlay.draw();

    // --- ImGui render ---
    ImGui::Render();

    // --- Record command buffer ---
    renderFrame(imageIndex);

    // --- Submit ---
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.waitSemaphoreCount   = 1;
    submitInfo.pWaitSemaphores      = &frame.imageAvailableSemaphore;
    submitInfo.pWaitDstStageMask    = &waitStage;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = &frame.renderFinishedSemaphore;

    if (vkQueueSubmit(ctx.graphicsQueue, 1, &submitInfo, frame.inFlightFence) != VK_SUCCESS)
      throw std::runtime_error("vkQueueSubmit failed");

    // --- Present ---
    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = &frame.renderFinishedSemaphore;
    presentInfo.swapchainCount     = 1;
    presentInfo.pSwapchains        = &ctx.swapchain;
    presentInfo.pImageIndices      = &imageIndex;

    result = vkQueuePresentKHR(ctx.graphicsQueue, &presentInfo);
    handleSwapchainResult(result);

    ctx.currentFrame = (ctx.currentFrame + 1) % FRAMES_IN_FLIGHT;

    // --- Frame-rate cap (60 fps) ---
    auto nextFrame = frameStart + k_frameBudget;
    std::this_thread::sleep_until(nextFrame);
    frameStart = Clock::now();
  }
}

void Application::renderFrame(uint32_t imageIndex) {
  auto& ctx   = m_window.vkCtx();
  auto& frame = ctx.frames[ctx.currentFrame];

  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);

  // Offscreen splat render pass (writes to SplatRenderer's color image)
  m_viewport.renderOffscreen(ctx, frame.commandBuffer);

  // Main ImGui render pass (swapchain framebuffer)
  VkClearValue clearColor{{{0.08f, 0.08f, 0.10f, 1.0f}}};
  VkRenderPassBeginInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rpInfo.renderPass      = ctx.renderPass;
  rpInfo.framebuffer     = ctx.framebuffers[imageIndex];
  rpInfo.renderArea      = {{0, 0}, ctx.swapchainExtent};
  rpInfo.clearValueCount = 1;
  rpInfo.pClearValues    = &clearColor;

  vkCmdBeginRenderPass(frame.commandBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commandBuffer);
  vkCmdEndRenderPass(frame.commandBuffer);

  vkEndCommandBuffer(frame.commandBuffer);
}

bool Application::handleSwapchainResult(VkResult result) {
  auto& ctx = m_window.vkCtx();
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    int w, h;
    glfwGetFramebufferSize(m_window.glfwHandle(), &w, &h);
    vkDeviceWaitIdle(ctx.device);
    ctx.recreateSwapchain(w, h);
    return false;
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    throw std::runtime_error("Swapchain operation failed");
  return true;
}
