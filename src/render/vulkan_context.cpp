#include "render/vulkan_context.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include "render/vulkan_debug.h"

namespace bro::render {

namespace {

const std::vector<const char*> kRequiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

} // namespace

VulkanContext::VulkanContext(const VulkanContextConfig& config)
    : config_(config)
{
    // Environment variable overrides: accept BRO_VK_VALIDATION or BRO_VULKAN_VALIDATION
    if (const char* val = std::getenv("BRO_VK_VALIDATION")) {
        config_.enableValidation = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0 || strcmp(val, "TRUE") == 0);
    } else if (const char* val2 = std::getenv("BRO_VULKAN_VALIDATION")) {
        config_.enableValidation = (strcmp(val2, "1") == 0 || strcmp(val2, "true") == 0 || strcmp(val2, "TRUE") == 0);
    }
    if (const char* dev = std::getenv("BRO_VK_DEVICE")) {
        config_.preferredDeviceIndex = std::atoi(dev);
    } else if (const char* dev2 = std::getenv("BRO_VULKAN_DEVICE")) {
        config_.preferredDeviceIndex = std::atoi(dev2);
    }
}

VulkanContext::~VulkanContext() {
    cleanup();
}

bool VulkanContext::init(VkSurfaceKHR compatibleSurface) {
    if (!createInstance()) {
        LOG_ERROR("Vulkan: Failed to create VkInstance");
        return false;
    }

    if (config_.enableValidation) {
        setupDebugMessenger();
    }

    if (!selectPhysicalDevice(compatibleSurface)) {
        LOG_ERROR("Vulkan: Failed to select suitable physical device");
        cleanup();
        return false;
    }

    if (!createLogicalDevice()) {
        LOG_ERROR("Vulkan: Failed to create logical device");
        cleanup();
        return false;
    }

    if (!createCommandPool()) {
        LOG_ERROR("Vulkan: Failed to create command pool");
        cleanup();
        return false;
    }

    LOG_INFO("Vulkan: Initialized successfully on device: %s (Driver %u.%u.%u)",
             deviceProperties_.deviceName,
             VK_VERSION_MAJOR(deviceProperties_.driverVersion),
             VK_VERSION_MINOR(deviceProperties_.driverVersion),
             VK_VERSION_PATCH(deviceProperties_.driverVersion));
    return true;
}

void VulkanContext::cleanup() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        memoryPool_.cleanup(device_);
    }

    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }

    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (debugMessenger_ != VK_NULL_HANDLE) {
        destroyDebugUtilsMessenger(instance_, debugMessenger_, nullptr);
        debugMessenger_ = VK_NULL_HANDLE;
    }

    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

bool VulkanContext::createInstance() {
    std::vector<const char*> validationLayers;
    if (config_.enableValidation) {
        validationLayers = getAvailableValidationLayers();
        if (validationLayers.empty() || !checkValidationLayerSupport(validationLayers)) {
            LOG_WARN("Vulkan: Validation layers requested, but not available on system");
            config_.enableValidation = false;
            validationLayers.clear();
        }
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Bro";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "BroEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);

    uint32_t instanceApiVersion = VK_API_VERSION_1_2;
    auto enumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerateInstanceVersion) {
        enumerateInstanceVersion(&instanceApiVersion);
    }
    appInfo.apiVersion = (instanceApiVersion >= VK_API_VERSION_1_3) ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;

    std::vector<const char*> extensions;

    // Enumerate available instance extensions
    uint32_t availExtCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availExtCount, nullptr);
    std::vector<VkExtensionProperties> availExts(availExtCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availExtCount, availExts.data());

    auto hasExt = [&](const char* name) {
        for (const auto& ext : availExts) {
            if (strcmp(ext.extensionName, name) == 0) return true;
        }
        return false;
    };

    // If not headless, query SDL for platform surface extensions
    if (!config_.headless) {
        uint32_t sdlExtCount = 0;
        char const* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
        if (sdlExtensions) {
            for (uint32_t i = 0; i < sdlExtCount; ++i) {
                extensions.push_back(sdlExtensions[i]);
            }
        } else {
            LOG_WARN("SDL_Vulkan_GetInstanceExtensions returned null: %s", SDL_GetError());
            if (hasExt(VK_KHR_SURFACE_EXTENSION_NAME)) {
                extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
            }
        }
    }

    if (config_.enableValidation && hasExt(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    // MoltenVK portability on macOS
    bool enablePortability = false;
    if (hasExt(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        enablePortability = true;
    }

    for (const auto& ext : config_.extraInstanceExtensions) {
        if (hasExt(ext.c_str())) {
            extensions.push_back(ext.c_str());
        }
    }

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    if (enablePortability) {
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (config_.enableValidation) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();
        populateDebugMessengerCreateInfo(debugCreateInfo);
        debugCreateInfo.pNext = createInfo.pNext;
        createInfo.pNext = &debugCreateInfo;
    } else {
        createInfo.enabledLayerCount = 0;
    }

    VkResult res = vkCreateInstance(&createInfo, nullptr, &instance_);
    return (res == VK_SUCCESS);
}

bool VulkanContext::setupDebugMessenger() {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    populateDebugMessengerCreateInfo(createInfo);
    return createDebugUtilsMessenger(instance_, &createInfo, nullptr, &debugMessenger_) == VK_SUCCESS;
}

VulkanQueueFamilyIndices VulkanContext::findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface) {
    VulkanQueueFamilyIndices indices;
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        const auto& qf = queueFamilies[i];
        if (qf.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            if (indices.graphicsFamily < 0) {
                indices.graphicsFamily = static_cast<int>(i);
            }
        }
        if (qf.queueFlags & VK_QUEUE_COMPUTE_BIT) {
            if (indices.computeFamily < 0) {
                indices.computeFamily = static_cast<int>(i);
            }
        }
        if (qf.queueFlags & VK_QUEUE_TRANSFER_BIT) {
            if (indices.transferFamily < 0 || !(qf.queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                indices.transferFamily = static_cast<int>(i);
            }
        }

        if (surface != VK_NULL_HANDLE) {
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
            if (presentSupport && indices.presentFamily < 0) {
                indices.presentFamily = static_cast<int>(i);
            }
        }
    }

    if (surface == VK_NULL_HANDLE) {
        indices.presentFamily = indices.graphicsFamily;
    }
    if (indices.transferFamily < 0) {
        indices.transferFamily = indices.graphicsFamily;
    }
    if (indices.computeFamily < 0) {
        indices.computeFamily = indices.graphicsFamily;
    }

    return indices;
}

bool VulkanContext::selectPhysicalDevice(VkSurfaceKHR compatibleSurface) {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (deviceCount == 0) {
        LOG_ERROR("Vulkan: No physical devices with Vulkan support found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

    int bestScore = -1;
    VkPhysicalDevice chosenDevice = VK_NULL_HANDLE;
    VulkanQueueFamilyIndices chosenIndices;

    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDevice dev = devices[i];
        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceFeatures feats;
        vkGetPhysicalDeviceProperties(dev, &props);
        vkGetPhysicalDeviceFeatures(dev, &feats);

        VulkanQueueFamilyIndices indices = findQueueFamilies(dev, compatibleSurface);
        if (!indices.isComplete(!config_.headless && compatibleSurface != VK_NULL_HANDLE)) {
            continue;
        }

        // Check required device extensions
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> availExts(extCount);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, availExts.data());

        bool extensionsSupported = true;
        if (!config_.headless) {
            for (const char* reqExt : kRequiredDeviceExtensions) {
                bool found = false;
                for (const auto& ext : availExts) {
                    if (strcmp(ext.extensionName, reqExt) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    extensionsSupported = false;
                    break;
                }
            }
        }
        if (!extensionsSupported) continue;

        if (config_.preferredDeviceIndex >= 0) {
            if (static_cast<int>(i) == config_.preferredDeviceIndex) {
                chosenDevice = dev;
                chosenIndices = indices;
                break;
            }
            continue;
        }

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 500;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) score += 200;
        else score += 100;

        if (score > bestScore) {
            bestScore = score;
            chosenDevice = dev;
            chosenIndices = indices;
        }
    }

    if (chosenDevice == VK_NULL_HANDLE) {
        LOG_ERROR("Vulkan: Failed to find a suitable physical device");
        return false;
    }

    physicalDevice_ = chosenDevice;
    queueIndices_ = chosenIndices;
    vkGetPhysicalDeviceProperties(physicalDevice_, &deviceProperties_);
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);
    vkGetPhysicalDeviceFeatures(physicalDevice_, &deviceFeatures_);
    return true;
}

bool VulkanContext::createLogicalDevice() {
    std::set<int> uniqueQueueFamilies = {
        queueIndices_.graphicsFamily,
        queueIndices_.presentFamily,
        queueIndices_.computeFamily,
        queueIndices_.transferFamily
    };

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    float queuePriority = 1.0f;
    for (int queueFamily : uniqueQueueFamilies) {
        if (queueFamily < 0) continue;
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = static_cast<uint32_t>(queueFamily);
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    std::vector<const char*> enabledExtensions;
    if (!config_.headless) {
        for (const char* ext : kRequiredDeviceExtensions) {
            enabledExtensions.push_back(ext);
        }
    }

    // Check for optional portability subset (MoltenVK)
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availExts(extCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, availExts.data());
    for (const auto& ext : availExts) {
        if (strcmp(ext.extensionName, "VK_KHR_portability_subset") == 0) {
            enabledExtensions.push_back("VK_KHR_portability_subset");
            break;
        }
    }

    for (const auto& ext : config_.extraDeviceExtensions) {
        enabledExtensions.push_back(ext.c_str());
    }

    VkPhysicalDeviceVulkan13Features vulkan13Features{};
    vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13Features.dynamicRendering = VK_TRUE;
    vulkan13Features.synchronization2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features vulkan12Features{};
    vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12Features.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timelineFeaturesKHR{};
    timelineFeaturesKHR.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR;
    timelineFeaturesKHR.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynFeaturesKHR{};
    dynFeaturesKHR.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
    dynFeaturesKHR.dynamicRendering = VK_TRUE;

    void* pNextChain = nullptr;
    if (deviceProperties_.apiVersion >= VK_API_VERSION_1_2) {
        vulkan12Features.pNext = pNextChain;
        pNextChain = &vulkan12Features;
    } else {
        bool hasTimelineExt = false;
        for (const auto& ext : availExts) {
            if (strcmp(ext.extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0) {
                hasTimelineExt = true;
                break;
            }
        }
        if (hasTimelineExt) {
            enabledExtensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
            timelineFeaturesKHR.pNext = pNextChain;
            pNextChain = &timelineFeaturesKHR;
        }
    }

    if (config_.enableDynamicRendering) {
        if (deviceProperties_.apiVersion >= VK_API_VERSION_1_3) {
            vulkan13Features.pNext = pNextChain;
            pNextChain = &vulkan13Features;
        } else {
            bool hasDynExt = false;
            for (const char* ext : enabledExtensions) {
                if (strcmp(ext, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0) {
                    hasDynExt = true;
                    break;
                }
            }
            if (!hasDynExt) {
                enabledExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
            }
            dynFeaturesKHR.pNext = pNextChain;
            pNextChain = &dynFeaturesKHR;
        }
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = deviceFeatures_.samplerAnisotropy;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = pNextChain;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();

    if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
        return false;
    }

    vkGetDeviceQueue(device_, queueIndices_.graphicsFamily, 0, &graphicsQueue_);
    if (queueIndices_.presentFamily >= 0) {
        vkGetDeviceQueue(device_, queueIndices_.presentFamily, 0, &presentQueue_);
    } else {
        presentQueue_ = graphicsQueue_;
    }
    if (queueIndices_.computeFamily >= 0) {
        vkGetDeviceQueue(device_, queueIndices_.computeFamily, 0, &computeQueue_);
    } else {
        computeQueue_ = graphicsQueue_;
    }
    if (queueIndices_.transferFamily >= 0) {
        vkGetDeviceQueue(device_, queueIndices_.transferFamily, 0, &transferQueue_);
    } else {
        transferQueue_ = graphicsQueue_;
    }

    return true;
}

bool VulkanContext::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueIndices_.graphicsFamily;

    return vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) == VK_SUCCESS;
}

VkCommandBuffer VulkanContext::beginSingleTimeCommands() const {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool_;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void VulkanContext::endSingleTimeCommands(VkCommandBuffer commandBuffer) const {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue_);

    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
}

uint32_t VulkanContext::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
    for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
        if ((typeFilter & (1 << i)) &&
            (memoryProperties_.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    LOG_ERROR("Vulkan: Failed to find suitable memory type for flags 0x%x", properties);
    return 0;
}

bool VulkanContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags properties,
                                 VkBuffer& buffer, VkDeviceMemory& memory,
                                 VkDeviceSize& outOffset, uint64_t& outAllocId,
                                 void*& outMappedData) {
    if (size == 0) return false;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        LOG_ERROR("VulkanContext: Failed to create buffer of size %zu", static_cast<size_t>(size));
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device_, buffer, &memRequirements);

    uint32_t memType = findMemoryType(memRequirements.memoryTypeBits, properties);

    if (!memoryPool_.allocate(device_, memRequirements.size, memRequirements.alignment,
                              memType, properties, /*isImage=*/false,
                              outAllocId, memory, outOffset, outMappedData)) {
        LOG_ERROR("VulkanContext: Failed to allocate pooled memory for buffer (%zu bytes)", static_cast<size_t>(memRequirements.size));
        vkDestroyBuffer(device_, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindBufferMemory(device_, buffer, memory, outOffset) != VK_SUCCESS) {
        LOG_ERROR("VulkanContext: Failed to bind buffer memory at offset %zu", static_cast<size_t>(outOffset));
        vkDestroyBuffer(device_, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        memoryPool_.free(device_, outAllocId);
        outAllocId = 0;
        memory = VK_NULL_HANDLE;
        outOffset = 0;
        outMappedData = nullptr;
        return false;
    }

    return true;
}

void VulkanContext::destroyBuffer(VkBuffer buffer, uint64_t allocId) {
    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer, nullptr);
    }
    if (allocId != 0) {
        memoryPool_.free(device_, allocId);
    }
}

bool VulkanContext::createImage(uint32_t width, uint32_t height, VkFormat format,
                                VkImageTiling tiling, VkImageUsageFlags usage,
                                VkMemoryPropertyFlags properties,
                                VkImage& image, VkDeviceMemory& memory,
                                VkDeviceSize& outOffset, uint64_t& outAllocId,
                                uint32_t mipLevels, uint32_t arrayLayers,
                                VkImageCreateFlags flags) {
    if (width == 0 || height == 0 || arrayLayers == 0) return false;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = flags;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = arrayLayers;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        LOG_ERROR("VulkanContext: Failed to create image (%ux%u)", width, height);
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device_, image, &memRequirements);

    uint32_t memType = findMemoryType(memRequirements.memoryTypeBits, properties);
    void* dummyMapped = nullptr;

    if (!memoryPool_.allocate(device_, memRequirements.size, memRequirements.alignment,
                              memType, properties, /*isImage=*/true,
                              outAllocId, memory, outOffset, dummyMapped)) {
        LOG_ERROR("VulkanContext: Failed to allocate pooled memory for image");
        vkDestroyImage(device_, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindImageMemory(device_, image, memory, outOffset) != VK_SUCCESS) {
        LOG_ERROR("VulkanContext: Failed to bind image memory at offset %zu", static_cast<size_t>(outOffset));
        vkDestroyImage(device_, image, nullptr);
        image = VK_NULL_HANDLE;
        memoryPool_.free(device_, outAllocId);
        outAllocId = 0;
        memory = VK_NULL_HANDLE;
        outOffset = 0;
        return false;
    }

    return true;
}

void VulkanContext::destroyImage(VkImage image, uint64_t allocId) {
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image, nullptr);
    }
    if (allocId != 0) {
        memoryPool_.free(device_, allocId);
    }
}

bool VulkanContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags properties,
                                 VkBuffer& buffer, VkDeviceMemory& bufferMemory) const
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device_, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device_, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(device_, buffer, bufferMemory, 0);
    return true;
}

bool VulkanContext::createImage(uint32_t width, uint32_t height, VkFormat format,
                                VkImageTiling tiling, VkImageUsageFlags usage,
                                VkMemoryPropertyFlags properties,
                                VkImage& image, VkDeviceMemory& imageMemory,
                                uint32_t mipLevels, uint32_t arrayLayers,
                                VkImageCreateFlags flags) const
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = flags;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = arrayLayers;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device_, image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device_, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
        vkDestroyImage(device_, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    vkBindImageMemory(device_, image, imageMemory, 0);
    return true;
}

void VulkanContext::copyBufferToImage(VkBuffer buffer, VkImage image,
                                      uint32_t width, uint32_t height,
                                      VkCommandBuffer cmd,
                                      uint32_t mipLevel,
                                      uint32_t baseArrayLayer,
                                      uint32_t layerCount) const
{
    bool ownsCmd = (cmd == VK_NULL_HANDLE);
    if (ownsCmd) cmd = beginSingleTimeCommands();

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = mipLevel;
    region.imageSubresource.baseArrayLayer = baseArrayLayer;
    region.imageSubresource.layerCount = layerCount;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    if (ownsCmd) endSingleTimeCommands(cmd);
}

void VulkanContext::copyImageToBuffer(VkImage image, VkBuffer buffer,
                                      uint32_t width, uint32_t height,
                                      VkCommandBuffer cmd) const
{
    bool ownsCmd = (cmd == VK_NULL_HANDLE);
    if (ownsCmd) cmd = beginSingleTimeCommands();

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

    if (ownsCmd) endSingleTimeCommands(cmd);
}

void VulkanContext::transitionImageLayout(VkImage image, VkFormat format,
                                          VkImageLayout oldLayout, VkImageLayout newLayout,
                                          VkCommandBuffer cmd,
                                          uint32_t mipLevels, uint32_t baseMipLevel,
                                          uint32_t layerCount, uint32_t baseArrayLayer) const
{
    (void)format;
    bool ownsCmd = (cmd == VK_NULL_HANDLE);
    if (ownsCmd) cmd = beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    if (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM) {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    } else if (format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT) {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    } else {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }
    barrier.subresourceRange.baseMipLevel = baseMipLevel;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = baseArrayLayer;
    barrier.subresourceRange.layerCount = layerCount;

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? 0 : (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        sourceStage = (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
        destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(cmd, sourceStage, destinationStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    if (ownsCmd) endSingleTimeCommands(cmd);
}

void VulkanContext::waitIdle() const {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
}

} // namespace bro::render
