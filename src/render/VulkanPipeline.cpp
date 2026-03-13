#include "VulkanPipeline.hpp"
#include "VulkanContext.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

// ---------------------------------------------------------------------------
// SPIR-V loader
// ---------------------------------------------------------------------------
VkShaderModule VulkanPipeline::loadSpv(VkDevice device, const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open())
    throw std::runtime_error("VulkanPipeline: cannot open SPIR-V: " + path);
  auto size = static_cast<size_t>(f.tellg());
  f.seekg(0);
  std::vector<char> buf(size);
  f.read(buf.data(), static_cast<std::streamsize>(size));

  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = size;
  ci.pCode    = reinterpret_cast<const uint32_t*>(buf.data());
  VkShaderModule mod;
  if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS)
    throw std::runtime_error("VulkanPipeline: vkCreateShaderModule failed: " + path);
  return mod;
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------
void VulkanPipeline::create(VulkanContext& ctx,
                            VkRenderPass   renderPass,
                            const std::string& vertSpvPath,
                            const std::string& fragSpvPath) {
  VkDevice device = ctx.device;

  m_vert = loadSpv(device, vertSpvPath);
  m_frag = loadSpv(device, fragSpvPath);

  // --- Descriptor set layout ---
  // Binding 0: CameraUBO (uniform buffer, vertex stage)
  // Binding 1: SplatSSBO (storage buffer, vertex stage)
  VkDescriptorSetLayoutBinding bindings[2]{};
  bindings[0].binding         = 0;
  bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

  bindings[1].binding         = 1;
  bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

  VkDescriptorSetLayoutCreateInfo dslCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dslCI.bindingCount = 2;
  dslCI.pBindings    = bindings;
  if (vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &descriptorSetLayout) != VK_SUCCESS)
    throw std::runtime_error("VulkanPipeline: vkCreateDescriptorSetLayout failed");

  // --- Pipeline layout ---
  VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plCI.setLayoutCount = 1;
  plCI.pSetLayouts    = &descriptorSetLayout;
  if (vkCreatePipelineLayout(device, &plCI, nullptr, &layout) != VK_SUCCESS)
    throw std::runtime_error("VulkanPipeline: vkCreatePipelineLayout failed");

  // --- Shader stages ---
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = m_vert;
  stages[0].pName  = "main";
  stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = m_frag;
  stages[1].pName  = "main";

  // --- No vertex input (data comes from SSBO via gl_VertexIndex) ---
  VkPipelineVertexInputStateCreateInfo vertInput{
    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{
    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  // Dynamic viewport/scissor
  VkDynamicState dynStates[]             = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynCI = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynCI.dynamicStateCount                = 2;
  dynCI.pDynamicStates                   = dynStates;

  VkPipelineViewportStateCreateInfo viewportState{
    VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewportState.viewportCount = 1;
  viewportState.scissorCount  = 1;

  VkPipelineRasterizationStateCreateInfo rast{
    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rast.polygonMode = VK_POLYGON_MODE_FILL;
  rast.cullMode    = VK_CULL_MODE_NONE; // splat billboards are double-sided
  rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rast.lineWidth   = 1.f;

  VkPipelineMultisampleStateCreateInfo ms{
    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Depth: no write (splats use alpha blending, back-to-front sorted)
  VkPipelineDepthStencilStateCreateInfo ds{
    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable  = VK_FALSE;
  ds.depthWriteEnable = VK_FALSE;

  // Premultiplied alpha blend: src=ONE, dst=ONE_MINUS_SRC_ALPHA
  VkPipelineColorBlendAttachmentState blendAtt{};
  blendAtt.blendEnable         = VK_TRUE;
  blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blendAtt.colorBlendOp        = VK_BLEND_OP_ADD;
  blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blendAtt.alphaBlendOp        = VK_BLEND_OP_ADD;
  blendAtt.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo blend{
    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments    = &blendAtt;

  VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pci.stageCount          = 2;
  pci.pStages             = stages;
  pci.pVertexInputState   = &vertInput;
  pci.pInputAssemblyState = &inputAssembly;
  pci.pViewportState      = &viewportState;
  pci.pRasterizationState = &rast;
  pci.pMultisampleState   = &ms;
  pci.pDepthStencilState  = &ds;
  pci.pColorBlendState    = &blend;
  pci.pDynamicState       = &dynCI;
  pci.layout              = layout;
  pci.renderPass          = renderPass;
  pci.subpass             = 0;

  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline) != VK_SUCCESS)
    throw std::runtime_error("VulkanPipeline: vkCreateGraphicsPipelines failed");
}

void VulkanPipeline::destroy(VkDevice device) {
  if (pipeline) { vkDestroyPipeline(device, pipeline, nullptr); pipeline = VK_NULL_HANDLE; }
  if (layout)   { vkDestroyPipelineLayout(device, layout, nullptr); layout = VK_NULL_HANDLE; }
  if (descriptorSetLayout) {
    vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
    descriptorSetLayout = VK_NULL_HANDLE;
  }
  if (m_vert) { vkDestroyShaderModule(device, m_vert, nullptr); m_vert = VK_NULL_HANDLE; }
  if (m_frag) { vkDestroyShaderModule(device, m_frag, nullptr); m_frag = VK_NULL_HANDLE; }
}
