#pragma once

#include "scene/vulkan/scene_vk_allocator.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace bro::scene::vk {

/// Configuration for creating an offscreen HDR/Depth render target.
struct SceneVkRenderTargetDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    bool hasColor = true;
    bool hasDepth = true;
    bool colorSampled = true;
    bool depthSampled = true;
};

/// Offscreen render target abstraction encapsulating HDR color, depth/stencil buffers,
/// and Vulkan 1.3 Dynamic Rendering pass execution.
class SceneVkRenderTarget {
public:
    SceneVkRenderTarget() = default;
    ~SceneVkRenderTarget();

    SceneVkRenderTarget(const SceneVkRenderTarget&) = delete;
    SceneVkRenderTarget& operator=(const SceneVkRenderTarget&) = delete;

    /// Allocate color and depth attachments according to desc.
    bool init(SceneVkAllocator& allocator, const SceneVkRenderTargetDesc& desc);

    /// Resize color and depth buffers to new dimensions.
    bool resize(SceneVkAllocator& allocator, uint32_t width, uint32_t height);

    /// Free all image resources.
    void cleanup(SceneVkAllocator& allocator);

    /// Begin dynamic rendering into this target with specified clear/load/store actions.
    void beginRendering(VkCommandBuffer cmd, SceneVkDevice& device,
                        VkAttachmentLoadOp colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                        VkAttachmentStoreOp colorStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
                        VkClearColorValue clearColor = {{0.0f, 0.0f, 0.0f, 0.0f}},
                        VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                        VkAttachmentStoreOp depthStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
                        float clearDepth = 0.0f);

    /// End dynamic rendering.
    void endRendering(VkCommandBuffer cmd, SceneVkDevice& device);

    /// Transition color buffer to SHADER_READ_ONLY_OPTIMAL for post-processing sampling.
    void transitionColorToShaderRead(VkCommandBuffer cmd, SceneVkAllocator& allocator);

    /// Transition depth buffer to DEPTH_READ_ONLY_OPTIMAL / SHADER_READ_ONLY_OPTIMAL for SSAO/SSR/shadow sampling.
    void transitionDepthToShaderRead(VkCommandBuffer cmd, SceneVkAllocator& allocator);

    // Accessors
    const SceneVkImage& colorImage() const { return colorImage_; }
    const SceneVkImage& depthImage() const { return depthImage_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    VkFormat colorFormat() const { return desc_.colorFormat; }
    VkFormat depthFormat() const { return desc_.depthFormat; }
    bool isValid() const { return (!desc_.hasColor || colorImage_.isValid()) && (!desc_.hasDepth || depthImage_.isValid()); }

private:
    SceneVkRenderTargetDesc desc_{};
    SceneVkImage colorImage_;
    SceneVkImage depthImage_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

/// Multi-cascade directional shadow map target using a layered 2D depth array image.
class SceneVkShadowCascadeTarget {
public:
    SceneVkShadowCascadeTarget() = default;
    ~SceneVkShadowCascadeTarget();

    SceneVkShadowCascadeTarget(const SceneVkShadowCascadeTarget&) = delete;
    SceneVkShadowCascadeTarget& operator=(const SceneVkShadowCascadeTarget&) = delete;

    /// Allocate layered shadow depth map with cascadeCount layers.
    bool init(SceneVkAllocator& allocator, uint32_t resolution = 2048,
              uint32_t cascadeCount = 4, VkFormat depthFormat = VK_FORMAT_D32_SFLOAT);

    /// Clean up shadow map resources.
    void cleanup(SceneVkAllocator& allocator);

    /// Begin dynamic rendering into a specific cascade layer.
    void beginCascadeRendering(VkCommandBuffer cmd, SceneVkDevice& device,
                               uint32_t cascadeIndex, float clearDepth = 1.0f);

    /// End cascade dynamic rendering.
    void endCascadeRendering(VkCommandBuffer cmd, SceneVkDevice& device);

    /// Transition shadow array image to SHADER_READ_ONLY_OPTIMAL for scene lighting passes.
    void transitionToShaderRead(VkCommandBuffer cmd, SceneVkAllocator& allocator);

    // Accessors
    VkImageView cascadeView(uint32_t cascadeIndex) const {
        return (cascadeIndex < cascadeViews_.size()) ? cascadeViews_[cascadeIndex] : VK_NULL_HANDLE;
    }
    VkImageView arrayView() const { return shadowImage_.view; }
    VkSampler shadowSampler() const { return shadowSampler_; }
    uint32_t resolution() const { return resolution_; }
    uint32_t cascadeCount() const { return cascadeCount_; }
    VkFormat format() const { return format_; }
    bool isValid() const { return shadowImage_.isValid(); }

private:
    SceneVkImage shadowImage_;
    std::vector<VkImageView> cascadeViews_;
    VkSampler shadowSampler_ = VK_NULL_HANDLE;
    uint32_t resolution_ = 0;
    uint32_t cascadeCount_ = 0;
    VkFormat format_ = VK_FORMAT_D32_SFLOAT;
    VkImageLayout currentLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
};

} // namespace bro::scene::vk
