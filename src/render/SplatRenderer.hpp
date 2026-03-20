#pragma once
#include "OrbitCamera.hpp"
#include "GpuBuffer.hpp"
#include "VulkanPipeline.hpp"
#include <cstdint>
#include <vulkan/vulkan.h>

struct VulkanContext;
class GaussianModel;

// Renders a GaussianModel into an offscreen VkImage that ImGui::Image() can display.
//
// Lifecycle:
//   init(ctx)  — allocate images, render pass, pipeline, descriptors
//   uploadSplats(ctx, model, cam) — depth-sort + stage splat data to GPU
//   render(ctx, cmd, cam, w, h)   — record draw commands into cmd (no render pass start)
//   destroy(ctx)
//   resize(ctx, w, h) — call when viewport size changes
class SplatRenderer {
public:
  void init(VulkanContext& ctx, uint32_t width, uint32_t height,
            const std::string& shaderDir);
  void destroy(VulkanContext& ctx);

  // Upload new splat data (depth-sorted along camFwd). Call on main thread.
  void uploadSplats(VulkanContext& ctx, const GaussianModel& model, glm::vec3 camPos,
                    glm::vec3 camFwd);

  // Record the offscreen render pass commands into cmd.
  // The offscreen color image starts and ends in SHADER_READ_ONLY_OPTIMAL.
  void render(VulkanContext& ctx, VkCommandBuffer cmd, const CameraUBO& ubo,
              uint32_t width, uint32_t height);

  void resize(VulkanContext& ctx, uint32_t width, uint32_t height);

  // ImGui handle — pass to ImGui::Image().
  VkDescriptorSet outputDescriptorSet() const { return m_imguiTexDescSet; }

  bool hasContent() const { return m_splatCount > 0; }

private:
  // Offscreen render target
  VkImage       m_colorImage{VK_NULL_HANDLE};
  VkImageView   m_colorView{VK_NULL_HANDLE};
  VmaAllocation m_colorAlloc{};
  VkImage       m_depthImage{VK_NULL_HANDLE};
  VkImageView   m_depthView{VK_NULL_HANDLE};
  VmaAllocation m_depthAlloc{};
  VkSampler     m_sampler{VK_NULL_HANDLE};
  VkRenderPass  m_renderPass{VK_NULL_HANDLE};
  VkFramebuffer m_framebuffer{VK_NULL_HANDLE};
  VkDescriptorSet m_imguiTexDescSet{VK_NULL_HANDLE};

  // Splat SSBO (device-local)
  GpuBuffer m_splatBuf;
  size_t    m_splatCount{0};

  // Camera UBO (persistently mapped CPU→GPU)
  GpuBuffer m_cameraUBOBuf;
  void*     m_cameraUBOMapped{nullptr};

  // Pipeline + descriptors
  VulkanPipeline  m_pipeline;
  VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
  VkDescriptorSet  m_descriptorSet{VK_NULL_HANDLE};

  uint32_t m_width{0};
  uint32_t m_height{0};

  void createOffscreenImages(VulkanContext& ctx, uint32_t w, uint32_t h);
  void destroyOffscreenImages(VulkanContext& ctx);
  void createRenderPass(VulkanContext& ctx);
  void createFramebuffer(VulkanContext& ctx, uint32_t w, uint32_t h);
  void createDescriptorPool(VulkanContext& ctx);
  void createDescriptors(VulkanContext& ctx);
  void updateDescriptors(VulkanContext& ctx);

  // One-time submit helper
  VkCommandBuffer beginOneTime(VulkanContext& ctx);
  void            endOneTime(VulkanContext& ctx, VkCommandBuffer cmd);

  // Transition color image to target layout
  void transitionColor(VkCommandBuffer cmd,
                       VkImageLayout oldLayout,
                       VkImageLayout newLayout,
                       VkPipelineStageFlags srcStage,
                       VkPipelineStageFlags dstStage,
                       VkAccessFlags srcAccess,
                       VkAccessFlags dstAccess);
};
