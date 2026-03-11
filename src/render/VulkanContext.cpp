#include "VulkanContext.hpp"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void vkCheck(VkResult r, const char* msg) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(msg) + " (VkResult=" + std::to_string(r) + ")");
}

// ---------------------------------------------------------------------------
// Validation layer support
// ---------------------------------------------------------------------------
#ifndef NDEBUG
static constexpr bool k_enableValidation       = true;
static constexpr const char* k_validationLayer = "VK_LAYER_KHRONOS_validation";

static VKAPI_ATTR VkBool32 VKAPI_CALL
debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
              VkDebugUtilsMessageTypeFlagsEXT /*type*/,
              const VkDebugUtilsMessengerCallbackDataEXT* data,
              void* /*userData*/) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
    return VK_FALSE;
}

static VkDebugUtilsMessengerCreateInfoEXT makeDebugMessengerCI() {
    VkDebugUtilsMessengerCreateInfoEXT ci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debugCallback;
    return ci;
}
#else
static constexpr bool k_enableValidation = false;
#endif

// ---------------------------------------------------------------------------
// VulkanContext::init
// ---------------------------------------------------------------------------
void VulkanContext::init(GLFWwindow* window) {
    m_window = window;
    createInstance();
#ifndef NDEBUG
    createDebugMessenger();
#endif
    createSurface(window);
    pickPhysicalDevice();
    createDevice();
    createAllocator();
    createCommandPool();
    createDescriptorPool();

    int w{}, h{};
    glfwGetFramebufferSize(window, &w, &h);
    createSwapchain(w, h);
    createSwapchainImageViews();
    createRenderPass();
    createFramebuffers();
    createFrameData();
}

// ---------------------------------------------------------------------------
// VulkanContext::destroy
// ---------------------------------------------------------------------------
void VulkanContext::destroy() {
    for (auto& f : frames) {
        vkDestroySemaphore(device, f.imageAvailableSemaphore, nullptr);
        vkDestroySemaphore(device, f.renderFinishedSemaphore, nullptr);
        vkDestroyFence(device, f.inFlightFence, nullptr);
    }
    cleanupSwapchain();
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vmaDestroyAllocator(allocator);
    vkDestroyDevice(device, nullptr);
#ifndef NDEBUG
    if (debugMessenger) {
        auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (fn)
            fn(instance, debugMessenger, nullptr);
    }
#endif
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
}

// ---------------------------------------------------------------------------
// VulkanContext::recreateSwapchain
// ---------------------------------------------------------------------------
void VulkanContext::recreateSwapchain(int width, int height) {
    cleanupSwapchain();
    createSwapchain(width, height);
    createSwapchainImageViews();
    createFramebuffers(); // render pass is reused, framebuffers need new image views
}

// ---------------------------------------------------------------------------
// Private: createInstance
// ---------------------------------------------------------------------------
void VulkanContext::createInstance() {
    // Required extensions from GLFW
    uint32_t glfwExtCount{};
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);

#ifndef NDEBUG
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName   = "World Imagine";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName        = "No Engine";
    appInfo.apiVersion         = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo        = &appInfo;
    ci.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();

#ifndef NDEBUG
    const char* layers[]   = {k_validationLayer};
    ci.enabledLayerCount   = 1;
    ci.ppEnabledLayerNames = layers;
    auto debugCI           = makeDebugMessengerCI();
    ci.pNext               = &debugCI;
#endif

    vkCheck(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");
}

// ---------------------------------------------------------------------------
// Private: createDebugMessenger
// ---------------------------------------------------------------------------
#ifndef NDEBUG
void VulkanContext::createDebugMessenger() {
    auto ci = makeDebugMessengerCI();
    auto fn =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,
                                                                  "vkCreateDebugUtilsMessengerEXT");
    if (fn)
        vkCheck(fn(instance, &ci, nullptr, &debugMessenger), "vkCreateDebugUtilsMessengerEXT");
}
#endif

// ---------------------------------------------------------------------------
// Private: createSurface
// ---------------------------------------------------------------------------
void VulkanContext::createSurface(GLFWwindow* window) {
    vkCheck(glfwCreateWindowSurface(instance, window, nullptr, &surface),
            "glfwCreateWindowSurface");
}

// ---------------------------------------------------------------------------
// Private: pickPhysicalDevice
// ---------------------------------------------------------------------------
void VulkanContext::pickPhysicalDevice() {
    uint32_t count{};
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0)
        throw std::runtime_error("No Vulkan physical devices found");

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    // Prefer discrete GPU, fall back to any device with a graphics queue
    auto scoreDevice = [&](VkPhysicalDevice d) -> int {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 1000 : 1;

        uint32_t qfc{};
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qfc, nullptr);
        std::vector<VkQueueFamilyProperties> qfps(qfc);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qfc, qfps.data());

        bool hasGraphics = false;
        for (uint32_t i = 0; i < qfc; ++i) {
            VkBool32 present{};
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &present);
            if ((qfps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
                hasGraphics = true;
        }
        return hasGraphics ? score : 0;
    };

    physicalDevice = *std::max_element(devices.begin(),
                                       devices.end(),
                                       [&](VkPhysicalDevice a, VkPhysicalDevice b) {
                                           return scoreDevice(a) < scoreDevice(b);
                                       });

    if (scoreDevice(physicalDevice) == 0)
        throw std::runtime_error("No suitable Vulkan device found");

    // Find graphics + present queue family
    uint32_t qfc{};
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfc, nullptr);
    std::vector<VkQueueFamilyProperties> qfps(qfc);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfc, qfps.data());

    for (uint32_t i = 0; i < qfc; ++i) {
        VkBool32 present{};
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &present);
        if ((qfps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            graphicsQueueFamily = i;
            break;
        }
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    printf("[Vulkan] Using device: %s\n", props.deviceName);
}

// ---------------------------------------------------------------------------
// Private: createDevice
// ---------------------------------------------------------------------------
void VulkanContext::createDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = graphicsQueueFamily;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &priority;

    const char* deviceExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount    = 1;
    ci.pQueueCreateInfos       = &qci;
    ci.enabledExtensionCount   = 1;
    ci.ppEnabledExtensionNames = deviceExts;
    ci.pEnabledFeatures        = &features;

    vkCheck(vkCreateDevice(physicalDevice, &ci, nullptr, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);
}

// ---------------------------------------------------------------------------
// Private: createAllocator
// ---------------------------------------------------------------------------
void VulkanContext::createAllocator() {
    VmaAllocatorCreateInfo ai{};
    ai.physicalDevice   = physicalDevice;
    ai.device           = device;
    ai.instance         = instance;
    ai.vulkanApiVersion = VK_API_VERSION_1_2;
    vkCheck(vmaCreateAllocator(&ai, &allocator), "vmaCreateAllocator");
}

// ---------------------------------------------------------------------------
// Private: createCommandPool
// ---------------------------------------------------------------------------
void VulkanContext::createCommandPool() {
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.queueFamilyIndex = graphicsQueueFamily;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCheck(vkCreateCommandPool(device, &ci, nullptr, &commandPool), "vkCreateCommandPool");
}

// ---------------------------------------------------------------------------
// Private: createDescriptorPool
// ---------------------------------------------------------------------------
void VulkanContext::createDescriptorPool() {
    VkDescriptorPoolSize poolSize{
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        128 // enough for ImGui fonts + future texture samplers
    };
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets       = 128;
    ci.poolSizeCount = 1;
    ci.pPoolSizes    = &poolSize;
    vkCheck(vkCreateDescriptorPool(device, &ci, nullptr, &descriptorPool),
            "vkCreateDescriptorPool");
}

// ---------------------------------------------------------------------------
// Private: createSwapchain
// ---------------------------------------------------------------------------
void VulkanContext::createSwapchain(int width, int height) {
    // Surface capabilities
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps);

    // Choose format: prefer B8G8R8A8_UNORM / SRGB_NONLINEAR
    uint32_t fmtCount{};
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &fmtCount, formats.data());

    VkSurfaceFormatKHR chosenFmt = formats[0];
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosenFmt = f;
            break;
        }
    }
    swapchainFormat = chosenFmt.format;

    // Choose present mode: prefer MAILBOX, fall back to FIFO
    uint32_t pmCount{};
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(pmCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice,
                                              surface,
                                              &pmCount,
                                              presentModes.data());

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto pm : presentModes) {
        if (pm == VK_PRESENT_MODE_MAILBOX_KHR) {
            presentMode = pm;
            break;
        }
    }

    // Choose extent
    if (caps.currentExtent.width != UINT32_MAX) {
        swapchainExtent = caps.currentExtent;
    } else {
        swapchainExtent = {std::clamp(static_cast<uint32_t>(width),
                                      caps.minImageExtent.width,
                                      caps.maxImageExtent.width),
                           std::clamp(static_cast<uint32_t>(height),
                                      caps.minImageExtent.height,
                                      caps.maxImageExtent.height)};
    }

    uint32_t imageCount =
        std::min(caps.minImageCount + 1, caps.maxImageCount > 0 ? caps.maxImageCount : UINT32_MAX);

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface          = surface;
    ci.minImageCount    = imageCount;
    ci.imageFormat      = swapchainFormat;
    ci.imageColorSpace  = chosenFmt.colorSpace;
    ci.imageExtent      = swapchainExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = presentMode;
    ci.clipped          = VK_TRUE;
    ci.oldSwapchain     = VK_NULL_HANDLE;

    vkCheck(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain), "vkCreateSwapchainKHR");

    uint32_t imgCount{};
    vkGetSwapchainImagesKHR(device, swapchain, &imgCount, nullptr);
    swapchainImages.resize(imgCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imgCount, swapchainImages.data());
}

// ---------------------------------------------------------------------------
// Private: createSwapchainImageViews
// ---------------------------------------------------------------------------
void VulkanContext::createSwapchainImageViews() {
    swapchainImageViews.resize(swapchainImages.size());
    for (size_t i = 0; i < swapchainImages.size(); ++i) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image                           = swapchainImages[i];
        ci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        ci.format                          = swapchainFormat;
        ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.baseMipLevel   = 0;
        ci.subresourceRange.levelCount     = 1;
        ci.subresourceRange.baseArrayLayer = 0;
        ci.subresourceRange.layerCount     = 1;
        vkCheck(vkCreateImageView(device, &ci, nullptr, &swapchainImageViews[i]),
                "vkCreateImageView");
    }
}

// ---------------------------------------------------------------------------
// Private: createRenderPass (for ImGui swapchain presentation)
// ---------------------------------------------------------------------------
void VulkanContext::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = swapchainFormat;
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1;
    ci.pAttachments    = &colorAttachment;
    ci.subpassCount    = 1;
    ci.pSubpasses      = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    vkCheck(vkCreateRenderPass(device, &ci, nullptr, &renderPass), "vkCreateRenderPass");
}

// ---------------------------------------------------------------------------
// Private: createFramebuffers
// ---------------------------------------------------------------------------
void VulkanContext::createFramebuffers() {
    framebuffers.resize(swapchainImageViews.size());
    for (size_t i = 0; i < swapchainImageViews.size(); ++i) {
        VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        ci.renderPass      = renderPass;
        ci.attachmentCount = 1;
        ci.pAttachments    = &swapchainImageViews[i];
        ci.width           = swapchainExtent.width;
        ci.height          = swapchainExtent.height;
        ci.layers          = 1;
        vkCheck(vkCreateFramebuffer(device, &ci, nullptr, &framebuffers[i]), "vkCreateFramebuffer");
    }
}

// ---------------------------------------------------------------------------
// Private: createFrameData
// ---------------------------------------------------------------------------
void VulkanContext::createFrameData() {
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool        = commandPool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkSemaphoreCreateInfo semCI{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenCI{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenCI.flags = VK_FENCE_CREATE_SIGNALED_BIT; // start signaled so first frame doesn't wait

    for (auto& f : frames) {
        vkCheck(vkAllocateCommandBuffers(device, &allocInfo, &f.commandBuffer),
                "vkAllocateCommandBuffers");
        vkCheck(vkCreateSemaphore(device, &semCI, nullptr, &f.imageAvailableSemaphore),
                "vkCreateSemaphore imageAvailable");
        vkCheck(vkCreateSemaphore(device, &semCI, nullptr, &f.renderFinishedSemaphore),
                "vkCreateSemaphore renderFinished");
        vkCheck(vkCreateFence(device, &fenCI, nullptr, &f.inFlightFence), "vkCreateFence");
    }
}

// ---------------------------------------------------------------------------
// Private: cleanupSwapchain
// ---------------------------------------------------------------------------
void VulkanContext::cleanupSwapchain() {
    for (auto fb : framebuffers)
        vkDestroyFramebuffer(device, fb, nullptr);
    framebuffers.clear();

    if (renderPass) {
        vkDestroyRenderPass(device, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
    }

    for (auto iv : swapchainImageViews)
        vkDestroyImageView(device, iv, nullptr);
    swapchainImageViews.clear();

    if (swapchain) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
}
