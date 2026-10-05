#include "scene/vulkan/scene_targets.h"

#include "scene/vulkan/scene_vk_depth.h"
#include "util/log.h"

namespace bro::scene::vk {

bool SceneTargets::setup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    device_ = device.device();
    sceneDevice_ = &device;

    const VkPhysicalDeviceLimits& limits = device.context().deviceProperties().limits;
    supportedSamples_ = limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts;

    VkPhysicalDeviceDepthStencilResolveProperties resolve{};
    resolve.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &resolve;
    vkGetPhysicalDeviceProperties2(device.physicalDevice(), &props);
    supportedDepthResolve_ = resolve.supportedDepthResolveModes;
    depthResolve_ = depth::resolveNearest(supportedDepthResolve_);

    if (!allocator.createImage(kShadowResolution, kShadowResolution, kDepthFormat,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, shadow, 1, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_ASPECT_DEPTH_BIT, kShadowCascades)) {
        LOG_ERROR("SceneTargets: Failed creating the shadow map");
        return false;
    }
    for (uint32_t i = 0; i < kShadowCascades; ++i) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = shadow.image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = kDepthFormat;
        info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1};
        if (vkCreateImageView(device_, &info, nullptr, &shadowCascadeViews[i]) != VK_SUCCESS) {
            LOG_ERROR("SceneTargets: Failed creating shadow cascade view %u", i);
            return false;
        }
    }
    return true;
}

void SceneTargets::cleanup(SceneVkAllocator& allocator) {
    destroyFrameImages(allocator);
    allocator.destroyImage(indirect);
    allocator.destroyImage(indirectMsaa);
    if (sceneDevice_) {
        VkDevice dev = device_;
        sceneDevice_->defer([dev, views = shadowCascadeViews] {
            for (VkImageView v : views) {
                if (v != VK_NULL_HANDLE) vkDestroyImageView(dev, v, nullptr);
            }
        });
    }
    shadowCascadeViews = {};
    allocator.destroyImage(shadow);
}

VkSampleCountFlagBits SceneTargets::supportedSamples(int requested) const {
    for (VkSampleCountFlagBits c : {VK_SAMPLE_COUNT_8_BIT, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_2_BIT}) {
        if (requested >= static_cast<int>(c) && (supportedSamples_ & c)) return c;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

TargetFormat SceneTargets::hdrTarget(bool withIndirect) const {
    TargetFormat t;
    t.color[0] = kHdrFormat;
    t.colorCount = 1;
    if (withIndirect) {
        t.color[1] = kHdrFormat;
        t.colorCount = 2;
    }
    t.depth = kDepthFormat;
    t.samples = samples_;
    return t;
}

bool SceneTargets::createSampled(SceneVkAllocator& allocator, SceneVkImage& out, VkFormat format,
                                 VkImageUsageFlags usage, bool nearest) {
    const VkImageAspectFlags aspect = format == kDepthFormat ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (!allocator.createImage(width_, height_, format, usage | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out, 1, VK_SAMPLE_COUNT_1_BIT, aspect)) {
        return false;
    }
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = info.magFilter;
    info.mipmapMode = nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return vkCreateSampler(device_, &info, nullptr, &out.sampler) == VK_SUCCESS;
}

bool SceneTargets::createAttachment(SceneVkAllocator& allocator, SceneVkImage& out, VkFormat format,
                                    VkImageUsageFlags usage, VkSampleCountFlagBits samples) {
    const VkImageAspectFlags aspect = format == kDepthFormat ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    return allocator.createImage(width_, height_, format, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out, 1,
                                 samples, aspect);
}

void SceneTargets::destroyFrameImages(SceneVkAllocator& allocator) {
    for (SceneVkImage* img : {&hdr, &hdrMsaa, &depth, &depthMsaa, &depthSnapshot, &ssrSnapshot, &dofHdr,
                              &postLdr, &ldr}) {
        allocator.destroyImage(*img);
    }
}

bool SceneTargets::ensure(SceneVkAllocator& allocator, uint32_t width, uint32_t height,
                          VkSampleCountFlagBits samples) {
    if (valid() && width == width_ && height == height_ && samples == samples_) return false;

    const bool hadIndirect = indirect.isValid();
    destroyFrameImages(allocator);
    allocator.destroyImage(indirect);
    allocator.destroyImage(indirectMsaa);
    width_ = width;
    height_ = height;
    samples_ = samples;

    constexpr VkImageUsageFlags kColor = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    bool ok = createSampled(allocator, hdr, kHdrFormat, kColor | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false) &&
              createAttachment(allocator, depth, kDepthFormat,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_SAMPLE_COUNT_1_BIT) &&
              createSampled(allocator, depthSnapshot, kDepthFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT, true) &&
              createSampled(allocator, ssrSnapshot, kHdrFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT, false) &&
              createSampled(allocator, dofHdr, kHdrFormat, kColor, false) &&
              createSampled(allocator, postLdr, kLdrFormat, kColor, false) &&
              createSampled(allocator, ldr, kLdrFormat, kColor | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false);
    if (ok && msaa()) {
        ok = createAttachment(allocator, hdrMsaa, kHdrFormat, kColor, samples) &&
             createAttachment(allocator, depthMsaa, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                              samples);
    }
    if (ok && hadIndirect) ok = ensureIndirect(allocator);
    if (!ok) {
        LOG_ERROR("SceneTargets: Failed creating the frame targets (%ux%u, %d samples)", width, height,
                  static_cast<int>(samples));
        destroyFrameImages(allocator);
    }
    return true;
}

bool SceneTargets::ensureIndirect(SceneVkAllocator& allocator) {
    if (indirect.isValid()) return true;
    bool ok = createSampled(allocator, indirect, kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false);
    if (ok && msaa()) {
        ok = createAttachment(allocator, indirectMsaa, kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, samples_);
    }
    if (!ok) {
        LOG_ERROR("SceneTargets: Failed creating the indirect-light target");
        allocator.destroyImage(indirect);
        allocator.destroyImage(indirectMsaa);
    }
    return ok;
}

}  // namespace bro::scene::vk
