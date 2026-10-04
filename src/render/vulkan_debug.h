#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
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

/// Error-severity messages the debug messenger has reported in this process,
/// not counting the ones listed as known (below). bro-headless exits nonzero
/// when this is nonzero, so a validation error fails the test that caused it.
uint32_t vulkanValidationErrorCount();

/// Known errors are message ids (VUIDs) listed, one per line ('#' comments),
/// in the file named by BRO_VK_VALIDATION_KNOWN: logged as warnings and
/// counted here instead, so a tree can carry a burn-down list of errors owned
/// elsewhere without them failing every test.
uint32_t vulkanKnownValidationErrorCount();

/// Standard debug callback for Vulkan validation and performance warnings.
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData);

} // namespace bro::render
