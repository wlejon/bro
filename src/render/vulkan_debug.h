#pragma once

#include <vulkan/vulkan.h>
#include <vector>

namespace bro::render {

/// Checks if validation layers are supported on the host system.
bool checkValidationLayerSupport(const std::vector<const char*>& requestedLayers);

/// Returns the prioritized list of validation layers available on this system.
std::vector<const char*> getAvailableValidationLayers();

/// Populates a VkDebugUtilsMessengerCreateInfoEXT struct with standard severity, types, and callback.
void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo);

/// Creates the debug utils messenger for the given Vulkan instance.
VkResult createDebugUtilsMessenger(VkInstance instance,
                                   const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
                                   const VkAllocationCallbacks* pAllocator,
                                   VkDebugUtilsMessengerEXT* pDebugMessenger);

/// Destroys the debug utils messenger.
void destroyDebugUtilsMessenger(VkInstance instance,
                                VkDebugUtilsMessengerEXT debugMessenger,
                                const VkAllocationCallbacks* pAllocator);

/// Standard debug callback for Vulkan validation and performance warnings.
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData);

} // namespace bro::render
