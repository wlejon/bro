#pragma once

// What changed between two composited frames, found from their pixels: the
// damage a remote-desktop encoder is told (bro.remote, Frame::damage in
// brovideo), so that a small change it can code finer lands sharp at once.
//
// The engine's own damage (render/damage.h) is the page's base layer only;
// a screen under DRM is mostly other things (client windows, terminals,
// canvases, video), and only the composited pixels say what all of them
// did. Both frames are compared in kTile x kTile tiles, and the tiles that
// differ become a few rects. On the GPU (GpuFrameDiff) the comparison is a
// compute pass recorded into the frame's own command buffer, reading the
// two images where they are; on the CPU (diffFrames) it is a memcmp per
// tile row, for the paths that already read the frame back.

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bro::render {

class VulkanContext;

struct DamageRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

/// The comparison's granularity, px.
inline constexpr uint32_t kDamageTile = 16;
/// More rects than this become their bounding box: damage that scattered is a
/// big change, and the encoder merges rects down to a handful anyway.
inline constexpr size_t kMaxDamageRects = 64;

/// The rects of a `tilesX` x `tilesY` grid of kDamageTile tiles over a
/// `width` x `height` frame, where `flags[ty * tilesX + tx]` is non-zero for
/// a tile that changed: each row's runs of changed tiles, a run continued
/// unchanged on the rows below it extended down. Clipped to the frame.
/// Empty when nothing changed.
void tileFlagsToRects(const uint32_t* flags, uint32_t tilesX, uint32_t tilesY, uint32_t width, uint32_t height,
                      std::vector<DamageRect>& out);

/// The damage from `older` to `newer`, two 4-byte-per-pixel frames of
/// `width` x `height` with rows `stride` bytes apart.
void diffFrames(const uint8_t* older, const uint8_t* newer, uint32_t width, uint32_t height, size_t stride,
                std::vector<DamageRect>& out);

/// The damage between two images on the GPU.
class GpuFrameDiff {
public:
    explicit GpuFrameDiff(VulkanContext& ctx);
    ~GpuFrameDiff();
    GpuFrameDiff(const GpuFrameDiff&) = delete;
    GpuFrameDiff& operator=(const GpuFrameDiff&) = delete;

    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

    /// Records into `cmd` (a command buffer of the context's current frame) a
    /// comparison of `newer` with `older`: two `width` x `height` colour
    /// images, both sampleable and in `layout`, last written as colour
    /// attachments or by transfers. Their layouts are left as they are.
    /// False when nothing was recorded.
    bool record(VkCommandBuffer cmd, VkImage newer, VkImageView newerView, VkImage older, VkImageView olderView,
                VkImageLayout layout, uint32_t width, uint32_t height);

    /// After the work of the command buffer last recorded into is done (and
    /// within the same frame): its damage. False when nothing was recorded.
    bool result(std::vector<DamageRect>& out);

private:
    VulkanContext& ctx_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    // The recorded comparison's tile flags (the frame's upload memory).
    const uint32_t* flags_ = nullptr;
    uint32_t tilesX_ = 0, tilesY_ = 0, width_ = 0, height_ = 0;
};

}  // namespace bro::render
