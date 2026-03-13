#pragma once
#include <string>
#include <vulkan/vulkan.h>

struct VulkanContext;

// Owns the shader modules, descriptor set layout, pipeline layout,
// and graphics pipeline for Gaussian splat rendering.
class VulkanPipeline {
public:
  void create(VulkanContext& ctx,
              VkRenderPass   renderPass,
              const std::string& vertSpvPath,
              const std::string& fragSpvPath);
  void destroy(VkDevice device);

  VkPipeline            pipeline{VK_NULL_HANDLE};
  VkPipelineLayout      layout{VK_NULL_HANDLE};
  VkDescriptorSetLayout descriptorSetLayout{VK_NULL_HANDLE};

private:
  VkShaderModule m_vert{VK_NULL_HANDLE};
  VkShaderModule m_frag{VK_NULL_HANDLE};

  static VkShaderModule loadSpv(VkDevice device, const std::string& path);
};
