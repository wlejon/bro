// The WebGL context's command stream: one open command buffer from the
// context's WebGLVkStream that every GPU operation is recorded into in API
// order, submitted (never waited on) at flushes, and the upload / layout /
// deferred-release helpers the API entry points build on.

#include "webgl/vulkan/webgl_vk_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <cstring>

namespace bro::webgl::vk {

VkCommandBuffer WebGLVkContext::commands() {
    return stream_.commands();
}

VkCommandBuffer WebGLVkContext::transferCommands() {
    endRendering();
    return commands();
}

void WebGLVkContext::flushCommands() {
    endRendering();
    stream_.submit();
    queriesSubmitted();
}

void WebGLVkContext::flushIfOverBudget() {
    if (stream_.wantsSubmit()) flushCommands();
}

bool WebGLVkContext::waitForCommands() {
    flushCommands();
    return context_.queue().wait(stream_.lastTicket());
}

render::UploadSlice WebGLVkContext::stage(const void* data, VkDeviceSize size, VkDeviceSize alignment) {
    render::UploadSlice slice = stream_.allocUpload(size, alignment);
    if (!slice) {
        LOG_ERROR("WebGLVkContext: out of upload memory (%llu bytes)", static_cast<unsigned long long>(size));
        setSyntheticError(GL_OUT_OF_MEMORY);
        return slice;
    }
    if (data) std::memcpy(slice.mapped, data, static_cast<size_t>(size));
    else std::memset(slice.mapped, 0, static_cast<size_t>(size));
    return slice;
}

void WebGLVkContext::uploadToBuffer(VkBufferResource& res, VkDeviceSize offset, const void* data,
                                    VkDeviceSize size) {
    ++res.version;
    if (!res.isValid() || size == 0) return;
    flushIfOverBudget();
    render::UploadSlice staging = stage(data, size, 4);
    if (!staging) return;
    VkCommandBuffer cmd = transferCommands();
    render::cmdBufferBarrier(cmd, res.buffer, offset, size,
                             kBufferReadStages, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferCopy region{staging.offset, offset, size};
    vkCmdCopyBuffer(cmd, staging.buffer, res.buffer, 1, &region);
    render::cmdBufferBarrier(cmd, res.buffer, offset, size,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             kBufferReadStages, kBufferReadAccess);
}

void WebGLVkContext::transitionTexture(VkCommandBuffer cmd, VkTextureResource& tex, VkImageLayout layout) {
    transitionTextureRange(cmd, tex, 0, tex.mipLevels, 0, tex.arrayLayers, layout);
}

// One barrier per run of layers sharing a layout, all in one call; a range
// in one layout throughout (the usual case) is one barrier.
void WebGLVkContext::transitionTextureRange(VkCommandBuffer cmd, VkTextureResource& tex, uint32_t level,
                                            uint32_t levelCount, uint32_t layer, uint32_t layerCount,
                                            VkImageLayout layout) {
    if (!tex.isValid() || levelCount == 0 || layerCount == 0) return;
    const VkImageAspectFlags aspect = render::imageAspectFor(tex.format);
    const uint32_t layers = tex.arrayLayers;
    auto at = [&](uint32_t l, uint32_t i) -> VkImageLayout& { return tex.layouts[static_cast<size_t>(l) * layers + i]; };

    std::vector<VkImageMemoryBarrier> barriers;
    VkPipelineStageFlags srcStages = 0, dstStages = 0;
    auto barrier = [&](VkImageLayout old, uint32_t l, uint32_t lc, uint32_t i, uint32_t ic) {
        if (old == layout) return;
        const render::LayoutUsage src = render::layoutUsage(old, /*asSource=*/true);
        const render::LayoutUsage dst = render::layoutUsage(layout, /*asSource=*/false);
        srcStages |= src.stages;
        dstStages |= dst.stages;
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = src.access;
        b.dstAccessMask = dst.access;
        b.oldLayout = old;
        b.newLayout = layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = tex.image;
        b.subresourceRange = {aspect, l, lc, i, ic};
        barriers.push_back(b);
    };

    const VkImageLayout first = at(level, layer);
    bool uniform = true;
    for (uint32_t l = level; uniform && l < level + levelCount; ++l)
        for (uint32_t i = layer; i < layer + layerCount; ++i)
            if (at(l, i) != first) {
                uniform = false;
                break;
            }
    if (uniform) {
        barrier(first, level, levelCount, layer, layerCount);
    } else {
        for (uint32_t l = level; l < level + levelCount; ++l) {
            for (uint32_t i = layer; i < layer + layerCount;) {
                const VkImageLayout old = at(l, i);
                uint32_t j = i + 1;
                while (j < layer + layerCount && at(l, j) == old) ++j;
                barrier(old, l, 1, i, j - i);
                i = j;
            }
        }
    }
    for (uint32_t l = level; l < level + levelCount; ++l)
        for (uint32_t i = layer; i < layer + layerCount; ++i) at(l, i) = layout;
    if (!barriers.empty())
        vkCmdPipelineBarrier(cmd, srcStages, dstStages, 0, 0, nullptr, 0, nullptr,
                             static_cast<uint32_t>(barriers.size()), barriers.data());
}

void WebGLVkContext::releaseBuffer(VkBufferResource& res) {
    if (res.buffer != VK_NULL_HANDLE || res.allocId != 0) {
        render::VulkanContext* ctx = &context_;
        VkBuffer buffer = res.buffer;
        uint64_t allocId = res.allocId;
        stream_.defer([ctx, buffer, allocId] { ctx->destroyBuffer(buffer, allocId); });
    }
    res.buffer = VK_NULL_HANDLE;
    res.memory = VK_NULL_HANDLE;
    res.allocId = 0;
    res.offset = 0;
}

void WebGLVkContext::releaseTexture(VkTextureResource& tex) {
    render::VulkanContext* ctx = &context_;
    VkDevice dev = context_.device();
    VkImage image = tex.image;
    std::vector<VkImageView> views;
    for (const auto& [key, v] : tex.sampledViews) views.push_back(v);
    for (const auto& [key, v] : tex.attachmentViews) views.push_back(v);
    uint64_t allocId = tex.allocId;
    if (image != VK_NULL_HANDLE || !views.empty()) {
        stream_.defer([ctx, dev, image, views = std::move(views), allocId] {
            for (VkImageView v : views) vkDestroyImageView(dev, v, nullptr);
            if (image != VK_NULL_HANDLE) ctx->destroyImage(image, allocId);
        });
    }
    tex.image = VK_NULL_HANDLE;
    tex.memory = VK_NULL_HANDLE;
    tex.sampledViews.clear();
    tex.attachmentViews.clear();
    tex.layouts.clear();
    tex.allocId = 0;
    tex.offset = 0;
}

void* WebGLVkContext::readbackMemory(VkDeviceSize size) {
    if (readback_.buffer != VK_NULL_HANDLE && readback_.size >= size) return readback_.mapped;
    if (readback_.buffer != VK_NULL_HANDLE) {
        render::VulkanContext* ctx = &context_;
        Readback dead = readback_;
        stream_.defer([ctx, dead] { ctx->destroyBuffer(dead.buffer, dead.allocId); });
        readback_ = Readback{};
    }
    // Cached memory makes the CPU read fast; coherent keeps it simple.
    for (VkMemoryPropertyFlags flags : {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
                                        VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)}) {
        if (render::findMemoryType(context_.memoryProperties(), ~0u, flags) &&
            context_.createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, flags, readback_.buffer,
                                  readback_.memory, readback_.offset, readback_.allocId, readback_.mapped) &&
            readback_.mapped) {
            readback_.size = size;
            return readback_.mapped;
        }
        if (readback_.buffer != VK_NULL_HANDLE) context_.destroyBuffer(readback_.buffer, readback_.allocId);
        readback_ = Readback{};
    }
    LOG_ERROR("WebGLVkContext: failed to allocate %llu bytes of readback memory",
              static_cast<unsigned long long>(size));
    setSyntheticError(GL_OUT_OF_MEMORY);
    return nullptr;
}

// Open a pass over the draw framebuffer: its attachments move to attachment
// layout and load what they hold. Nothing opens (and the GL error is set)
// when the framebuffer is incomplete.
void WebGLVkContext::beginRendering() {
    if (inRenderPass_) return;
    VkCommandBuffer cmd = commands();
    if (cmd == VK_NULL_HANDLE) return;
    RenderTarget target;
    if (!drawTarget(target) || target.extent.width == 0 || target.extent.height == 0) return;

    auto attachment = [&](const Surface& s, VkImageLayout layout) {
        VkRenderingAttachmentInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!s) return info;
        transitionSurface(cmd, s, layout);
        info.imageView = s.view;
        info.imageLayout = layout;
        info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        return info;
    };
    std::array<VkRenderingAttachmentInfo, 8> colors{};
    for (uint32_t i = 0; i < target.colorCount; ++i)
        colors[i] = attachment(target.color[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const VkRenderingAttachmentInfo depth =
        attachment(target.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    const VkRenderingAttachmentInfo stencil =
        attachment(target.stencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea.extent = target.extent;
    info.layerCount = 1;
    info.colorAttachmentCount = target.colorCount;
    info.pColorAttachments = colors.data();
    info.pDepthAttachment = target.depth ? &depth : nullptr;
    info.pStencilAttachment = target.stencil ? &stencil : nullptr;
    prepareOcclusionSlot(cmd);
    vkCmdBeginRendering(cmd, &info);
    beginOcclusionSlot(cmd);
    pass_ = target;
    inRenderPass_ = true;
}

void WebGLVkContext::endRendering() {
    if (!inRenderPass_) return;
    VkCommandBuffer cmd = stream_.commands();
    endOcclusionSlot(cmd);
    vkCmdEndRendering(cmd);
    inRenderPass_ = false;

    // What the pass wrote is visible to whatever reads or writes the images
    // next — another pass over them, a copy, or sampling.
    render::cmdMemoryBarrier(
        cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);

    // Textures go back to sampleable between passes.
    auto release = [&](const Surface& s) {
        if (s.source == Surface::Source::Texture)
            transitionSurface(cmd, s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };
    for (uint32_t i = 0; i < pass_.colorCount; ++i) release(pass_.color[i]);
    release(pass_.depth);
    release(pass_.stencil);
    pass_ = RenderTarget{};
}

} // namespace bro::webgl::vk
