#pragma once

// The GPU half of the scene's depth policy (scene/depth_policy.h): camera
// depth is reversed (near 1, far 0), and this is what every camera-depth pass
// and pipeline derives from that. Shadow maps are outside it: they are
// always conventional (see makeOrthoZeroToOne).

#include "scene/depth_policy.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk::depth {

/// Clear value for "nothing drawn yet" (the far plane).
inline float clearFar() { return depthClearFar(); }

/// Depth test for ordinary "keep what is nearer" draws.
inline VkCompareOp compareCloser() { return VK_COMPARE_OP_GREATER_OR_EQUAL; }

/// Depth test for "keep what lies at or behind the scene surface" — volumes
/// that must only touch pixels whose surface is inside them.
inline VkCompareOp compareFartherEqual() { return VK_COMPARE_OP_LESS_OR_EQUAL; }

/// MSAA depth resolve that keeps the NEAREST sample of each pixel (what a
/// single-sample render would have kept): MAX, or sample zero when the
/// device does not support it. `supported` is
/// VkPhysicalDeviceDepthStencilResolveProperties::supportedDepthResolveModes.
inline VkResolveModeFlagBits resolveNearest(VkResolveModeFlags supported) {
    return (supported & VK_RESOLVE_MODE_MAX_BIT) ? VK_RESOLVE_MODE_MAX_BIT : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
}

}  // namespace bro::scene::vk::depth
