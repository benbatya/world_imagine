#include "Application.hpp"

#include "render/VulkanContext.hpp"

#include <GLFW/glfw3.h>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <stdexcept>
#include <vulkan/vulkan.h>

Application::Application() {
  m_window.init("World Imagine", 1280, 720);
}

Application::~Application() {
  // Ensure GPU is idle before destroying anything
  auto& ctx = m_window.vkCtx();
  vkDeviceWaitIdle(ctx.device);
  m_window.destroy();
}

void Application::run() {
  auto& ctx = m_window.vkCtx();

  while (!m_window.shouldClose()) {
    glfwPollEvents();

    // Handle window resize
    if (m_window.wasResized()) {
      int w, h;
      glfwGetFramebufferSize(m_window.glfwHandle(), &w, &h);
      // Wait while minimized
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
    m_menuOverlay.draw(m_state);

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
  }
}

void Application::renderFrame(uint32_t imageIndex) {
  auto& ctx   = m_window.vkCtx();
  auto& frame = ctx.frames[ctx.currentFrame];

  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);

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
