#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

namespace bro::platform {

class Window;

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    void init(Window& window);
    void cleanup();

    void recreateSwapchain(uint32_t width, uint32_t height);

    VkCommandBuffer beginFrame();
    void endFrame();

    // Accessors
    VkInstance getInstance() const { return m_instance; }
    VkDevice getDevice() const { return m_device; }
    VkPhysicalDevice getPhysicalDevice() const { return m_physicalDevice; }
    VkQueue getGraphicsQueue() const { return m_graphicsQueue; }
    VkSwapchainKHR getSwapchain() const { return m_swapchain; }
    VkFormat getSwapchainFormat() const { return m_swapchainFormat; }
    VkExtent2D getSwapchainExtent() const { return m_swapchainExtent; }
    VkRenderPass getRenderPass() const { return m_renderPass; }
    VkCommandBuffer getCurrentCommandBuffer() const { return m_currentCommandBuffer; }
    uint32_t getCurrentImageIndex() const { return m_currentImageIndex; }

private:
    void createInstance(Window& window);
    void createSurface(Window& window);
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createSwapchain(uint32_t width, uint32_t height);
    void createRenderPass();
    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void cleanupSwapchain();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0;
    uint32_t m_presentQueueFamily = 0;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_swapchainExtent = {0, 0};
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;

    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;

    // Sync objects (double-buffered)
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
    VkSemaphore m_imageAvailableSemaphores[MAX_FRAMES_IN_FLIGHT] = {};
    VkSemaphore m_renderFinishedSemaphores[MAX_FRAMES_IN_FLIGHT] = {};
    VkFence m_inFlightFences[MAX_FRAMES_IN_FLIGHT] = {};
    uint32_t m_currentFrame = 0;
    uint32_t m_currentImageIndex = 0;
    VkCommandBuffer m_currentCommandBuffer = VK_NULL_HANDLE;

    Window* m_window = nullptr;
    bool m_initialized = false;

#ifdef NDEBUG
    static constexpr bool m_enableValidationLayers = false;
#else
    static constexpr bool m_enableValidationLayers = true;
#endif
    static constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";
};

} // namespace bro::platform
