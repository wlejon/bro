#pragma once

// The full-screen-triangle passes' shared plumbing (SSAO, depth of field,
// bloom, tonemap, FXAA, colour grading): a pipeline layout of one set and an
// optional fragment push block, a pipeline drawing postfx.vert's triangle
// into one colour attachment, and the draw itself into a whole image.

#include "scene/vulkan/scene_vk_allocator.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::scene::vk {

class SceneVkDevice;

namespace fullscreen {

/// One descriptor set, `pushBytes` of fragment push constants (0 for none).
VkPipelineLayout layout(VkDevice dev, VkDescriptorSetLayout set, uint32_t pushBytes);

/// A layout of `samplers` combined-image-sampler bindings (0..n-1, fragment).
VkDescriptorSetLayout samplerSetLayout(VkDevice dev, uint32_t samplers);

/// postfx.vert + `fs` into one `format` attachment, no blending, no depth.
VkPipeline pipeline(VkDevice dev, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs, VkFormat format);

/// Draw the triangle over all of `target` (which must be in
/// COLOR_ATTACHMENT_OPTIMAL; the caller transitions it).
void draw(SceneVkDevice& device, VkCommandBuffer cmd, const SceneVkImage& target, VkPipeline pipeline,
          VkPipelineLayout layout, VkDescriptorSet set, const void* push = nullptr, uint32_t pushBytes = 0);

}  // namespace fullscreen
}  // namespace bro::scene::vk
