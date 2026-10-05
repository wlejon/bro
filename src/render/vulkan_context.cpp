#include "render/vulkan_context.h"
#include "render/vulkan_debug.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan_beta.h>  // VkPhysicalDevicePortabilitySubsetFeaturesKHR

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/stat.h>
#endif

namespace bro::render {

namespace {

#ifdef __APPLE__
// A packaged bro carries its own Vulkan loader and MoltenVK
// (scripts/package-release.sh): vulkan/icd.d/MoltenVK_icd.json beside the
// executable. Unless the environment already names the drivers, add that one
// to the loader's search — added rather than substituted, so a machine with the
// Vulkan SDK or Homebrew's MoltenVK keeps its own as well. Set before the first
// instance-level call, which is when the loader scans for drivers.
void addBundledMoltenVK() {
    if (std::getenv("VK_DRIVER_FILES") || std::getenv("VK_ICD_FILENAMES") ||
        std::getenv("VK_ADD_DRIVER_FILES"))
        return;
    char exe[4096];
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0) return;
    std::string dir(exe);
    dir.erase(dir.find_last_of('/') + 1);
    const std::string icd = dir + "vulkan/icd.d/MoltenVK_icd.json";
    struct stat st{};
    if (stat(icd.c_str(), &st) == 0) setenv("VK_ADD_DRIVER_FILES", icd.c_str(), 0);
}
#endif

const std::vector<const char*> kRequiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

bool envFlag(const char* a, const char* b) {
    const char* v = std::getenv(a);
    if (!v) v = std::getenv(b);
    return v && (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "TRUE") == 0);
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice dev) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, exts.data());
    return exts;
}

bool hasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts)
        if (strcmp(e.extensionName, name) == 0) return true;
    return false;
}

struct DeviceFeatureSupport {
    bool dynamicRendering = false;
    bool synchronization2 = false;
    bool timelineSemaphore = false;
};

DeviceFeatureSupport queryFeatureSupport(VkPhysicalDevice dev) {
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(dev, &f2);
    return {f13.dynamicRendering == VK_TRUE, f13.synchronization2 == VK_TRUE,
            f12.timelineSemaphore == VK_TRUE};
}

} // namespace

VulkanContext::VulkanContext(const VulkanContextConfig& config)
    : config_(config)
{
    // Environment overrides: BRO_VK_VALIDATION (or BRO_VULKAN_VALIDATION) and
    // BRO_VK_DEVICE (or BRO_VULKAN_DEVICE).
    if (std::getenv("BRO_VK_VALIDATION") || std::getenv("BRO_VULKAN_VALIDATION"))
        config_.enableValidation = envFlag("BRO_VK_VALIDATION", "BRO_VULKAN_VALIDATION");
    if (const char* dev = std::getenv("BRO_VK_DEVICE")) {
        config_.preferredDeviceIndex = std::atoi(dev);
    } else if (const char* dev2 = std::getenv("BRO_VULKAN_DEVICE")) {
        config_.preferredDeviceIndex = std::atoi(dev2);
    }
}

VulkanContext::~VulkanContext() {
    cleanup();
}

bool VulkanContext::init(SDL_Window* presentTarget) {
    if (!createInstance()) {
        LOG_ERROR("Vulkan: Failed to create VkInstance");
        return false;
    }

    if (config_.enableValidation) {
        setupDebugMessenger();
    }

    // Choose the device against a surface on the window it will present to,
    // so the present queue family is one that can. The probe surface goes
    // away again; the swapchain creates its own.
    VkSurfaceKHR probe = VK_NULL_HANDLE;
    if (presentTarget && !config_.headless &&
        !SDL_Vulkan_CreateSurface(presentTarget, instance_, nullptr, &probe)) {
        LOG_ERROR("Vulkan: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        cleanup();
        return false;
    }
    const bool selected = selectPhysicalDevice(probe);
    if (probe != VK_NULL_HANDLE) SDL_Vulkan_DestroySurface(instance_, probe, nullptr);
    if (!selected) {
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

    if (!queue_.init(device_, graphicsQueue_, presentQueue_,
                     static_cast<uint32_t>(queueIndices_.graphicsFamily)) ||
        !frames_.init(*this)) {
        LOG_ERROR("Vulkan: Failed to create the queue owner / frame ring");
        cleanup();
        return false;
    }
    pipelineCache_.init(device_, deviceProperties_);

    LOG_INFO("Vulkan: Initialized successfully on device: %s (Driver %u.%u.%u), API %u.%u%s",
             deviceProperties_.deviceName,
             VK_VERSION_MAJOR(deviceProperties_.driverVersion),
             VK_VERSION_MINOR(deviceProperties_.driverVersion),
             VK_VERSION_PATCH(deviceProperties_.driverVersion),
             VK_API_VERSION_MAJOR(apiVersion_), VK_API_VERSION_MINOR(apiVersion_),
             synchronization2_ ? ", synchronization2" : "");
    return true;
}

void VulkanContext::cleanup() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        pipelineCache_.shutdown();
        frames_.shutdown();
        queue_.shutdown();
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
#ifdef __APPLE__
    addBundledMoltenVK();
#endif
    std::vector<const char*> validationLayers;
    if (config_.enableValidation) {
        validationLayers = getAvailableValidationLayers();
        if (validationLayers.empty() || !checkValidationLayerSupport(validationLayers)) {
            LOG_WARN("Vulkan: Validation layers requested, but not available on system");
            config_.enableValidation = false;
            validationLayers.clear();
        }
    }

    // bro is a Vulkan 1.3 application. A 1.0 loader has no
    // vkEnumerateInstanceVersion and cannot run it at all.
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    auto enumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerateInstanceVersion) enumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_3) {
        LOG_ERROR("Vulkan: the loader supports API %u.%u; bro needs Vulkan 1.3",
                  VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
        return false;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Bro";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "BroEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions;

    uint32_t availExtCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availExtCount, nullptr);
    std::vector<VkExtensionProperties> availExts(availExtCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availExtCount, availExts.data());
    auto hasExt = [&](const char* name) { return hasExtension(availExts, name); };

    // Windowed: the platform surface extensions SDL needs.
    if (!config_.headless) {
        uint32_t sdlExtCount = 0;
        char const* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
        if (!sdlExtensions) {
            LOG_ERROR("Vulkan: SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
            return false;
        }
        for (uint32_t i = 0; i < sdlExtCount; ++i) extensions.push_back(sdlExtensions[i]);
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
        } else {
            LOG_WARN("Vulkan: requested instance extension %s is not available", ext.c_str());
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
    if (res != VK_SUCCESS) LOG_ERROR("Vulkan: vkCreateInstance failed (%d)", res);
    return res == VK_SUCCESS;
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

    auto canPresent = [&](uint32_t i) {
        VkBool32 support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &support);
        return support == VK_TRUE;
    };

    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        const auto& qf = queueFamilies[i];
        if ((qf.queueFlags & VK_QUEUE_GRAPHICS_BIT) && indices.graphicsFamily < 0) {
            indices.graphicsFamily = static_cast<int>(i);
        }
        if ((qf.queueFlags & VK_QUEUE_COMPUTE_BIT) && indices.computeFamily < 0) {
            indices.computeFamily = static_cast<int>(i);
        }
        if (qf.queueFlags & VK_QUEUE_TRANSFER_BIT) {
            if (indices.transferFamily < 0 || !(qf.queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                indices.transferFamily = static_cast<int>(i);
            }
        }
        if (surface != VK_NULL_HANDLE && indices.presentFamily < 0 && canPresent(i)) {
            indices.presentFamily = static_cast<int>(i);
        }
    }

    // Prefer presenting from the graphics family: one queue then does both.
    if (surface != VK_NULL_HANDLE && indices.graphicsFamily >= 0 &&
        canPresent(static_cast<uint32_t>(indices.graphicsFamily))) {
        indices.presentFamily = indices.graphicsFamily;
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

bool VulkanContext::canPresentTo(VkSurfaceKHR surface) const {
    if (surface == VK_NULL_HANDLE || queueIndices_.presentFamily < 0) return false;
    VkBool32 support = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_,
                                         static_cast<uint32_t>(queueIndices_.presentFamily),
                                         surface, &support);
    return support == VK_TRUE;
}

bool VulkanContext::deviceMeetsRequirements(VkPhysicalDevice dev,
                                            const VkPhysicalDeviceProperties& props) const {
    if (props.apiVersion < VK_API_VERSION_1_3) {
        LOG_INFO("Vulkan: skipping %s (API %u.%u; bro needs 1.3)", props.deviceName,
                 VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
        return false;
    }
    const DeviceFeatureSupport f = queryFeatureSupport(dev);
    if (!f.dynamicRendering || !f.timelineSemaphore) {
        LOG_INFO("Vulkan: skipping %s (dynamicRendering=%d timelineSemaphore=%d)", props.deviceName,
                 f.dynamicRendering, f.timelineSemaphore);
        return false;
    }
    if (!config_.headless) {
        const auto exts = deviceExtensions(dev);
        for (const char* req : kRequiredDeviceExtensions) {
            if (!hasExtension(exts, req)) {
                LOG_INFO("Vulkan: skipping %s (no %s)", props.deviceName, req);
                return false;
            }
        }
    }
    return true;
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
        vkGetPhysicalDeviceProperties(dev, &props);

        if (config_.preferredDeviceIndex >= 0 && static_cast<int>(i) != config_.preferredDeviceIndex)
            continue;

        VulkanQueueFamilyIndices indices = findQueueFamilies(dev, compatibleSurface);
        if (!indices.isComplete(compatibleSurface != VK_NULL_HANDLE)) {
            LOG_INFO("Vulkan: skipping %s (no graphics%s queue family)", props.deviceName,
                     compatibleSurface != VK_NULL_HANDLE ? " or present" : "");
            continue;
        }
        if (!deviceMeetsRequirements(dev, props)) continue;

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
        if (config_.preferredDeviceIndex >= 0)
            LOG_ERROR("Vulkan: device %d (BRO_VK_DEVICE) is missing or unsuitable",
                      config_.preferredDeviceIndex);
        else
            LOG_ERROR("Vulkan: Failed to find a suitable physical device");
        return false;
    }

    physicalDevice_ = chosenDevice;
    queueIndices_ = chosenIndices;
    vkGetPhysicalDeviceProperties(physicalDevice_, &deviceProperties_);
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);
    vkGetPhysicalDeviceFeatures(physicalDevice_, &deviceFeatures_);
    apiVersion_ = std::min<uint32_t>(VK_API_VERSION_1_3, deviceProperties_.apiVersion);  // what the instance asked for
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

    const auto availExts = deviceExtensions(physicalDevice_);
    std::vector<const char*> enabledExtensions;
    if (!config_.headless) {
        for (const char* ext : kRequiredDeviceExtensions) enabledExtensions.push_back(ext);
    }
    // MoltenVK: a portability-subset device must enable the extension.
    if (hasExtension(availExts, "VK_KHR_portability_subset")) {
        enabledExtensions.push_back("VK_KHR_portability_subset");
    }
    for (const auto& ext : config_.extraDeviceExtensions) {
        if (hasExtension(availExts, ext.c_str())) {
            enabledExtensions.push_back(ext.c_str());
        } else {
            LOG_WARN("Vulkan: requested device extension %s is not available", ext.c_str());
        }
    }

    // Selection guaranteed dynamicRendering and timelineSemaphore;
    // synchronization2 is enabled only where the device has it.
    const DeviceFeatureSupport support = queryFeatureSupport(physicalDevice_);
    synchronization2_ = support.synchronization2;

    VkPhysicalDeviceVulkan13Features vulkan13Features{};
    vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13Features.dynamicRendering = VK_TRUE;
    vulkan13Features.synchronization2 = synchronization2_ ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceVulkan12Features vulkan12Features{};
    vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12Features.timelineSemaphore = VK_TRUE;
    vulkan12Features.pNext = &vulkan13Features;

    // Optional core features, each enabled where the device has it; the
    // accessors report what was enabled. WebGL is their main consumer:
    // vertex-stage stores carry transform feedback, the compression features
    // decide which compressed-texture extensions it offers.
    const VkPhysicalDeviceFeatures available = deviceFeatures_;
    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = available.samplerAnisotropy;
    deviceFeatures.wideLines = available.wideLines;
    deviceFeatures.largePoints = available.largePoints;
    deviceFeatures.vertexPipelineStoresAndAtomics = available.vertexPipelineStoresAndAtomics;
    deviceFeatures.textureCompressionBC = available.textureCompressionBC;
    deviceFeatures.textureCompressionETC2 = available.textureCompressionETC2;
    deviceFeatures.textureCompressionASTC_LDR = available.textureCompressionASTC_LDR;
    deviceFeatures.occlusionQueryPrecise = available.occlusionQueryPrecise;
    deviceFeatures.fullDrawIndexUint32 = available.fullDrawIndexUint32;
    deviceFeatures.depthBiasClamp = available.depthBiasClamp;
    deviceFeatures.independentBlend = available.independentBlend;
    deviceFeatures.shaderClipDistance = available.shaderClipDistance;
    deviceFeatures_ = deviceFeatures;

    // Instanced-attribute divisors other than 0 and 1, and primitive restart
    // for list topologies (WebGL 2 restarts every indexed draw).
    void* chain = &vulkan12Features;
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT divisorFeatures{};
    divisorFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT;
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT restartFeatures{};
    restartFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT;
    // MoltenVK: the portability subset's features must be enabled to be
    // relied on (mutable comparison samplers, triangle fans, ...).
    VkPhysicalDevicePortabilitySubsetFeaturesKHR portabilityFeatures{};
    // The enum is behind VK_ENABLE_BETA_EXTENSIONS in vulkan_core.h.
    portabilityFeatures.sType = static_cast<VkStructureType>(1000163000);
    auto probe = [&](auto& features) {
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &features;
        vkGetPhysicalDeviceFeatures2(physicalDevice_, &f2);
        features.pNext = chain;
        chain = &features;
    };
    if (hasExtension(availExts, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)) {
        probe(divisorFeatures);
        if (divisorFeatures.vertexAttributeInstanceRateDivisor) {
            enabledExtensions.push_back(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
            vertexAttributeDivisor_ = true;
        }
        divisorFeatures.vertexAttributeInstanceRateZeroDivisor = VK_FALSE;
        if (!vertexAttributeDivisor_) chain = divisorFeatures.pNext;
    }
    if (hasExtension(availExts, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME)) {
        probe(restartFeatures);
        if (restartFeatures.primitiveTopologyListRestart) {
            enabledExtensions.push_back(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
            listRestart_ = true;
        }
        restartFeatures.primitiveTopologyPatchListRestart = VK_FALSE;
        if (!listRestart_) chain = restartFeatures.pNext;
    }
    if (hasExtension(availExts, "VK_KHR_portability_subset")) {
        probe(portabilityFeatures);
        imageView2DOn3D_ = portabilityFeatures.imageView2DOn3DImage == VK_TRUE;
        mutableComparisonSamplers_ = portabilityFeatures.mutableComparisonSamplers == VK_TRUE;
    }

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = chain;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();

    VkResult res = vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_);
    if (res != VK_SUCCESS) {
        LOG_ERROR("Vulkan: vkCreateDevice failed (%d)", res);
        return false;
    }

    vkGetDeviceQueue(device_, queueIndices_.graphicsFamily, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, queueIndices_.presentFamily, 0, &presentQueue_);
    vkGetDeviceQueue(device_, queueIndices_.computeFamily, 0, &computeQueue_);
    vkGetDeviceQueue(device_, queueIndices_.transferFamily, 0, &transferQueue_);
    return true;
}

bool VulkanContext::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueIndices_.graphicsFamily;

    return vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) == VK_SUCCESS;
}

} // namespace bro::render
