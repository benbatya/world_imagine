#pragma once
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <vector>
#include <array>
#include <cstdint>

struct GLFWwindow;

static constexpr uint32_t FRAMES_IN_FLIGHT = 2;

struct FrameData {
    VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
    VkSemaphore     imageAvailableSemaphore{VK_NULL_HANDLE};
    VkSemaphore     renderFinishedSemaphore{VK_NULL_HANDLE};
    VkFence         inFlightFence{VK_NULL_HANDLE};
};

class VulkanContext {
public:
    void init(GLFWwindow* window);
    void destroy();
    void recreateSwapchain(int width, int height);

    // --- Core Vulkan handles ---
    VkInstance       instance{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice         device{VK_NULL_HANDLE};
    VkQueue          graphicsQueue{VK_NULL_HANDLE};
    uint32_t         graphicsQueueFamily{0};

    // --- Surface & swapchain ---
    VkSurfaceKHR              surface{VK_NULL_HANDLE};
    VkSwapchainKHR            swapchain{VK_NULL_HANDLE};
    VkFormat                  swapchainFormat{VK_FORMAT_UNDEFINED};
    VkExtent2D                swapchainExtent{};
    std::vector<VkImage>      swapchainImages;
    std::vector<VkImageView>  swapchainImageViews;

    // --- Render pass & framebuffers (for ImGui swapchain presentation) ---
    VkRenderPass               renderPass{VK_NULL_HANDLE};
    std::vector<VkFramebuffer> framebuffers;

    // --- Commands & sync ---
    VkCommandPool                        commandPool{VK_NULL_HANDLE};
    std::array<FrameData, FRAMES_IN_FLIGHT> frames{};
    uint32_t                             currentFrame{0};

    // --- Descriptor pool (shared by ImGui + future renderers) ---
    VkDescriptorPool descriptorPool{VK_NULL_HANDLE};

    // --- VMA allocator ---
    VmaAllocator allocator{VK_NULL_HANDLE};

#ifndef NDEBUG
    VkDebugUtilsMessengerEXT debugMessenger{VK_NULL_HANDLE};
#endif

private:
    void createInstance();
    void createDebugMessenger();
    void createSurface(GLFWwindow* window);
    void pickPhysicalDevice();
    void createDevice();
    void createAllocator();
    void createCommandPool();
    void createDescriptorPool();
    void createSwapchain(int width, int height);
    void createSwapchainImageViews();
    void createRenderPass();
    void createFramebuffers();
    void createFrameData();

    void cleanupSwapchain();

    GLFWwindow* m_window{nullptr};
};
