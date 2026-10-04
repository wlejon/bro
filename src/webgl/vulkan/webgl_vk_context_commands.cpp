// The WebGL context's command stream: one command buffer from the frame ring
// that every GPU operation is recorded into in API order, submitted (never
// waited on) at flushes, and the upload / layout / deferred-release helpers
// the API entry points build on.

#include "webgl/vulkan/webgl_vk_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <cstring>

namespace bro::webgl::vk {

namespace {

// Every way a WebGL draw or copy can read a buffer.
constexpr VkPipelineStageFlags kBufferReadStages =
    VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
constexpr VkAccessFlags kBufferReadAccess =
    VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
    VK_ACCESS_TRANSFER_READ_BIT;

VkImageView createAttachmentView(VkDevice dev, const VkTextureResource& tex) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = tex.image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = tex.format;
    info.subresourceRange = {render::imageAspectFor(tex.format), 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &info, nullptr, &view) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkContext: failed to create a framebuffer attachment view");
        return VK_NULL_HANDLE;
    }
    return view;
}

} // namespace

VkCommandBuffer WebGLVkContext::commands() {
    if (currentCmd_ == VK_NULL_HANDLE) currentCmd_ = context_.frames().beginCommands();
    return currentCmd_;
}

VkCommandBuffer WebGLVkContext::transferCommands() {
    endRendering();
    return commands();
}

void WebGLVkContext::flushCommands() {
    endRendering();
    if (currentCmd_ == VK_NULL_HANDLE) return;
    const uint64_t ticket = context_.frames().submit(currentCmd_);
    currentCmd_ = VK_NULL_HANDLE;
    if (ticket == 0) {
        LOG_ERROR("WebGLVkContext: submitting recorded commands failed");
        return;
    }
    lastTicket_ = ticket;
}

bool WebGLVkContext::waitForCommands() {
    flushCommands();
    return context_.queue().wait(lastTicket_);
}

render::UploadSlice WebGLVkContext::stage(const void* data, VkDeviceSize size, VkDeviceSize alignment) {
    render::UploadSlice slice = context_.frames().allocUpload(size, alignment);
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
    if (!res.isValid() || size == 0) return;
    render::UploadSlice staging = stage(data, size, 4);
    if (!staging) return;
    VkCommandBuffer cmd = transferCommands();
    render::cmdBufferBarrier(cmd, res.buffer, offset, size,
                             kBufferReadStages, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferCopy region{staging.offset, offset, size};
    vkCmdCopyBuffer(cmd, staging.buffer, res.buffer, 1, &region);
    render::cmdBufferBarrier(cmd, res.buffer, offset, size,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             kBufferReadStages, kBufferReadAccess);
}

void WebGLVkContext::transitionTexture(VkCommandBuffer cmd, VkTextureResource& tex, VkImageLayout layout) {
    if (!tex.isValid() || tex.currentLayout == layout) return;
    const VkImageSubresourceRange range{render::imageAspectFor(tex.format), 0, tex.mipLevels, 0,
                                        tex.arrayLayers};
    render::cmdTransitionImage(cmd, tex.image, range, tex.currentLayout, layout);
    tex.currentLayout = layout;
}

void WebGLVkContext::releaseBuffer(VkBufferResource& res) {
    if (res.buffer != VK_NULL_HANDLE || res.allocId != 0) {
        render::VulkanContext* ctx = &context_;
        VkBuffer buffer = res.buffer;
        uint64_t allocId = res.allocId;
        context_.frames().defer([ctx, buffer, allocId] { ctx->destroyBuffer(buffer, allocId); });
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
    VkImageView view = tex.view;
    VkImageView attachment = tex.attachmentView;
    uint64_t allocId = tex.allocId;
    releaseSampler(tex.sampler);
    if (image != VK_NULL_HANDLE || view != VK_NULL_HANDLE || attachment != VK_NULL_HANDLE) {
        context_.frames().defer([ctx, dev, image, view, attachment, allocId] {
            if (attachment != VK_NULL_HANDLE) vkDestroyImageView(dev, attachment, nullptr);
            if (view != VK_NULL_HANDLE) vkDestroyImageView(dev, view, nullptr);
            ctx->destroyImage(image, allocId);
        });
    }
    tex.image = VK_NULL_HANDLE;
    tex.memory = VK_NULL_HANDLE;
    tex.view = VK_NULL_HANDLE;
    tex.attachmentView = VK_NULL_HANDLE;
    tex.allocId = 0;
    tex.offset = 0;
    tex.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    tex.samplerDirty = true;
}

void WebGLVkContext::releaseSampler(VkSampler& sampler) {
    if (sampler == VK_NULL_HANDLE) return;
    VkDevice dev = context_.device();
    VkSampler dead = sampler;
    context_.frames().defer([dev, dead] { vkDestroySampler(dev, dead, nullptr); });
    sampler = VK_NULL_HANDLE;
}

void* WebGLVkContext::readbackMemory(VkDeviceSize size) {
    if (readback_.buffer != VK_NULL_HANDLE && readback_.size >= size) return readback_.mapped;
    if (readback_.buffer != VK_NULL_HANDLE) {
        render::VulkanContext* ctx = &context_;
        Readback dead = readback_;
        context_.frames().defer([ctx, dead] { ctx->destroyBuffer(dead.buffer, dead.allocId); });
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

void WebGLVkContext::beginRendering() {
    if (inRenderPass_) return;
    VkCommandBuffer cmd = commands();
    if (cmd == VK_NULL_HANDLE) return;

    std::vector<VkRenderingAttachmentInfo> colorAttachments;
    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.layerCount = 1;

    if (currentFboId_ != 0) {
        auto itFbo = framebuffers_.find(currentFboId_);
        if (itFbo == framebuffers_.end()) return;
        VkFramebufferResource& fbo = itFbo->second;
        uint32_t fboWidth = 0;
        uint32_t fboHeight = 0;

        auto attach = [&](GLuint texId) {
            VkRenderingAttachmentInfo att{};
            att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            auto itTex = texId != 0 ? textures_.find(texId) : textures_.end();
            if (itTex != textures_.end() && itTex->second.isValid()) {
                VkTextureResource& tex = itTex->second;
                if (tex.attachmentView == VK_NULL_HANDLE)
                    tex.attachmentView = createAttachmentView(context_.device(), tex);
                transitionTexture(cmd, tex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                fboWidth = tex.width;
                fboHeight = tex.height;
                att.imageView = tex.attachmentView;
                att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            colorAttachments.push_back(att);
        };

        for (GLenum db : fbo.drawBuffers) {
            GLuint texId = 0;
            if (db >= 0x8CE0 && db <= 0x8CE7) {
                uint32_t idx = db - 0x8CE0;
                if (idx < fbo.colorAttachments.size()) texId = fbo.colorAttachments[idx];
                if (texId == 0 && idx == 0) texId = fbo.colorAttachmentTex;
            }
            attach(texId);
        }
        if (colorAttachments.empty()) {
            GLuint single = fbo.colorAttachments[0] != 0 ? fbo.colorAttachments[0] : fbo.colorAttachmentTex;
            if (single != 0) attach(single);
        }

        renderingInfo.renderArea.extent = {fboWidth, fboHeight};
        renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
        renderingInfo.pColorAttachments = colorAttachments.data();
        vkCmdBeginRendering(cmd, &renderingInfo);
        inRenderPass_ = true;
        return;
    }

    canvas_.transitionToColorAttachment(cmd);
    if (canvas_.depthImage() != VK_NULL_HANDLE)
        canvas_.transitionDepth(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = canvas_.colorView();
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = canvas_.depthView();
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    const bool hasDepth = canvas_.depthView() != VK_NULL_HANDLE;
    const bool hasStencil = hasDepth && (render::imageAspectFor(canvas_.depthFormat()) & VK_IMAGE_ASPECT_STENCIL_BIT);
    renderingInfo.renderArea.extent = {canvas_.width(), canvas_.height()};
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = hasDepth ? &depthAttachment : nullptr;
    renderingInfo.pStencilAttachment = hasStencil ? &depthAttachment : nullptr;
    vkCmdBeginRendering(cmd, &renderingInfo);
    inRenderPass_ = true;
}

void WebGLVkContext::endRendering() {
    if (!inRenderPass_ || currentCmd_ == VK_NULL_HANDLE) return;
    vkCmdEndRendering(currentCmd_);
    inRenderPass_ = false;

    // Framebuffer textures go back to sampleable between passes.
    if (currentFboId_ == 0) return;
    auto itFbo = framebuffers_.find(currentFboId_);
    if (itFbo == framebuffers_.end()) return;
    auto release = [&](GLuint texId) {
        auto itTex = texId != 0 ? textures_.find(texId) : textures_.end();
        if (itTex != textures_.end() && itTex->second.currentLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            transitionTexture(currentCmd_, itTex->second, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };
    for (GLuint tid : itFbo->second.colorAttachments) release(tid);
    release(itFbo->second.colorAttachmentTex);
}

} // namespace bro::webgl::vk
