#pragma once

// The frame's shared images. Every pass reaches them through SceneGpu, and
// the frame graph tracks their layouts (SceneVkImage::currentLayout) as the
// passes declare how they use them.
//
//   hdr / hdrMsaa            the HDR scene colour (RGBA16F). With MSAA the
//                            passes draw into hdrMsaa and every scope resolves
//                            into hdr, which is what later passes sample.
//   mark / markMsaa          the opaque surfaces' sub-pixel mark (R8): how far
//                            each pixel's surface is a cut card's detail too
//                            fine to show, which PassSubpixel blends with its
//                            neighbours. The opaque scope's second attachment.
//   indirect / indirectMsaa  the opaque surfaces' indirect light, the opaque
//                            scope's third attachment while SSAO is on
//                            (allocated on first use).
//   depth / depthMsaa        scene depth; the MSAA depth resolves to the
//                            nearest sample (depth::resolveNearest). The
//                            shadow pages' marking pass samples depth.
//   depthSnapshot            sampleable copy of the opaque depth (decals,
//                            soft particles, SSAO, SSR, depth of field).
//   colorSnapshot            sampleable copy of the opaque colour (the
//                            sub-pixel resolve's source, then the lit opaque
//                            colour SSR reflects).
//   dofHdr, postLdr, ldr     the post chain; ldr is the frame's output.
//
// The shadow pages' pool and scratch target are not frame-sized: they are
// SceneShadowPages' (vulkan/shadow/scene_shadow_pages.h).

#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/frame/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::scene::vk {

class SceneTargets {
public:
    static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr VkFormat kLdrFormat = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat kMarkFormat = VK_FORMAT_R8_UNORM;
    // The opaque scope's attachments, at these output locations of the
    // shaders that draw in it (mesh.frag, plants.frag, cover_far.frag).
    static constexpr uint32_t kMarkSlot = 1;
    static constexpr uint32_t kIndirectSlot = 2;

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

    bool valid() const { return hdr.isValid() && depth.isValid() && ldr.isValid(); }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    VkSampleCountFlagBits samples() const { return samples_; }
    bool msaa() const { return samples_ != VK_SAMPLE_COUNT_1_BIT; }
    VkResolveModeFlagBits depthResolve() const { return depthResolve_; }

    /// The HDR scope's attachment formats: the colour, then (the opaque
    /// scope's) the mark and, with it, the indirect light.
    TargetFormat hdrTarget(bool mark, bool indirect) const;
    /// Whether a target carries the opaque scope's mark / indirect light.
    static bool carriesMark(const TargetFormat& t) {
        return t.colorCount > kMarkSlot && t.color[kMarkSlot] == kMarkFormat;
    }
    static bool carriesIndirect(const TargetFormat& t) {
        return carriesMark(t) && t.colorCount > kIndirectSlot && t.color[kIndirectSlot] == kHdrFormat;
    }

    // Drawn into by the HDR scope (the MSAA images when msaa()).
    SceneVkImage& hdrDrawn() { return msaa() ? hdrMsaa : hdr; }
    SceneVkImage& markDrawn() { return msaa() ? markMsaa : mark; }
    SceneVkImage& indirectDrawn() { return msaa() ? indirectMsaa : indirect; }
    SceneVkImage& depthDrawn() { return msaa() ? depthMsaa : depth; }

    SceneVkImage hdr, hdrMsaa;
    SceneVkImage mark, markMsaa;
    SceneVkImage indirect, indirectMsaa;
    SceneVkImage depth, depthMsaa;
    SceneVkImage depthSnapshot;
    SceneVkImage colorSnapshot;
    SceneVkImage dofHdr;
    SceneVkImage postLdr;
    SceneVkImage ldr;

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
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
};

}  // namespace bro::scene::vk
