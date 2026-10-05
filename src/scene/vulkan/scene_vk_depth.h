#pragma once

// The GPU half of the scene's depth policy (scene/depth_policy.h): what every
// camera-depth pass and pipeline derives from reversedZ(). Shadow maps are
// outside it — they are always conventional (see makeOrthoZeroToOne).
//
// Shaders get the policy as specialization constant 0, which
// SceneVkPipelineBuilder sets on every stage of every scene pipeline:
//
//   layout(constant_id = 0) const bool REVERSED_Z = true;
//
// A shader that does not declare it is unaffected.

#include "scene/depth_policy.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk::depth {

/// Specialization constant id carrying reversedZ() into shaders.
constexpr uint32_t kReversedZConstantId = 0;

/// Clear value for "nothing drawn yet" (the far plane).
inline float clearFar() { return depthClearFar(); }

/// Depth test for ordinary "keep what is nearer" draws.
inline VkCompareOp compareCloser() {
    return reversedZ() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
}

/// Depth test for "keep what lies at or behind the scene surface" — volumes
/// that must only touch pixels whose surface is inside them.
inline VkCompareOp compareFartherEqual() {
    return reversedZ() ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_GREATER_OR_EQUAL;
}

/// MSAA depth resolve that keeps the NEAREST sample of each pixel (what a
/// single-sample render would have kept): MAX under reversed-Z, MIN otherwise,
/// or sample zero when the device supports neither. `supported` is
/// VkPhysicalDeviceDepthStencilResolveProperties::supportedDepthResolveModes.
inline VkResolveModeFlagBits resolveNearest(VkResolveModeFlags supported) {
    const VkResolveModeFlagBits want = reversedZ() ? VK_RESOLVE_MODE_MAX_BIT : VK_RESOLVE_MODE_MIN_BIT;
    return (supported & want) ? want : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
}

}  // namespace bro::scene::vk::depth
