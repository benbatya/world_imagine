#include "MainWindow.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

// GLFW must be included AFTER Vulkan headers
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cstdio>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void vkCheck(VkResult r) {
    if (r != VK_SUCCESS)
        throw std::runtime_error("ImGui Vulkan check failed (VkResult=" + std::to_string(r) + ")");
}

void MainWindow::framebufferResizeCallback(GLFWwindow* window, int /*w*/, int /*h*/) {
    auto* self      = static_cast<MainWindow*>(glfwGetWindowUserPointer(window));
    self->m_resized = true;
}

// ---------------------------------------------------------------------------
// MainWindow::init
// ---------------------------------------------------------------------------
void MainWindow::init(const char* title, int width, int height) {
    if (!glfwInit())
        throw std::runtime_error("glfwInit failed");

    // No OpenGL context — we manage Vulkan ourselves
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!m_window)
        throw std::runtime_error("glfwCreateWindow failed");

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback);

    // --- Vulkan context ---
    m_ctx.init(m_window);

    // --- Dear ImGui ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Dark theme
    ImGui::StyleColorsDark();
    ImGuiStyle& style    = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding  = 3.0f;
    style.GrabRounding   = 3.0f;
    style.Alpha          = 0.95f;

    // GLFW backend
    ImGui_ImplGlfw_InitForVulkan(m_window, /*install_callbacks=*/true);

    // Vulkan backend
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance                     = m_ctx.instance;
    initInfo.PhysicalDevice               = m_ctx.physicalDevice;
    initInfo.Device                       = m_ctx.device;
    initInfo.QueueFamily                  = m_ctx.graphicsQueueFamily;
    initInfo.Queue                        = m_ctx.graphicsQueue;
    initInfo.DescriptorPool               = m_ctx.descriptorPool;
    initInfo.PipelineInfoMain.RenderPass  = m_ctx.renderPass;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.MinImageCount                = 2;
    initInfo.ImageCount                   = static_cast<uint32_t>(m_ctx.swapchainImages.size());
    initInfo.CheckVkResultFn              = [](VkResult r) { vkCheck(r); };

    ImGui_ImplVulkan_Init(&initInfo);
    // Note: fonts are uploaded automatically by ImGui_ImplVulkan_Init in this ImGui version

    printf("[MainWindow] Initialized (%dx%d)\n", width, height);
}

// ---------------------------------------------------------------------------
// MainWindow::destroy
// ---------------------------------------------------------------------------
void MainWindow::destroy() {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    m_ctx.destroy();

    glfwDestroyWindow(m_window);
    glfwTerminate();
}

bool MainWindow::shouldClose() const {
    return glfwWindowShouldClose(m_window);
}
