#include "scene/vulkan/scene_occlusion_readback.h"

#include "render/vulkan_util.h"
#include "scene/vulkan/scene_vk_depth.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace bro::scene::vk {

namespace {
uint32_t levelSize(uint32_t size, uint32_t level) {
    return std::max(size >> level, 1u);
}
}  // namespace

bool SceneOcclusionReadback::resize(SceneVkAllocator& allocator, uint32_t width, uint32_t height, uint32_t levels,
                                    uint32_t frameWidth, uint32_t frameHeight) {
    cleanup(allocator);
    if (levels == 0) return true;
    firstLevel_ = 0;
    while (firstLevel_ + 1 < levels && levelSize(width, firstLevel_) > kMaxWidth) ++firstLevel_;
    levels_ = levels - firstLevel_;
    width_ = width;
    height_ = height;
    frameWidth_ = frameWidth;
    frameHeight_ = frameHeight;
    VkDeviceSize bytes = 0;
    for (uint32_t l = 0; l < levels_; ++l)
        bytes += VkDeviceSize{levelSize(width, firstLevel_ + l)} * levelSize(height, firstLevel_ + l) * 4;
    for (Slot& s : slots_) {
        if (!allocator.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                    s.buffer) ||
            !s.buffer.mappedData) {
            cleanup(allocator);
            return false;
        }
    }
    return true;
}

void SceneOcclusionReadback::cleanup(SceneVkAllocator& allocator) {
    for (Slot& s : slots_) {
        if (s.buffer.isValid()) allocator.destroyBuffer(s.buffer);
        s = {};
    }
    levels_ = 0;
    snapshot_.clear();
}

void SceneOcclusionReadback::record(VkCommandBuffer cmd, VkImage hiZ, uint32_t slot, uint64_t frame,
                                    const bromath::Mat4& proj, const bromath::Mat4& relView,
                                    const bromath::Vec3& eye) {
    Slot& s = slots_[slot];
    if (!s.buffer.isValid()) return;
    // (No one asking, no copies; nor a stale snapshot to find.)
    if (askedAt_ == 0 || frame > askedAt_ + kIdleFrames) {
        s.frame = 0;
        snapshot_.clear();
        return;
    }
    VkBufferImageCopy regions[16];
    VkDeviceSize at = 0;
    const uint32_t n = std::min<uint32_t>(levels_, 16);
    for (uint32_t l = 0; l < n; ++l) {
        const uint32_t w = levelSize(width_, firstLevel_ + l), h = levelSize(height_, firstLevel_ + l);
        VkBufferImageCopy& r = regions[l];
        r = {};
        r.bufferOffset = at;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, firstLevel_ + l, 0, 1};
        r.imageExtent = {w, h, 1};
        at += VkDeviceSize{w} * h * 4;
    }
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cmd, hiZ, VK_IMAGE_LAYOUT_GENERAL, s.buffer.buffer, n, regions);
    // The host reads it once the slot's frame is done; and the compute
    // stage's later barriers (the pyramid's next layout) come after the copy.
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_HOST_READ_BIT);
    s.frame = frame;
    s.proj = proj;
    s.relView = relView;
    s.eye = eye;
}

void SceneOcclusionReadback::harvest(uint32_t slot, uint64_t frame) {
    Slot& s = slots_[slot];
    // (A copy of this very frame is not done yet.)
    if (!s.buffer.isValid() || s.frame == 0 || s.frame >= frame || s.frame <= snapshot_.frame()) return;
    std::vector<OcclusionSnapshot::Level> levels(std::min<uint32_t>(levels_, 16));
    const auto* src = static_cast<const uint8_t*>(s.buffer.mappedData);
    for (uint32_t l = 0; l < levels.size(); ++l) {
        OcclusionSnapshot::Level& lv = levels[l];
        lv.width = levelSize(width_, firstLevel_ + l);
        lv.height = levelSize(height_, firstLevel_ + l);
        lv.depth.resize(size_t{lv.width} * lv.height);
        std::memcpy(lv.depth.data(), src, lv.depth.size() * 4);
        src += lv.depth.size() * 4;
    }
    snapshot_.set(std::move(levels), static_cast<int>(firstLevel_), frameWidth_, frameHeight_, s.proj, s.relView,
                  s.eye, reversedZ(), s.frame);
}

}  // namespace bro::scene::vk
