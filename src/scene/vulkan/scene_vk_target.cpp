#include "scene/vulkan/scene_vk_target.h"
#include "util/log.h"

#include <cassert>

namespace bro::scene::vk {

SceneVkRenderTarget::~SceneVkRenderTarget() {
}

bool SceneVkRenderTarget::init(SceneVkAllocator& allocator, const SceneVkRenderTargetDesc& desc) {
    cleanup(allocator);

    if (desc.width == 0 || desc.height == 0) {
        LOG_ERROR("SceneVkRenderTarget: Invalid dimensions (%ux%u)", desc.width, desc.height);
        return false;
    }

    desc_ = desc;
    width_ = desc.width;
    height_ = desc.height;

    // 1. Create HDR color buffer if enabled
    if (desc_.hasColor) {
        VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (desc_.colorSampled) {
            colorUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        }

        bool ok = allocator.createImage(width_, height_, desc_.colorFormat,
                                       colorUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                       colorImage_, 1, VK_SAMPLE_COUNT_1_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
        if (!ok) {
            LOG_ERROR("SceneVkRenderTarget: Failed to create color buffer");
            cleanup(allocator);
            return false;
        }

        // Create linear clamp sampler for post-processing sampling
        if (desc_.colorSampled) {
            VkSamplerCreateInfo samplerInfo{};
            samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxAnisotropy = 1.0f;
            samplerInfo.compareEnable = VK_FALSE;
            samplerInfo.minLod = 0.0f;
            samplerInfo.maxLod = 1.0f;

            if (vkCreateSampler(allocator.device().device(), &samplerInfo, nullptr, &colorImage_.sampler) != VK_SUCCESS) {
                LOG_ERROR("SceneVkRenderTarget: Failed to create color sampler");
                cleanup(allocator);
                return false;
            }
        }
    }

    // 2. Create depth/stencil buffer if enabled
    if (desc_.hasDepth) {
        VkImageUsageFlags depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (desc_.depthSampled) {
            depthUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        }

        VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (desc_.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT || desc_.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT) {
            aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        bool ok = allocator.createImage(width_, height_, desc_.depthFormat,
                                       depthUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                       depthImage_, 1, VK_SAMPLE_COUNT_1_BIT,
                                       aspectMask);
        if (!ok) {
            LOG_ERROR("SceneVkRenderTarget: Failed to create depth buffer");
            cleanup(allocator);
            return false;
        }

        if (desc_.depthSampled) {
            VkSamplerCreateInfo depthSamplerInfo{};
            depthSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            depthSamplerInfo.magFilter = VK_FILTER_NEAREST;
            depthSamplerInfo.minFilter = VK_FILTER_NEAREST;
            depthSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            depthSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            depthSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            depthSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            depthSamplerInfo.maxAnisotropy = 1.0f;
            depthSamplerInfo.compareEnable = VK_FALSE;

            if (vkCreateSampler(allocator.device().device(), &depthSamplerInfo, nullptr, &depthImage_.sampler) != VK_SUCCESS) {
                LOG_ERROR("SceneVkRenderTarget: Failed to create depth sampler");
                cleanup(allocator);
                return false;
            }
        }
    }

    return true;
}

bool SceneVkRenderTarget::resize(SceneVkAllocator& allocator, uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) return true;
    SceneVkRenderTargetDesc newDesc = desc_;
    newDesc.width = width;
    newDesc.height = height;
    return init(allocator, newDesc);
}

void SceneVkRenderTarget::cleanup(SceneVkAllocator& allocator) {
    if (colorImage_.isValid()) {
        allocator.destroyImage(colorImage_);
    }
    if (depthImage_.isValid()) {
        allocator.destroyImage(depthImage_);
    }
    width_ = 0;
    height_ = 0;
}

void SceneVkRenderTarget::beginRendering(VkCommandBuffer cmd, SceneVkDevice& device,
                                         VkAttachmentLoadOp colorLoadOp,
                                         VkAttachmentStoreOp colorStoreOp,
                                         VkClearColorValue clearColor,
                                         VkAttachmentLoadOp depthLoadOp,
                                         VkAttachmentStoreOp depthStoreOp,
                                         float clearDepth) {
    assert(isValid());

    VkRenderingAttachmentInfoKHR colorAttachment{};
    if (desc_.hasColor) {
        if (colorImage_.currentLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            SceneVkAllocator allocator(device);
            allocator.transitionImageLayout(cmd, colorImage_.image, desc_.colorFormat,
                                           colorImage_.currentLayout,
                                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            colorImage_.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }

        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
        colorAttachment.imageView = colorImage_.view;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = colorLoadOp;
        colorAttachment.storeOp = colorStoreOp;
        colorAttachment.clearValue.color = clearColor;
    }

    VkRenderingAttachmentInfoKHR depthAttachment{};
    if (desc_.hasDepth) {
        if (depthImage_.currentLayout != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
            SceneVkAllocator allocator(device);
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (desc_.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT || desc_.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT) {
                aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
            allocator.transitionImageLayout(cmd, depthImage_.image, desc_.depthFormat,
                                           depthImage_.currentLayout,
                                           VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                           1, 0, aspect);
            depthImage_.currentLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        }

        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
        depthAttachment.imageView = depthImage_.view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = depthLoadOp;
        depthAttachment.storeOp = depthStoreOp;
        depthAttachment.clearValue.depthStencil.depth = clearDepth;
        depthAttachment.clearValue.depthStencil.stencil = 0;
    }

    VkRenderingInfoKHR renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
    renderingInfo.renderArea.offset = {0, 0};
    renderingInfo.renderArea.extent = {width_, height_};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = desc_.hasColor ? 1 : 0;
    renderingInfo.pColorAttachments = desc_.hasColor ? &colorAttachment : nullptr;
    renderingInfo.pDepthAttachment = desc_.hasDepth ? &depthAttachment : nullptr;

    // Set dynamic viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width_);
    viewport.height = static_cast<float>(height_);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {width_, height_};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    device.cmdBeginRendering(cmd, &renderingInfo);
}

void SceneVkRenderTarget::endRendering(VkCommandBuffer cmd, SceneVkDevice& device) {
    device.cmdEndRendering(cmd);
}

void SceneVkRenderTarget::transitionColorToShaderRead(VkCommandBuffer cmd, SceneVkAllocator& allocator) {
    if (desc_.hasColor && colorImage_.currentLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        allocator.transitionImageLayout(cmd, colorImage_.image, desc_.colorFormat,
                                       colorImage_.currentLayout,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        colorImage_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}

void SceneVkRenderTarget::transitionDepthToShaderRead(VkCommandBuffer cmd, SceneVkAllocator& allocator) {
    if (desc_.hasDepth && depthImage_.currentLayout != VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL) {
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (desc_.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT || desc_.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT) {
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        allocator.transitionImageLayout(cmd, depthImage_.image, desc_.depthFormat,
                                       depthImage_.currentLayout,
                                       VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                                       1, 0, aspect);
        depthImage_.currentLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    }
}

SceneVkShadowCascadeTarget::~SceneVkShadowCascadeTarget() {
}

bool SceneVkShadowCascadeTarget::init(SceneVkAllocator& allocator, uint32_t resolution,
                                     uint32_t cascadeCount, VkFormat depthFormat) {
    cleanup(allocator);

    resolution_ = resolution;
    cascadeCount_ = cascadeCount;
    format_ = depthFormat;

    VkDevice dev = allocator.device().device();

    // 1. Create 2D Array Image and View for Cascades via allocator
    bool ok = allocator.createImage(resolution_, resolution_, format_,
                                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                   shadowImage_, 1, VK_SAMPLE_COUNT_1_BIT,
                                   VK_IMAGE_ASPECT_DEPTH_BIT, cascadeCount_);
    if (!ok) {
        LOG_ERROR("SceneVkShadowCascadeTarget: Failed to create shadow cascade array image");
        return false;
    }

    // 3. Per-cascade layer views for rendering passes
    cascadeViews_.resize(cascadeCount_);
    for (uint32_t i = 0; i < cascadeCount_; ++i) {
        VkImageViewCreateInfo layerViewInfo{};
        layerViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        layerViewInfo.image = shadowImage_.image;
        layerViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        layerViewInfo.format = format_;
        layerViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        layerViewInfo.subresourceRange.baseMipLevel = 0;
        layerViewInfo.subresourceRange.levelCount = 1;
        layerViewInfo.subresourceRange.baseArrayLayer = i;
        layerViewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(dev, &layerViewInfo, nullptr, &cascadeViews_[i]) != VK_SUCCESS) {
            LOG_ERROR("SceneVkShadowCascadeTarget: Failed to create cascade %u layer view", i);
            cleanup(allocator);
            return false;
        }
    }

    // 4. Comparison shadow sampler with hardware PCF
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 1.0f;

    if (vkCreateSampler(dev, &samplerInfo, nullptr, &shadowSampler_) != VK_SUCCESS) {
        LOG_ERROR("SceneVkShadowCascadeTarget: Failed to create shadow comparison sampler");
        cleanup(allocator);
        return false;
    }

    shadowImage_.format = format_;
    shadowImage_.width = resolution_;
    shadowImage_.height = resolution_;
    currentLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    return true;
}

void SceneVkShadowCascadeTarget::cleanup(SceneVkAllocator& allocator) {
    VkDevice dev = allocator.device().device();

    if (shadowSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, shadowSampler_, nullptr);
        shadowSampler_ = VK_NULL_HANDLE;
    }

    for (VkImageView view : cascadeViews_) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(dev, view, nullptr);
        }
    }
    cascadeViews_.clear();

    if (shadowImage_.isValid()) {
        allocator.destroyImage(shadowImage_);
    }

    resolution_ = 0;
    cascadeCount_ = 0;
    currentLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

void SceneVkShadowCascadeTarget::beginCascadeRendering(VkCommandBuffer cmd, SceneVkDevice& device,
                                                      uint32_t cascadeIndex, float clearDepth) {
    assert(cascadeIndex < cascadeViews_.size());

    if (currentLayout_ != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = currentLayout_;
        barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = shadowImage_.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = cascadeCount_;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        currentLayout_ = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }

    VkRenderingAttachmentInfoKHR depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    depthAttachment.imageView = cascadeViews_[cascadeIndex];
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil.depth = clearDepth;
    depthAttachment.clearValue.depthStencil.stencil = 0;

    VkRenderingInfoKHR renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
    renderingInfo.renderArea.offset = {0, 0};
    renderingInfo.renderArea.extent = {resolution_, resolution_};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 0;
    renderingInfo.pColorAttachments = nullptr;
    renderingInfo.pDepthAttachment = &depthAttachment;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(resolution_);
    viewport.height = static_cast<float>(resolution_);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {resolution_, resolution_};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    device.cmdBeginRendering(cmd, &renderingInfo);
}

void SceneVkShadowCascadeTarget::endCascadeRendering(VkCommandBuffer cmd, SceneVkDevice& device) {
    device.cmdEndRendering(cmd);
}

void SceneVkShadowCascadeTarget::transitionToShaderRead(VkCommandBuffer cmd, SceneVkAllocator& allocator) {
    (void)allocator;
    if (currentLayout_ != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = currentLayout_;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = shadowImage_.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = cascadeCount_;
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        currentLayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}

} // namespace bro::scene::vk
