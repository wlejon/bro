#pragma once

// A GPU image one producer hands another: the 3D scene's finished frame to the
// compositor, or to a mesh in another scene that samples it as a texture.
//
// The image belongs to its producer and stays valid until that producer's next
// render (a resize or render-scale change recreates it), so consumers resolve
// it again every frame rather than keeping it. `layout` is the layout the
// producer leaves it in once its submitted work completes; a consumer that
// transitions it must put it back.

#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::render {

struct LayerImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;   // linear, clamp to edge
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;

    explicit operator bool() const {
        return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE && width > 0 && height > 0;
    }
};

}  // namespace bro::render
