#include "SplatRenderer.hpp"
#include "VulkanContext.hpp"
#include "model/GaussianModel.hpp"

#include <imgui_impl_vulkan.h>
#include <stdexcept>
#include <cstring>
#include <torch/torch.h>

static constexpr VkFormat k_colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
static constexpr VkFormat k_depthFormat = VK_FORMAT_D32_SFLOAT;

// ---------------------------------------------------------------------------
// One-time command buffer helpers
// ---------------------------------------------------------------------------
VkCommandBuffer SplatRenderer::beginOneTime(VulkanContext& ctx) {
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool        = ctx.commandPool;
  ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  vkAllocateCommandBuffers(ctx.device, &ai, &cmd);

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);
  return cmd;
}

void SplatRenderer::endOneTime(VulkanContext& ctx, VkCommandBuffer cmd) {
  vkEndCommandBuffer(cmd);

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers    = &cmd;

  VkFence fence;
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  vkCreateFence(ctx.device, &fci, nullptr, &fence);

  vkQueueSubmit(ctx.graphicsQueue, 1, &si, fence);
  vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(ctx.device, fence, nullptr);
  vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &cmd);
}

// ---------------------------------------------------------------------------
// Image layout transition
// ---------------------------------------------------------------------------
void SplatRenderer::transitionColor(VkCommandBuffer      cmd,
                                    VkImageLayout        oldLayout,
                                    VkImageLayout        newLayout,
                                    VkPipelineStageFlags srcStage,
                                    VkPipelineStageFlags dstStage,
                                    VkAccessFlags        srcAccess,
                                    VkAccessFlags        dstAccess) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask               = srcAccess;
  b.dstAccessMask               = dstAccess;
  b.oldLayout                   = oldLayout;
  b.newLayout                   = newLayout;
  b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
  b.image                       = m_colorImage;
  b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  b.subresourceRange.levelCount = 1;
  b.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ---------------------------------------------------------------------------
// Offscreen images
// ---------------------------------------------------------------------------
void SplatRenderer::createOffscreenImages(VulkanContext& ctx, uint32_t w, uint32_t h) {
  auto makeImage = [&](VkFormat fmt,
                       VkImageUsageFlags usage,
                       VkImageAspectFlags aspect,
                       VkImage& image,
                       VkImageView& view,
                       VmaAllocation& alloc) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = fmt;
    ici.extent        = {w, h, 1};
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(ctx.allocator, &ici, &aci, &image, &alloc, nullptr) != VK_SUCCESS)
      throw std::runtime_error("SplatRenderer: vmaCreateImage failed");

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image                           = image;
    vci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    vci.format                          = fmt;
    vci.subresourceRange.aspectMask     = aspect;
    vci.subresourceRange.levelCount     = 1;
    vci.subresourceRange.layerCount     = 1;
    if (vkCreateImageView(ctx.device, &vci, nullptr, &view) != VK_SUCCESS)
      throw std::runtime_error("SplatRenderer: vkCreateImageView failed");
  };

  makeImage(k_colorFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            m_colorImage, m_colorView, m_colorAlloc);

  makeImage(k_depthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT,
            m_depthImage, m_depthView, m_depthAlloc);

  // Transition color image to SHADER_READ_ONLY so render pass initialLayout is satisfied
  {
    auto cmd = beginOneTime(ctx);
    transitionColor(cmd,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0,
                    VK_ACCESS_SHADER_READ_BIT);
    endOneTime(ctx, cmd);
  }
}

void SplatRenderer::destroyOffscreenImages(VulkanContext& ctx) {
  if (m_framebuffer) { vkDestroyFramebuffer(ctx.device, m_framebuffer, nullptr); m_framebuffer = VK_NULL_HANDLE; }
  if (m_colorView)   { vkDestroyImageView(ctx.device, m_colorView, nullptr); m_colorView = VK_NULL_HANDLE; }
  if (m_colorImage)  { vmaDestroyImage(ctx.allocator, m_colorImage, m_colorAlloc); m_colorImage = VK_NULL_HANDLE; }
  if (m_depthView)   { vkDestroyImageView(ctx.device, m_depthView, nullptr); m_depthView = VK_NULL_HANDLE; }
  if (m_depthImage)  { vmaDestroyImage(ctx.allocator, m_depthImage, m_depthAlloc); m_depthImage = VK_NULL_HANDLE; }
}

// ---------------------------------------------------------------------------
// Render pass
// ---------------------------------------------------------------------------
void SplatRenderer::createRenderPass(VulkanContext& ctx) {
  // Color: SHADER_READ_ONLY → COLOR_ATTACHMENT (during subpass) → SHADER_READ_ONLY
  VkAttachmentDescription colorDesc{};
  colorDesc.format         = k_colorFormat;
  colorDesc.samples        = VK_SAMPLE_COUNT_1_BIT;
  colorDesc.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorDesc.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
  colorDesc.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorDesc.initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  colorDesc.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentDescription depthDesc{};
  depthDesc.format         = k_depthFormat;
  depthDesc.samples        = VK_SAMPLE_COUNT_1_BIT;
  depthDesc.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depthDesc.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthDesc.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depthDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthDesc.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  depthDesc.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount    = 1;
  subpass.pColorAttachments       = &colorRef;
  subpass.pDepthStencilAttachment = &depthRef;

  // Dependencies to synchronize with ImGui sampling on previous/next frame
  VkSubpassDependency deps[2]{};
  deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
  deps[0].dstSubpass    = 0;
  deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  deps[1].srcSubpass    = 0;
  deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  VkAttachmentDescription attachments[] = {colorDesc, depthDesc};
  VkRenderPassCreateInfo  rpCI{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpCI.attachmentCount = 2;
  rpCI.pAttachments    = attachments;
  rpCI.subpassCount    = 1;
  rpCI.pSubpasses      = &subpass;
  rpCI.dependencyCount = 2;
  rpCI.pDependencies   = deps;

  if (vkCreateRenderPass(ctx.device, &rpCI, nullptr, &m_renderPass) != VK_SUCCESS)
    throw std::runtime_error("SplatRenderer: vkCreateRenderPass failed");
}

void SplatRenderer::createFramebuffer(VulkanContext& ctx, uint32_t w, uint32_t h) {
  VkImageView attachments[] = {m_colorView, m_depthView};
  VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fci.renderPass      = m_renderPass;
  fci.attachmentCount = 2;
  fci.pAttachments    = attachments;
  fci.width           = w;
  fci.height          = h;
  fci.layers          = 1;
  if (vkCreateFramebuffer(ctx.device, &fci, nullptr, &m_framebuffer) != VK_SUCCESS)
    throw std::runtime_error("SplatRenderer: vkCreateFramebuffer failed");
}

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------
void SplatRenderer::createDescriptorPool(VulkanContext& ctx) {
  VkDescriptorPoolSize sizes[2]{};
  sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  sizes[0].descriptorCount = 1;
  sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  sizes[1].descriptorCount = 1;

  VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  ci.maxSets       = 1;
  ci.poolSizeCount = 2;
  ci.pPoolSizes    = sizes;
  if (vkCreateDescriptorPool(ctx.device, &ci, nullptr, &m_descriptorPool) != VK_SUCCESS)
    throw std::runtime_error("SplatRenderer: vkCreateDescriptorPool failed");
}

void SplatRenderer::createDescriptors(VulkanContext& ctx) {
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool     = m_descriptorPool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts        = &m_pipeline.descriptorSetLayout;
  if (vkAllocateDescriptorSets(ctx.device, &ai, &m_descriptorSet) != VK_SUCCESS)
    throw std::runtime_error("SplatRenderer: vkAllocateDescriptorSets failed");
}

void SplatRenderer::updateDescriptors(VulkanContext& ctx) {
  VkDescriptorBufferInfo uboInfo{};
  uboInfo.buffer = m_cameraUBOBuf.buffer;
  uboInfo.offset = 0;
  uboInfo.range  = sizeof(CameraUBO);

  VkDescriptorBufferInfo ssboInfo{};
  ssboInfo.buffer = m_splatBuf.buffer;
  ssboInfo.offset = 0;
  ssboInfo.range  = VK_WHOLE_SIZE;

  VkWriteDescriptorSet writes[2]{};
  writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet          = m_descriptorSet;
  writes[0].dstBinding      = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[0].pBufferInfo     = &uboInfo;

  writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet          = m_descriptorSet;
  writes[1].dstBinding      = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo     = &ssboInfo;

  vkUpdateDescriptorSets(ctx.device, 2, writes, 0, nullptr);
}

// ---------------------------------------------------------------------------
// init / destroy / resize
// ---------------------------------------------------------------------------
void SplatRenderer::init(VulkanContext& ctx, uint32_t width, uint32_t height,
                         const std::string& shaderDir) {
  m_width  = width;
  m_height = height;

  createOffscreenImages(ctx, width, height);
  createRenderPass(ctx);
  createFramebuffer(ctx, width, height);

  // Pipeline (needs render pass)
  m_pipeline.create(ctx, m_renderPass,
                    shaderDir + "/splat.vert.spv",
                    shaderDir + "/splat.frag.spv");

  // Sampler
  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
  sci.addressModeU = sci.addressModeV = sci.addressModeW =
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(ctx.device, &sci, nullptr, &m_sampler) != VK_SUCCESS)
    throw std::runtime_error("SplatRenderer: vkCreateSampler failed");

  // Register offscreen image with ImGui
  m_imguiTexDescSet = ImGui_ImplVulkan_AddTexture(
    m_sampler, m_colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  // Camera UBO (persistently mapped)
  m_cameraUBOMapped = m_cameraUBOBuf.createMapped(
    ctx.allocator,
    sizeof(CameraUBO),
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

  // Placeholder 1-float SSBO (so descriptor set is always valid)
  m_splatBuf.create(ctx.allocator,
                    sizeof(float),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VMA_MEMORY_USAGE_GPU_ONLY);

  createDescriptorPool(ctx);
  createDescriptors(ctx);
  updateDescriptors(ctx);
}

void SplatRenderer::destroy(VulkanContext& ctx) {
  vkDeviceWaitIdle(ctx.device);

  if (m_imguiTexDescSet) {
    ImGui_ImplVulkan_RemoveTexture(m_imguiTexDescSet);
    m_imguiTexDescSet = VK_NULL_HANDLE;
  }

  m_pipeline.destroy(ctx.device);

  if (m_descriptorPool) { vkDestroyDescriptorPool(ctx.device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
  if (m_sampler)        { vkDestroySampler(ctx.device, m_sampler, nullptr); m_sampler = VK_NULL_HANDLE; }
  if (m_renderPass)     { vkDestroyRenderPass(ctx.device, m_renderPass, nullptr); m_renderPass = VK_NULL_HANDLE; }

  destroyOffscreenImages(ctx);
  m_splatBuf.destroy(ctx.allocator);
  m_cameraUBOBuf.destroy(ctx.allocator);
}

void SplatRenderer::resize(VulkanContext& ctx, uint32_t width, uint32_t height) {
  if (width == m_width && height == m_height) return;
  vkDeviceWaitIdle(ctx.device);

  m_width  = width;
  m_height = height;

  // Remove old ImGui texture registration
  if (m_imguiTexDescSet) {
    ImGui_ImplVulkan_RemoveTexture(m_imguiTexDescSet);
    m_imguiTexDescSet = VK_NULL_HANDLE;
  }

  destroyOffscreenImages(ctx);
  createOffscreenImages(ctx, width, height);
  createFramebuffer(ctx, width, height);

  m_imguiTexDescSet = ImGui_ImplVulkan_AddTexture(
    m_sampler, m_colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  // Update UBO descriptor (sampler/image changed)
  updateDescriptors(ctx);
}

// ---------------------------------------------------------------------------
// uploadSplats — depth-sort, flatten, stage to GPU
// ---------------------------------------------------------------------------
void SplatRenderer::uploadSplats(VulkanContext& ctx,
                                 const GaussianModel& model,
                                 const Camera& cam) {
  std::lock_guard lock{model.mutex};

  const size_t N = model.numSplats();
  if (N == 0) {
    m_splatCount = 0;
    return;
  }

  // Depth sort: back-to-front (descending distance = far-first for over blending)
  Vec3 camPos = cam.position();
  auto camPosTensor = torch::tensor({camPos.x, camPos.y, camPos.z});
  auto diffs  = model.positions - camPosTensor; // [N,3]
  auto depths = diffs.norm(2, 1);               // [N]
  auto idx    = depths.argsort(0, /*descending=*/true);

  auto sortedPos = model.positions.index_select(0, idx).contiguous().cpu();
  auto sortedSc  = model.scales.index_select(0, idx).contiguous().cpu();
  auto sortedRot = model.rotations.index_select(0, idx).contiguous().cpu();
  auto sortedOp  = model.opacities.index_select(0, idx).contiguous().cpu();

  torch::Tensor sortedDC;
  if (model.sh_coeffs.defined() && model.sh_coeffs.size(1) > 0) {
    sortedDC = model.sh_coeffs.index_select(0, idx).select(1, 0).contiguous().cpu(); // [N,3]
  }

  // Pack 14 floats per splat
  const size_t stride = 14;
  std::vector<float> flat(N * stride);

  const float* pPos = sortedPos.data_ptr<float>();
  const float* pSc  = sortedSc.data_ptr<float>();
  const float* pRot = sortedRot.data_ptr<float>();
  const float* pOp  = sortedOp.data_ptr<float>();
  const float* pDC  = sortedDC.defined() ? sortedDC.data_ptr<float>() : nullptr;

  for (size_t i = 0; i < N; ++i) {
    float* o  = flat.data() + i * stride;
    o[0] = pPos[i*3+0]; o[1] = pPos[i*3+1]; o[2] = pPos[i*3+2];
    o[3] = pSc [i*3+0]; o[4] = pSc [i*3+1]; o[5] = pSc [i*3+2];
    o[6] = pRot[i*4+0]; o[7] = pRot[i*4+1]; o[8] = pRot[i*4+2]; o[9] = pRot[i*4+3];
    o[10] = pOp[i];
    o[11] = pDC ? pDC[i*3+0] : 0.f;
    o[12] = pDC ? pDC[i*3+1] : 0.f;
    o[13] = pDC ? pDC[i*3+2] : 0.f;
  }

  VkDeviceSize byteSize = flat.size() * sizeof(float);

  // Recreate SSBO if size changed
  if (!m_splatBuf.valid() || m_splatBuf.size < byteSize) {
    m_splatBuf.destroy(ctx.allocator);
    m_splatBuf.create(ctx.allocator,
                      byteSize,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_GPU_ONLY);
    updateDescriptors(ctx);
  }

  // Staging buffer upload
  GpuBuffer staging;
  void* mapped = staging.createMapped(ctx.allocator, byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  std::memcpy(mapped, flat.data(), byteSize);
  vmaFlushAllocation(ctx.allocator, staging.alloc, 0, VK_WHOLE_SIZE);

  auto cmd = beginOneTime(ctx);

  VkBufferCopy region{0, 0, byteSize};
  vkCmdCopyBuffer(cmd, staging.buffer, m_splatBuf.buffer, 1, &region);

  VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer              = m_splatBuf.buffer;
  barrier.offset              = 0;
  barrier.size                = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                       0, 0, nullptr, 1, &barrier, 0, nullptr);

  endOneTime(ctx, cmd);
  staging.destroy(ctx.allocator);

  m_splatCount = N;
}

// ---------------------------------------------------------------------------
// render — record into provided command buffer
// ---------------------------------------------------------------------------
void SplatRenderer::render(VulkanContext& ctx,
                           VkCommandBuffer cmd,
                           const Camera& cam,
                           uint32_t width,
                           uint32_t height) {
  // Update camera UBO every frame
  float aspect = (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.f;
  CameraUBO ubo = cam.makeUBO(aspect);
  std::memcpy(m_cameraUBOMapped, &ubo, sizeof(ubo));
  vmaFlushAllocation(ctx.allocator, m_cameraUBOBuf.alloc, 0, sizeof(ubo));

  VkClearValue clears[2]{};
  clears[0].color.float32[0] = 0.f;
  clears[0].color.float32[1] = 0.f;
  clears[0].color.float32[2] = 0.f;
  clears[0].color.float32[3] = 0.f;
  clears[1].depthStencil     = {1.f, 0};

  VkRenderPassBeginInfo rpBI{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rpBI.renderPass      = m_renderPass;
  rpBI.framebuffer     = m_framebuffer;
  rpBI.renderArea      = {{0, 0}, {width, height}};
  rpBI.clearValueCount = 2;
  rpBI.pClearValues    = clears;

  vkCmdBeginRenderPass(cmd, &rpBI, VK_SUBPASS_CONTENTS_INLINE);

  if (m_splatCount > 0) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipeline.layout, 0, 1, &m_descriptorSet, 0, nullptr);

    VkViewport vp{0, 0, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
    VkRect2D   sc{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // Cap draw count to avoid GPU context-switch timeout (Xid 109) on large models.
    // Splats are sorted back-to-front; draw the closest ones via firstVertex offset.
    static constexpr size_t kMaxRenderSplats = 250'000;
    size_t drawCount = std::min(m_splatCount, kMaxRenderSplats);
    uint32_t firstVert = static_cast<uint32_t>((m_splatCount - drawCount) * 6);

    vkCmdDraw(cmd, static_cast<uint32_t>(drawCount * 6), 1, firstVert, 0);
  }

  vkCmdEndRenderPass(cmd);
}
