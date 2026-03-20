#include "SplatRenderer.hpp"
#include "VulkanContext.hpp"
#include "model/GaussianModel.hpp"

#include <imgui_impl_vulkan.h>
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <fstream>
#include <torch/torch.h>

static constexpr VkFormat k_colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
static constexpr VkFormat k_depthFormat = VK_FORMAT_D32_SFLOAT;

// ---------------------------------------------------------------------------
// SPIR-V loader (shared by graphics and compute)
// ---------------------------------------------------------------------------
VkShaderModule SplatRenderer::loadSpv(VkDevice device, const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        throw std::runtime_error("SplatRenderer: cannot open SPIR-V: " + path);
    auto size = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<char> buf(size);
    f.read(buf.data(), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode    = reinterpret_cast<const uint32_t*>(buf.data());
    VkShaderModule mod;
    if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS)
        throw std::runtime_error("SplatRenderer: vkCreateShaderModule failed: " + path);
    return mod;
}

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
    auto makeImage = [&](VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                         VkImage& image, VkImageView& view, VmaAllocation& alloc) {
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
        vci.image                       = image;
        vci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        vci.format                      = fmt;
        vci.subresourceRange.aspectMask = aspect;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(ctx.device, &vci, nullptr, &view) != VK_SUCCESS)
            throw std::runtime_error("SplatRenderer: vkCreateImageView failed");
    };

    makeImage(k_colorFormat,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
              VK_IMAGE_ASPECT_COLOR_BIT, m_colorImage, m_colorView, m_colorAlloc);

    makeImage(k_depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
              VK_IMAGE_ASPECT_DEPTH_BIT, m_depthImage, m_depthView, m_depthAlloc);

    // Transition color image to SHADER_READ_ONLY so render pass initialLayout is satisfied
    {
        auto cmd = beginOneTime(ctx);
        transitionColor(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, VK_ACCESS_SHADER_READ_BIT);
        endOneTime(ctx, cmd);
    }
}

void SplatRenderer::destroyOffscreenImages(VulkanContext& ctx) {
    if (m_framebuffer) {
        vkDestroyFramebuffer(ctx.device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }
    if (m_colorView) {
        vkDestroyImageView(ctx.device, m_colorView, nullptr);
        m_colorView = VK_NULL_HANDLE;
    }
    if (m_colorImage) {
        vmaDestroyImage(ctx.allocator, m_colorImage, m_colorAlloc);
        m_colorImage = VK_NULL_HANDLE;
    }
    if (m_depthView) {
        vkDestroyImageView(ctx.device, m_depthView, nullptr);
        m_depthView = VK_NULL_HANDLE;
    }
    if (m_depthImage) {
        vmaDestroyImage(ctx.allocator, m_depthImage, m_depthAlloc);
        m_depthImage = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Render pass
// ---------------------------------------------------------------------------
void SplatRenderer::createRenderPass(VulkanContext& ctx) {
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
    // Graphics: 1 UBO + 2 SSBOs (srcSplats + indexBuf)
    // Depth compute: 3 SSBOs (srcSplats, depthKeys, indices)
    // Sort compute: 2 SSBOs (depthKeys, indices)
    // Total: 1 UBO, 7 SSBOs, 3 sets
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = 1;
    sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[1].descriptorCount = 7;

    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets       = 3;
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

    VkDescriptorBufferInfo srcSplatInfo{};
    srcSplatInfo.buffer = m_srcSplatBuf.buffer;
    srcSplatInfo.offset = 0;
    srcSplatInfo.range  = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo indexInfo{};
    indexInfo.buffer = m_indexBuf.buffer;
    indexInfo.offset = 0;
    indexInfo.range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[3]{};
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
    writes[1].pBufferInfo     = &srcSplatInfo;

    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = m_descriptorSet;
    writes[2].dstBinding      = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo     = &indexInfo;

    vkUpdateDescriptorSets(ctx.device, 3, writes, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Compute pipelines
// ---------------------------------------------------------------------------
void SplatRenderer::createComputePipelines(VulkanContext& ctx, const std::string& shaderDir) {
    VkDevice device = ctx.device;

    // --- Depth compute pipeline ---
    {
        m_depthCompShader = loadSpv(device, shaderDir + "/sort_depth.comp.spv");

        // Descriptor set layout: binding 0 = srcSplats, 1 = depthKeys, 2 = indices
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding         = 2;
        bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo dslCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dslCI.bindingCount = 3;
        dslCI.pBindings    = bindings;
        if (vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &m_depthCompDSLayout) !=
            VK_SUCCESS)
            throw std::runtime_error("SplatRenderer: depth compute DS layout failed");

        // Push constants: camPos(vec4) + camFwd(vec4) + splatCount(uint) + paddedCount(uint)
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset     = 0;
        pcRange.size       = 40; // 2*vec4 + 2*uint = 32+8 = 40

        VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plCI.setLayoutCount         = 1;
        plCI.pSetLayouts            = &m_depthCompDSLayout;
        plCI.pushConstantRangeCount = 1;
        plCI.pPushConstantRanges    = &pcRange;
        if (vkCreatePipelineLayout(device, &plCI, nullptr, &m_depthCompLayout) != VK_SUCCESS)
            throw std::runtime_error("SplatRenderer: depth compute pipeline layout failed");

        VkComputePipelineCreateInfo cpCI{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        cpCI.stage.module = m_depthCompShader;
        cpCI.stage.pName  = "main";
        cpCI.layout       = m_depthCompLayout;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCI, nullptr,
                                     &m_depthCompPipeline) != VK_SUCCESS)
            throw std::runtime_error("SplatRenderer: depth compute pipeline failed");
    }

    // --- Bitonic sort compute pipeline ---
    {
        m_sortCompShader = loadSpv(device, shaderDir + "/sort_bitonic.comp.spv");

        // Descriptor set layout: binding 0 = depthKeys, 1 = indices
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo dslCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dslCI.bindingCount = 2;
        dslCI.pBindings    = bindings;
        if (vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &m_sortCompDSLayout) !=
            VK_SUCCESS)
            throw std::runtime_error("SplatRenderer: sort compute DS layout failed");

        // Push constants: k(uint) + j(uint) + paddedCount(uint) = 12 bytes
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset     = 0;
        pcRange.size       = 12;

        VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plCI.setLayoutCount         = 1;
        plCI.pSetLayouts            = &m_sortCompDSLayout;
        plCI.pushConstantRangeCount = 1;
        plCI.pPushConstantRanges    = &pcRange;
        if (vkCreatePipelineLayout(device, &plCI, nullptr, &m_sortCompLayout) != VK_SUCCESS)
            throw std::runtime_error("SplatRenderer: sort compute pipeline layout failed");

        VkComputePipelineCreateInfo cpCI{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        cpCI.stage.module = m_sortCompShader;
        cpCI.stage.pName  = "main";
        cpCI.layout       = m_sortCompLayout;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCI, nullptr,
                                     &m_sortCompPipeline) != VK_SUCCESS)
            throw std::runtime_error("SplatRenderer: sort compute pipeline failed");
    }

    // Allocate compute descriptor sets
    {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool     = m_descriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &m_depthCompDSLayout;
        if (vkAllocateDescriptorSets(ctx.device, &ai, &m_depthCompDS) != VK_SUCCESS)
            throw std::runtime_error("SplatRenderer: depth compute DS alloc failed");
    }
    {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool     = m_descriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &m_sortCompDSLayout;
        if (vkAllocateDescriptorSets(ctx.device, &ai, &m_sortCompDS) != VK_SUCCESS)
            throw std::runtime_error("SplatRenderer: sort compute DS alloc failed");
    }
}

void SplatRenderer::destroyComputePipelines(VulkanContext& ctx) {
    VkDevice device = ctx.device;
    if (m_depthCompPipeline) {
        vkDestroyPipeline(device, m_depthCompPipeline, nullptr);
        m_depthCompPipeline = VK_NULL_HANDLE;
    }
    if (m_depthCompLayout) {
        vkDestroyPipelineLayout(device, m_depthCompLayout, nullptr);
        m_depthCompLayout = VK_NULL_HANDLE;
    }
    if (m_depthCompDSLayout) {
        vkDestroyDescriptorSetLayout(device, m_depthCompDSLayout, nullptr);
        m_depthCompDSLayout = VK_NULL_HANDLE;
    }
    if (m_depthCompShader) {
        vkDestroyShaderModule(device, m_depthCompShader, nullptr);
        m_depthCompShader = VK_NULL_HANDLE;
    }
    if (m_sortCompPipeline) {
        vkDestroyPipeline(device, m_sortCompPipeline, nullptr);
        m_sortCompPipeline = VK_NULL_HANDLE;
    }
    if (m_sortCompLayout) {
        vkDestroyPipelineLayout(device, m_sortCompLayout, nullptr);
        m_sortCompLayout = VK_NULL_HANDLE;
    }
    if (m_sortCompDSLayout) {
        vkDestroyDescriptorSetLayout(device, m_sortCompDSLayout, nullptr);
        m_sortCompDSLayout = VK_NULL_HANDLE;
    }
    if (m_sortCompShader) {
        vkDestroyShaderModule(device, m_sortCompShader, nullptr);
        m_sortCompShader = VK_NULL_HANDLE;
    }
}

void SplatRenderer::updateComputeDescriptors(VulkanContext& ctx) {
    // Depth compute: binding 0 = srcSplats, 1 = depthKeys, 2 = indices
    {
        VkDescriptorBufferInfo srcInfo{};
        srcInfo.buffer = m_srcSplatBuf.buffer;
        srcInfo.offset = 0;
        srcInfo.range  = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo depthInfo{};
        depthInfo.buffer = m_depthKeyBuf.buffer;
        depthInfo.offset = 0;
        depthInfo.range  = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo idxInfo{};
        idxInfo.buffer = m_indexBuf.buffer;
        idxInfo.offset = 0;
        idxInfo.range  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_depthCompDS;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo     = &srcInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_depthCompDS;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo     = &depthInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_depthCompDS;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo     = &idxInfo;

        vkUpdateDescriptorSets(ctx.device, 3, writes, 0, nullptr);
    }

    // Sort compute: binding 0 = depthKeys, 1 = indices
    {
        VkDescriptorBufferInfo depthInfo{};
        depthInfo.buffer = m_depthKeyBuf.buffer;
        depthInfo.offset = 0;
        depthInfo.range  = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo idxInfo{};
        idxInfo.buffer = m_indexBuf.buffer;
        idxInfo.offset = 0;
        idxInfo.range  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_sortCompDS;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo     = &depthInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_sortCompDS;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo     = &idxInfo;

        vkUpdateDescriptorSets(ctx.device, 2, writes, 0, nullptr);
    }
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

    // Graphics pipeline (needs render pass)
    m_pipeline.create(ctx, m_renderPass, shaderDir + "/splat.vert.spv",
                      shaderDir + "/splat.frag.spv");

    // Sampler
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(ctx.device, &sci, nullptr, &m_sampler) != VK_SUCCESS)
        throw std::runtime_error("SplatRenderer: vkCreateSampler failed");

    // Register offscreen image with ImGui
    m_imguiTexDescSet = ImGui_ImplVulkan_AddTexture(m_sampler, m_colorView,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Camera UBO (persistently mapped)
    m_cameraUBOMapped =
        m_cameraUBOBuf.createMapped(ctx.allocator, sizeof(CameraUBO),
                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    // Placeholder 1-float SSBO so descriptor set is always valid
    m_srcSplatBuf.create(ctx.allocator, sizeof(float),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VMA_MEMORY_USAGE_GPU_ONLY);

    // Placeholder index buffer (1 uint32)
    m_indexBuf.create(ctx.allocator, sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VMA_MEMORY_USAGE_GPU_ONLY);

    // Placeholder depth key buffer (1 float)
    m_depthKeyBuf.create(ctx.allocator, sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VMA_MEMORY_USAGE_GPU_ONLY);

    createDescriptorPool(ctx);
    createDescriptors(ctx);
    updateDescriptors(ctx);

    // Compute pipelines (after descriptor pool so we can allocate compute DS)
    createComputePipelines(ctx, shaderDir);
    updateComputeDescriptors(ctx);
}

void SplatRenderer::destroy(VulkanContext& ctx) {
    vkDeviceWaitIdle(ctx.device);

    if (m_imguiTexDescSet) {
        ImGui_ImplVulkan_RemoveTexture(m_imguiTexDescSet);
        m_imguiTexDescSet = VK_NULL_HANDLE;
    }

    destroyComputePipelines(ctx);
    m_pipeline.destroy(ctx.device);

    if (m_descriptorPool) {
        vkDestroyDescriptorPool(ctx.device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_sampler) {
        vkDestroySampler(ctx.device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_renderPass) {
        vkDestroyRenderPass(ctx.device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }

    destroyOffscreenImages(ctx);
    m_srcSplatBuf.destroy(ctx.allocator);
    m_depthKeyBuf.destroy(ctx.allocator);
    m_indexBuf.destroy(ctx.allocator);
    m_cameraUBOBuf.destroy(ctx.allocator);
}

void SplatRenderer::resize(VulkanContext& ctx, uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    vkDeviceWaitIdle(ctx.device);

    m_width  = width;
    m_height = height;

    if (m_imguiTexDescSet) {
        ImGui_ImplVulkan_RemoveTexture(m_imguiTexDescSet);
        m_imguiTexDescSet = VK_NULL_HANDLE;
    }

    destroyOffscreenImages(ctx);
    createOffscreenImages(ctx, width, height);
    createFramebuffer(ctx, width, height);

    m_imguiTexDescSet = ImGui_ImplVulkan_AddTexture(m_sampler, m_colorView,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    updateDescriptors(ctx);
}

// ---------------------------------------------------------------------------
// uploadSplatData — pack unsorted splat data, stage to GPU
// ---------------------------------------------------------------------------
void SplatRenderer::uploadSplatData(VulkanContext& ctx, const GaussianModel& model) {
    std::lock_guard lock{model.mutex};

    const size_t N = model.numSplats();
    if (N == 0) {
        m_splatCount = 0;
        return;
    }

    // Pack 14 floats per splat in original order (no sorting)
    const size_t stride = 14;
    std::vector<float> flat(N * stride);

    auto pos = model.positions.contiguous().cpu();
    auto sc  = model.scales.contiguous().cpu();
    auto rot = model.rotations.contiguous().cpu();
    auto op  = model.opacities.contiguous().cpu();

    torch::Tensor dc;
    if (model.sh_coeffs.defined() && model.sh_coeffs.size(1) > 0) {
        dc = model.sh_coeffs.select(1, 0).contiguous().cpu(); // [N,3]
    }

    const float* pPos = pos.data_ptr<float>();
    const float* pSc  = sc.data_ptr<float>();
    const float* pRot = rot.data_ptr<float>();
    const float* pOp  = op.data_ptr<float>();
    const float* pDC  = dc.defined() ? dc.data_ptr<float>() : nullptr;

    for (size_t i = 0; i < N; ++i) {
        float* o = flat.data() + i * stride;
        o[0]  = pPos[i * 3 + 0];
        o[1]  = pPos[i * 3 + 1];
        o[2]  = pPos[i * 3 + 2];
        o[3]  = pSc[i * 3 + 0];
        o[4]  = pSc[i * 3 + 1];
        o[5]  = pSc[i * 3 + 2];
        o[6]  = pRot[i * 4 + 0];
        o[7]  = pRot[i * 4 + 1];
        o[8]  = pRot[i * 4 + 2];
        o[9]  = pRot[i * 4 + 3];
        o[10] = pOp[i];
        o[11] = pDC ? pDC[i * 3 + 0] : 0.f;
        o[12] = pDC ? pDC[i * 3 + 1] : 0.f;
        o[13] = pDC ? pDC[i * 3 + 2] : 0.f;
    }

    // Compute padded count (next power of 2)
    size_t padded = 1;
    while (padded < N) padded <<= 1;

    VkDeviceSize dataSize     = N * stride * sizeof(float);
    VkDeviceSize depthSize    = padded * sizeof(float);
    VkDeviceSize indexSize    = padded * sizeof(uint32_t);
    bool         needRealloc  = !m_srcSplatBuf.valid() || m_srcSplatBuf.size < dataSize ||
                                m_splatCountPadded != padded;

    if (needRealloc) {
        vkDeviceWaitIdle(ctx.device);

        m_srcSplatBuf.destroy(ctx.allocator);
        m_srcSplatBuf.create(ctx.allocator, dataSize,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY);

        m_depthKeyBuf.destroy(ctx.allocator);
        m_depthKeyBuf.create(ctx.allocator, depthSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY);

        m_indexBuf.destroy(ctx.allocator);
        m_indexBuf.create(ctx.allocator, indexSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          VMA_MEMORY_USAGE_GPU_ONLY);

        m_splatCountPadded = padded;

        updateDescriptors(ctx);
        updateComputeDescriptors(ctx);
    }

    // Staging buffer upload
    GpuBuffer staging;
    void* mapped = staging.createMapped(ctx.allocator, dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(mapped, flat.data(), dataSize);
    vmaFlushAllocation(ctx.allocator, staging.alloc, 0, VK_WHOLE_SIZE);

    auto cmd = beginOneTime(ctx);

    VkBufferCopy region{0, 0, dataSize};
    vkCmdCopyBuffer(cmd, staging.buffer, m_srcSplatBuf.buffer, 1, &region);

    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer              = m_srcSplatBuf.buffer;
    barrier.offset              = 0;
    barrier.size                = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &barrier, 0,
                         nullptr);

    endOneTime(ctx, cmd);
    staging.destroy(ctx.allocator);

    m_splatCount = N;
}

// ---------------------------------------------------------------------------
// gpuSort — record compute dispatches for depth sort
// ---------------------------------------------------------------------------
void SplatRenderer::gpuSort(VkCommandBuffer cmd, glm::vec3 camPos, glm::vec3 camFwd) {
    uint32_t padded = static_cast<uint32_t>(m_splatCountPadded);

    // --- Pass 1: Compute depths + initialize indices ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_depthCompPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_depthCompLayout, 0, 1,
                            &m_depthCompDS, 0, nullptr);

    struct DepthPC {
        float camPos[4];
        float camFwd[4];
        uint32_t splatCount;
        uint32_t paddedCount;
    } depthPC;
    depthPC.camPos[0]    = camPos.x;
    depthPC.camPos[1]    = camPos.y;
    depthPC.camPos[2]    = camPos.z;
    depthPC.camPos[3]    = 0.f;
    depthPC.camFwd[0]    = camFwd.x;
    depthPC.camFwd[1]    = camFwd.y;
    depthPC.camFwd[2]    = camFwd.z;
    depthPC.camFwd[3]    = 0.f;
    depthPC.splatCount   = static_cast<uint32_t>(m_splatCount);
    depthPC.paddedCount  = padded;

    vkCmdPushConstants(cmd, m_depthCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(depthPC),
                       &depthPC);

    uint32_t depthGroups = (padded + 255) / 256;
    vkCmdDispatch(cmd, depthGroups, 1, 1);

    // Barrier: depth compute writes → sort compute reads
    VkMemoryBarrier memBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memBarrier, 0, nullptr, 0,
                         nullptr);

    // --- Pass 2: Bitonic sort ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sortCompPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sortCompLayout, 0, 1,
                            &m_sortCompDS, 0, nullptr);

    uint32_t sortGroups = (padded + 255) / 256; // one thread per element, half do work

    struct SortPC {
        uint32_t k;
        uint32_t j;
        uint32_t paddedCount;
    } sortPC;
    sortPC.paddedCount = padded;

    for (uint32_t k = 2; k <= padded; k <<= 1) {
        for (uint32_t j = k >> 1; j > 0; j >>= 1) {
            sortPC.k = k;
            sortPC.j = j;
            vkCmdPushConstants(cmd, m_sortCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(sortPC), &sortPC);
            vkCmdDispatch(cmd, sortGroups, 1, 1);

            // Barrier between sort passes
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memBarrier, 0,
                                 nullptr, 0, nullptr);
        }
    }

    // Final barrier: compute writes → vertex shader reads
    VkMemoryBarrier finalBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    finalBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    finalBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &finalBarrier, 0, nullptr, 0,
                         nullptr);
}

// ---------------------------------------------------------------------------
// render — GPU sort + record into provided command buffer
// ---------------------------------------------------------------------------
void SplatRenderer::render(VulkanContext& ctx, VkCommandBuffer cmd, const CameraUBO& ubo,
                           uint32_t width, uint32_t height) {
    // Upload camera UBO every frame
    std::memcpy(m_cameraUBOMapped, &ubo, sizeof(ubo));
    vmaFlushAllocation(ctx.allocator, m_cameraUBOBuf.alloc, 0, sizeof(ubo));

    // GPU depth sort (before render pass)
    if (m_splatCount > 0) {
        // Extract camera forward from view matrix: -third column of rotation part
        glm::vec3 camPos{ubo.camPos.x, ubo.camPos.y, ubo.camPos.z};
        glm::vec3 camFwd{-ubo.view[0][2], -ubo.view[1][2], -ubo.view[2][2]};
        gpuSort(cmd, camPos, camFwd);
    }

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
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.layout, 0, 1,
                                &m_descriptorSet, 0, nullptr);

        VkViewport vp{0, 0, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
        VkRect2D   sc{{0, 0}, {width, height}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        // Cap draw count to avoid GPU context-switch timeout on large models.
        // Splats are sorted back-to-front in the index buffer; draw the closest ones
        // via firstVertex offset. The padded elements have -FLT_MAX depth and sort
        // to the tail (highest indices), so they're naturally beyond splatCount and
        // never drawn.
        static constexpr size_t kMaxRenderSplats = 1'250'000;
        size_t drawCount = std::min(m_splatCount, kMaxRenderSplats);
        uint32_t firstVert = static_cast<uint32_t>((m_splatCount - drawCount) * 6);

        vkCmdDraw(cmd, static_cast<uint32_t>(drawCount * 6), 1, firstVert, 0);
    }

    vkCmdEndRenderPass(cmd);
}
