#include "render/vulkan_debug.h"
#include "util/log.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>

namespace bro::render {

namespace {

const std::vector<const char*> kCandidateValidationLayers = {
    "VK_LAYER_KHRONOS_validation",
    "VK_LAYER_KHR_validation"
};

std::atomic<uint32_t> gErrorCount{0};
std::atomic<uint32_t> gKnownErrorCount{0};

// Message ids listed in $BRO_VK_VALIDATION_KNOWN, read once.
const std::set<std::string>& knownMessageIds() {
    static std::set<std::string> ids;
    static std::once_flag once;
    std::call_once(once, [] {
        const char* path = std::getenv("BRO_VK_VALIDATION_KNOWN");
        if (!path || !*path) return;
        std::ifstream in(path);
        if (!in) {
            LOG_WARN("BRO_VK_VALIDATION_KNOWN: cannot read %s", path);
            return;
        }
        std::string line;
        while (std::getline(in, line)) {
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            const size_t b = line.find_first_not_of(" \t\r");
            const size_t e = line.find_last_not_of(" \t\r");
            if (b != std::string::npos) ids.insert(line.substr(b, e - b + 1));
        }
    });
    return ids;
}

} // namespace

uint32_t vulkanValidationErrorCount() {
    return gErrorCount.load(std::memory_order_relaxed);
}

uint32_t vulkanKnownValidationErrorCount() {
    return gKnownErrorCount.load(std::memory_order_relaxed);
}

bool checkValidationLayerSupport(const std::vector<const char*>& requestedLayers) {
    if (requestedLayers.empty()) return false;

    uint32_t layerCount = 0;
    if (vkEnumerateInstanceLayerProperties(&layerCount, nullptr) != VK_SUCCESS || layerCount == 0) {
        return false;
    }
    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    for (const char* layerName : requestedLayers) {
        bool layerFound = false;
        for (const auto& layerProperties : availableLayers) {
            if (strcmp(layerName, layerProperties.layerName) == 0) {
                layerFound = true;
                break;
            }
        }
        if (!layerFound) return false;
    }
    return true;
}

std::vector<const char*> getAvailableValidationLayers() {
    uint32_t layerCount = 0;
    if (vkEnumerateInstanceLayerProperties(&layerCount, nullptr) != VK_SUCCESS || layerCount == 0) {
        return {};
    }
    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    std::vector<const char*> foundLayers;
    for (const char* candidate : kCandidateValidationLayers) {
        for (const auto& layerProperties : availableLayers) {
            if (strcmp(candidate, layerProperties.layerName) == 0) {
                foundLayers.push_back(candidate);
                return foundLayers; // Primary validation layer found
            }
        }
    }
    return foundLayers;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData)
{
    (void)userData;
    const char* typeStr = "General";
    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) {
        typeStr = "Validation";
    } else if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) {
        typeStr = "Performance";
    }

    const char* msgId = (callbackData && callbackData->pMessageIdName) ? callbackData->pMessageIdName : "VUID-General";
    const char* msg = (callbackData && callbackData->pMessage) ? callbackData->pMessage : "";

    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (knownMessageIds().count(msgId)) {
            gKnownErrorCount.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("[Vulkan %s KNOWN] [%s] %s", typeStr, msgId, msg);
            return VK_FALSE;
        }
        gErrorCount.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("[Vulkan %s ERROR] [%s] %s", typeStr, msgId, msg);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG_WARN("[Vulkan %s WARNING] [%s] %s", typeStr, msgId, msg);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        LOG_INFO("[Vulkan %s INFO] [%s] %s", typeStr, msgId, msg);
    }

    return VK_FALSE;
}

void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
    createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;

    VkDebugUtilsMessageSeverityFlagsEXT severity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

    if (const char* v = std::getenv("BRO_VK_VERBOSE_VALIDATION")) {
        if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0) {
            severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
        }
    }

    createInfo.messageSeverity = severity;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
}

VkResult createDebugUtilsMessenger(VkInstance instance,
                                   const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
                                   const VkAllocationCallbacks* pAllocator,
                                   VkDebugUtilsMessengerEXT* pDebugMessenger)
{
    auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void destroyDebugUtilsMessenger(VkInstance instance,
                                VkDebugUtilsMessengerEXT debugMessenger,
                                const VkAllocationCallbacks* pAllocator)
{
    if (instance == VK_NULL_HANDLE || debugMessenger == VK_NULL_HANDLE) return;

    auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}

} // namespace bro::render
