#include "scene/vulkan/world/scene_surface_layers.h"

#include "render/vulkan_util.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_device.h"

#include <algorithm>
#include <cstring>

namespace bro::scene::vk {

namespace {

VkSampler makeSampler(VkDevice dev, VkFilter filter) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = filter;
    info.minFilter = filter;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    info.maxLod = 0.0f;
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(dev, &info, nullptr, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    return s;
}

}  // namespace

bool SceneSurfaceLayerAtlas::create(SceneVkDevice& device, SceneVkAllocator& allocator,
                                    const SurfaceLayerConfig& config, int levels, std::span<const VkFormat> formats,
                                    Fill fill) {
    destroy(device, allocator);
    const int count = std::min(static_cast<int>(formats.size()), kMaxLayers);
    fill_ = fill;
    stride_ = static_cast<uint32_t>(config.stride());
    slots_ = config.slots;
    levels_ = levels;
    pageSize_ = config.pageSize;
    const VkImageUsageFlags written =
        fill == Fill::Compute ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    for (int i = 0; i < count; ++i) {
        if (!allocator.createImage(stride_, stride_, formats[i], VK_IMAGE_USAGE_SAMPLED_BIT | written,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, layers_[i], 1, VK_SAMPLE_COUNT_1_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(slots_), 0,
                                   VK_IMAGE_VIEW_TYPE_2D_ARRAY)) {
            destroy(device, allocator);
            return false;
        }
        slotViews_[i].assign(static_cast<size_t>(slots_), VK_NULL_HANDLE);
    }
    if (!allocator.createImage(static_cast<uint32_t>(pageSize_), static_cast<uint32_t>(pageSize_), VK_FORMAT_R32_UINT,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, page_, 1, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(std::max(levels_, 1)), 0,
                               VK_IMAGE_VIEW_TYPE_2D_ARRAY) ||
        (fill == Fill::Render &&
         !allocator.createImage(stride_, stride_, SceneTargets::kDepthFormat,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                depth_, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))) {
        destroy(device, allocator);
        return false;
    }
    linear_ = makeSampler(device.device(), VK_FILTER_LINEAR);
    nearest_ = makeSampler(device.device(), VK_FILTER_NEAREST);
    layerCount_ = count;
    if (!linear_ || !nearest_) {
        destroy(device, allocator);
        return false;
    }
    return true;
}

void SceneSurfaceLayerAtlas::destroy(SceneVkDevice& device, SceneVkAllocator& allocator) {
    VkDevice dev = device.device();
    std::vector<VkImageView> views;
    for (auto& list : slotViews_) {
        for (VkImageView v : list)
            if (v != VK_NULL_HANDLE) views.push_back(v);
        list.clear();
    }
    const VkSampler samplers[2] = {linear_, nearest_};
    if (!views.empty() || linear_ || nearest_) {
        device.defer([dev, views = std::move(views), samplers] {
            for (VkImageView v : views) vkDestroyImageView(dev, v, nullptr);
            for (VkSampler s : samplers)
                if (s != VK_NULL_HANDLE) vkDestroySampler(dev, s, nullptr);
        });
    }
    linear_ = nearest_ = VK_NULL_HANDLE;
    for (SceneVkImage& img : layers_)
        if (img.isValid()) allocator.destroyImage(img);
    if (page_.isValid()) allocator.destroyImage(page_);
    if (depth_.isValid()) allocator.destroyImage(depth_);
    layerCount_ = 0;
}

bool SceneSurfaceLayerAtlas::fits(const SurfaceLayerConfig& config, int levels) const {
    return valid() && slots_ == config.slots && stride_ == static_cast<uint32_t>(config.stride()) &&
           levels_ == levels && pageSize_ == config.pageSize;
}

TargetFormat SceneSurfaceLayerAtlas::target() const {
    TargetFormat t;
    for (int i = 0; i < layerCount_; ++i) t.color[i] = layers_[i].format;
    t.colorCount = static_cast<uint32_t>(layerCount_);
    t.depth = SceneTargets::kDepthFormat;
    return t;
}

VkImageView SceneSurfaceLayerAtlas::slotView(SceneVkDevice& device, int layer, int slot) {
    VkImageView& v = slotViews_[layer][static_cast<size_t>(slot)];
    if (v != VK_NULL_HANDLE) return v;
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = layers_[layer].image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = layers_[layer].format;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, static_cast<uint32_t>(slot), 1};
    if (vkCreateImageView(device.device(), &info, nullptr, &v) != VK_SUCCESS) v = VK_NULL_HANDLE;
    return v;
}

void SceneSurfaceLayerAtlas::slotBarrier(VkCommandBuffer cmd, std::span<const SurfaceFill> fills,
                                         bool toTarget) const {
    std::vector<VkImageMemoryBarrier> barriers;
    barriers.reserve(fills.size() * static_cast<size_t>(layerCount_));
    for (const SurfaceFill& f : fills) {
        for (int i = 0; i < layerCount_; ++i) {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.image = layers_[i].image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, static_cast<uint32_t>(f.slot), 1};
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            if (fill_ == Fill::Compute) {
                b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b.srcAccessMask = toTarget ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT;
                b.dstAccessMask = toTarget ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
            } else if (toTarget) {
                // (Its old contents go: the fill clears it.)
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                b.srcAccessMask = 0;
                b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            } else {
                b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            }
            barriers.push_back(b);
        }
    }
    if (barriers.empty()) return;
    // Into a target: after every earlier read of the slot (in an earlier
    // frame). Back: before this frame's reads.
    const VkPipelineStageFlags writer = fill_ == Fill::Compute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                                               : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags src = toTarget ? kReaders : writer;
    const VkPipelineStageFlags dst = toTarget ? writer : kReaders;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(barriers.size()),
                         barriers.data());
}

void SceneSurfaceLayerAtlas::beginFills(VkCommandBuffer cmd, std::span<const SurfaceFill> fills) const {
    slotBarrier(cmd, fills, true);
}

void SceneSurfaceLayerAtlas::endFills(VkCommandBuffer cmd, std::span<const SurfaceFill> fills) const {
    slotBarrier(cmd, fills, false);
}

void SceneSurfaceLayerAtlas::beginDepth(VkCommandBuffer cmd) const {
    render::ImageBarrier b;
    b.image = depth_.image;
    b.range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    b.srcStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    b.srcAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    b.dstAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    render::cmdImageBarrier(cmd, b);
}

void SceneSurfaceLayerAtlas::writePages(SceneVkDevice& device, VkCommandBuffer cmd,
                                        std::span<const SurfacePageWrite> writes) {
    if (writes.empty() || !page_.isValid()) return;
    const render::UploadSlice slice = device.frameUpload(writes.size() * sizeof(uint32_t), 4);
    if (!slice) return;
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(writes.size());
    auto* words = static_cast<uint32_t*>(slice.mapped);
    for (size_t i = 0; i < writes.size(); ++i) {
        const SurfacePageWrite& w = writes[i];
        words[i] = w.value;
        VkBufferImageCopy r{};
        r.bufferOffset = slice.offset + i * sizeof(uint32_t);
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(w.layer), 1};
        r.imageOffset = {w.ex, w.ez, 0};
        r.imageExtent = {1, 1, 1};
        regions.push_back(r);
    }
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, static_cast<uint32_t>(std::max(levels_, 1))};
    render::ImageBarrier in;
    in.image = page_.image;
    in.range = all;
    in.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    in.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    in.srcStages = kReaders;
    in.dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    in.dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    render::cmdImageBarrier(cmd, in);
    // (Several writes to one entry in a frame land in order: an eviction's
    // zero, then the new tile's.)
    vkCmdCopyBufferToImage(cmd, slice.buffer, page_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());
    render::ImageBarrier out = in;
    out.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    out.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    out.srcStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    out.srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    out.dstStages = kReaders;
    out.dstAccess = VK_ACCESS_SHADER_READ_BIT;
    render::cmdImageBarrier(cmd, out);
}

void SceneSurfaceLayerAtlas::clearPages(VkCommandBuffer cmd) {
    if (!page_.isValid()) return;
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, static_cast<uint32_t>(std::max(levels_, 1))};
    // (From UNDEFINED: the table is cleared whatever it held, and a new one
    // has never been in any other layout.)
    render::ImageBarrier in;
    in.image = page_.image;
    in.range = all;
    in.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    in.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    in.srcStages = kReaders;
    in.dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    in.dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    render::cmdImageBarrier(cmd, in);
    const VkClearColorValue zero{};
    vkCmdClearColorImage(cmd, page_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
    render::ImageBarrier out = in;
    out.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    out.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    out.srcStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    out.srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    out.dstStages = kReaders;
    out.dstAccess = VK_ACCESS_SHADER_READ_BIT;
    render::cmdImageBarrier(cmd, out);
    // A compute-filled atlas's layers into GENERAL, where they stay.
    if (fill_ == Fill::Compute) {
        for (int i = 0; i < layerCount_; ++i) {
            render::ImageBarrier b;
            b.image = layers_[i].image;
            b.range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, static_cast<uint32_t>(slots_)};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcStages = kReaders;
            b.dstStages = kReaders | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            b.dstAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            render::cmdImageBarrier(cmd, b);
        }
    }
}

}  // namespace bro::scene::vk
