#pragma once

// The frame's shared images. Every pass reaches them through SceneGpu, and
// the frame graph tracks their layouts (SceneVkImage::currentLayout) as the
// passes declare how they use them.
//
//   hdr / hdrMsaa            the HDR scene colour (RGBA16F). With MSAA the
//                            passes draw into hdrMsaa and every scope resolves
//                            into hdr, which is what later passes sample.
//   indirect / indirectMsaa  the opaque surfaces' indirect light, a second
//                            colour attachment of the opaque scope while SSAO
//                            is on (allocated on first use).
//   depth / depthMsaa        scene depth; the MSAA depth resolves to the
//                            nearest sample (depth::resolveNearest).
//   depthSnapshot            sampleable copy of the opaque depth (decals,
//                            soft particles, SSAO, SSR, depth of field).
//   ssrSnapshot              the lit opaque colour SSR reflects.
//   dofHdr, postLdr, ldr     the post chain; ldr is the frame's output.
//   shadowAtlas              every light's shadow tiles (SceneRenderer's
//                            ShadowPlan), sampled with the compare sampler of
//                            the lighting layout. 1x1 until a light casts a
//                            shadow, then the plan's size; it never shrinks,
//                            so cached tiles survive frames without shadows.

#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::scene::vk {

class SceneTargets {
public:
    static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr VkFormat kLdrFormat = VK_FORMAT_R8G8B8A8_UNORM;

    bool setup(SceneVkDevice& device, SceneVkAllocator& allocator);
    void cleanup(SceneVkAllocator& allocator);

    /// The highest sample count at or below `requested` that the device
    /// renders colour and depth at (Apple GPUs stop at 4).
    VkSampleCountFlagBits supportedSamples(int requested) const;

    /// (Re)allocate the frame-sized images for `width` x `height` at
    /// `samples`. Returns true when anything was recreated (the passes then
    /// get resize()); false with valid() false on failure.
    bool ensure(SceneVkAllocator& allocator, uint32_t width, uint32_t height, VkSampleCountFlagBits samples);
    /// Allocate the indirect-light attachments if they are not yet.
    bool ensureIndirect(SceneVkAllocator& allocator);
    /// Make the shadow atlas `size` texels square (clamped to the device),
    /// recreating it when the size differs. False on allocation failure.
    bool ensureShadowAtlas(SceneVkAllocator& allocator, uint32_t size);
    /// The largest shadow atlas the device can allocate.
    uint32_t maxShadowAtlas() const { return maxImageDimension_; }

    bool valid() const { return hdr.isValid() && depth.isValid() && ldr.isValid(); }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    VkSampleCountFlagBits samples() const { return samples_; }
    bool msaa() const { return samples_ != VK_SAMPLE_COUNT_1_BIT; }
    VkResolveModeFlagBits depthResolve() const { return depthResolve_; }

    /// The HDR scope's attachment formats, with or without the indirect one.
    TargetFormat hdrTarget(bool indirect) const;

    // Drawn into by the HDR scope (the MSAA images when msaa()).
    SceneVkImage& hdrDrawn() { return msaa() ? hdrMsaa : hdr; }
    SceneVkImage& indirectDrawn() { return msaa() ? indirectMsaa : indirect; }
    SceneVkImage& depthDrawn() { return msaa() ? depthMsaa : depth; }

    SceneVkImage hdr, hdrMsaa;
    SceneVkImage indirect, indirectMsaa;
    SceneVkImage depth, depthMsaa;
    SceneVkImage depthSnapshot;
    SceneVkImage ssrSnapshot;
    SceneVkImage dofHdr;
    SceneVkImage postLdr;
    SceneVkImage ldr;

    SceneVkImage shadowAtlas;

private:
    void destroyFrameImages(SceneVkAllocator& allocator);
    bool createSampled(SceneVkAllocator& allocator, SceneVkImage& out, VkFormat format,
                       VkImageUsageFlags usage, bool nearest);
    bool createAttachment(SceneVkAllocator& allocator, SceneVkImage& out, VkFormat format,
                          VkImageUsageFlags usage, VkSampleCountFlagBits samples);

    VkDevice device_ = VK_NULL_HANDLE;
    SceneVkDevice* sceneDevice_ = nullptr;
    VkSampleCountFlags supportedSamples_ = VK_SAMPLE_COUNT_1_BIT;
    VkResolveModeFlags supportedDepthResolve_ = 0;
    VkResolveModeFlagBits depthResolve_ = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    uint32_t maxImageDimension_ = 4096;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
};

}  // namespace bro::scene::vk
