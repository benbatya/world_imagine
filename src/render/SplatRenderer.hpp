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
//   init(ctx)  — allocate images, render pass, pipeline, descriptors, compute pipelines
//   uploadSplatData(ctx, model) — pack + stage unsorted splat data to GPU (on model load/growth)
//   render(ctx, cmd, cam, w, h)   — GPU sort + record draw commands into cmd
//   destroy(ctx)
//   resize(ctx, w, h) — call when viewport size changes
class SplatRenderer {
public:
  void init(VulkanContext& ctx, uint32_t width, uint32_t height,
            const std::string& shaderDir);
  void destroy(VulkanContext& ctx);

  // Upload unsorted splat data to GPU. Call on model load or growth (main thread).
  void uploadSplatData(VulkanContext& ctx, const GaussianModel& model);

  // GPU depth-sort + record the offscreen render pass commands into cmd.
  // Camera forward is extracted from the UBO view matrix.
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

  // Source splat SSBO (unsorted, device-local)
  GpuBuffer m_srcSplatBuf;
  size_t    m_splatCount{0};
  size_t    m_splatCountPadded{0};

  // Sort buffers (device-local)
  GpuBuffer m_depthKeyBuf;  // [paddedCount] floats — view-space depths
  GpuBuffer m_indexBuf;     // [paddedCount] uint32 — sorted indices

  // Camera UBO (persistently mapped CPU→GPU)
  GpuBuffer m_cameraUBOBuf;
  void*     m_cameraUBOMapped{nullptr};

  // Graphics pipeline + descriptors
  VulkanPipeline  m_pipeline;
  VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
  VkDescriptorSet  m_descriptorSet{VK_NULL_HANDLE};

  // Compute pipelines for GPU sort
  VkPipeline            m_depthCompPipeline{VK_NULL_HANDLE};
  VkPipelineLayout      m_depthCompLayout{VK_NULL_HANDLE};
  VkDescriptorSetLayout m_depthCompDSLayout{VK_NULL_HANDLE};
  VkDescriptorSet       m_depthCompDS{VK_NULL_HANDLE};
  VkShaderModule        m_depthCompShader{VK_NULL_HANDLE};

  VkPipeline            m_sortCompPipeline{VK_NULL_HANDLE};
  VkPipelineLayout      m_sortCompLayout{VK_NULL_HANDLE};
  VkDescriptorSetLayout m_sortCompDSLayout{VK_NULL_HANDLE};
  VkDescriptorSet       m_sortCompDS{VK_NULL_HANDLE};
  VkShaderModule        m_sortCompShader{VK_NULL_HANDLE};

  uint32_t m_width{0};
  uint32_t m_height{0};

  void createOffscreenImages(VulkanContext& ctx, uint32_t w, uint32_t h);
  void destroyOffscreenImages(VulkanContext& ctx);
  void createRenderPass(VulkanContext& ctx);
  void createFramebuffer(VulkanContext& ctx, uint32_t w, uint32_t h);
  void createDescriptorPool(VulkanContext& ctx);
  void createDescriptors(VulkanContext& ctx);
  void updateDescriptors(VulkanContext& ctx);

  void createComputePipelines(VulkanContext& ctx, const std::string& shaderDir);
  void destroyComputePipelines(VulkanContext& ctx);
  void updateComputeDescriptors(VulkanContext& ctx);
  void gpuSort(VkCommandBuffer cmd, glm::vec3 camPos, glm::vec3 camFwd);

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

  static VkShaderModule loadSpv(VkDevice device, const std::string& path);
};
