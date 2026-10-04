#include "webgl/vulkan/webgl_vk_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace bro::webgl::vk {

WebGLFramebuffer WebGLVkContext::createFramebuffer() {
    GLuint id = nextFboId_++;
    framebuffers_[id] = VkFramebufferResource{};
    return {id};
}

void WebGLVkContext::deleteFramebuffer(WebGLFramebuffer fb) {
    if (currentFboId_ == fb.id) endRendering();
    framebuffers_.erase(fb.id);
    if (currentFboId_ == fb.id) currentFboId_ = 0;
    if (readFboId_ == fb.id) readFboId_ = 0;
    if (drawFboId_ == fb.id) drawFboId_ = 0;
}

void WebGLVkContext::bindFramebuffer(GLenum target, WebGLFramebuffer fb) {
    if (target == 0x8CA8 /* GL_READ_FRAMEBUFFER */) {
        readFboId_ = fb.id;
        return;
    }
    if (target == 0x8CA9 /* GL_DRAW_FRAMEBUFFER */) {
        drawFboId_ = fb.id;
        return;
    }
    // GL_FRAMEBUFFER sets both and current
    readFboId_ = fb.id;
    drawFboId_ = fb.id;
    if (currentFboId_ != fb.id) {
        endRendering();
        currentFboId_ = fb.id;
    }
}

static GLuint getTargetFbo(GLenum target, GLuint currentFbo, GLuint drawFbo, GLuint readFbo) {
    if (target == 0x8CA9 /* GL_DRAW_FRAMEBUFFER */) {
        return (drawFbo != 0) ? drawFbo : currentFbo;
    }
    if (target == 0x8CA8 /* GL_READ_FRAMEBUFFER */) {
        return (readFbo != 0) ? readFbo : currentFbo;
    }
    return currentFbo;
}

void WebGLVkContext::framebufferTexture2D(GLenum target, GLenum attachment, GLenum /*textarget*/,
                                         WebGLTexture tex, GLint /*level*/) {
    GLuint fboId = getTargetFbo(target, currentFboId_, drawFboId_, readFboId_);
    if (fboId == 0) return;
    VkFramebufferResource& fbo = framebuffers_[fboId];
    if (attachment == GL_COLOR_ATTACHMENT0) {
        fbo.colorAttachments[0] = tex.id;
        fbo.colorAttachmentTex = tex.id;
    } else if (attachment >= 0x8CE1 && attachment <= 0x8CE7 /* GL_COLOR_ATTACHMENT1..7 */) {
        fbo.colorAttachments[attachment - 0x8CE0] = tex.id;
    } else if (attachment == GL_DEPTH_ATTACHMENT || attachment == 0x821A /* GL_DEPTH_STENCIL_ATTACHMENT */) {
        fbo.depthAttachmentTex = tex.id;
    }
}

void WebGLVkContext::framebufferRenderbuffer(GLenum target, GLenum attachment,
                                             GLenum /*renderbuffertarget*/, WebGLRenderbuffer rbo) {
    GLuint fboId = getTargetFbo(target, currentFboId_, drawFboId_, readFboId_);
    if (fboId == 0) return;
    VkFramebufferResource& fbo = framebuffers_[fboId];
    GLuint texId = 0;
    if (rbo.id != 0) {
        auto itRb = renderbuffers_.find(rbo.id);
        if (itRb != renderbuffers_.end()) texId = itRb->second.textureId;
    }

    if (attachment == GL_COLOR_ATTACHMENT0) {
        fbo.colorAttachments[0] = texId;
        fbo.colorAttachmentTex = texId;
    } else if (attachment >= 0x8CE1 && attachment <= 0x8CE7 /* GL_COLOR_ATTACHMENT1..7 */) {
        fbo.colorAttachments[attachment - 0x8CE0] = texId;
    } else if (attachment == GL_DEPTH_ATTACHMENT || attachment == 0x821A /* GL_DEPTH_STENCIL_ATTACHMENT */) {
        fbo.depthAttachmentTex = texId;
    }
}

GLenum WebGLVkContext::checkFramebufferStatus(GLenum target) {
    GLuint fboId = getTargetFbo(target, currentFboId_, drawFboId_, readFboId_);
    if (fboId == 0) return GL_FRAMEBUFFER_COMPLETE;
    auto itFbo = framebuffers_.find(fboId);
    if (itFbo == framebuffers_.end()) return 0x8CD7; // GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT
    const VkFramebufferResource& fbo = itFbo->second;
    bool hasAttachment = (fbo.depthAttachmentTex != 0);
    for (GLuint tex : fbo.colorAttachments) {
        if (tex != 0) { hasAttachment = true; break; }
    }
    if (!hasAttachment && fbo.colorAttachmentTex != 0) hasAttachment = true;
    if (!hasAttachment) {
        return 0x8CD7; // GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT
    }
    return GL_FRAMEBUFFER_COMPLETE;
}

void WebGLVkContext::drawBuffers(GLsizei n, const GLenum* bufs) {
    GLuint fboId = (drawFboId_ != 0) ? drawFboId_ : currentFboId_;
    if (fboId == 0) return;
    VkFramebufferResource& fbo = framebuffers_[fboId];
    fbo.drawBuffers.clear();
    for (GLsizei i = 0; i < n; ++i) {
        fbo.drawBuffers.push_back(bufs[i]);
    }
}

void WebGLVkContext::readBuffer(GLenum src) {
    GLuint fboId = (readFboId_ != 0) ? readFboId_ : currentFboId_;
    if (fboId == 0) return;
    framebuffers_[fboId].readBuffer = src;
}

void WebGLVkContext::blitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                     GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                     GLbitfield /*mask*/, GLenum filter) {
    GLuint srcFbo = (readFboId_ != 0) ? readFboId_ : currentFboId_;
    GLuint dstFbo = (drawFboId_ != 0) ? drawFboId_ : currentFboId_;
    if (srcFbo == 0 || dstFbo == 0) return;

    auto itSrc = framebuffers_.find(srcFbo);
    auto itDst = framebuffers_.find(dstFbo);
    if (itSrc == framebuffers_.end() || itDst == framebuffers_.end()) return;

    GLuint srcTexId = itSrc->second.colorAttachments[0];
    if (srcTexId == 0) srcTexId = itSrc->second.colorAttachmentTex;
    GLuint dstTexId = itDst->second.colorAttachments[0];
    if (dstTexId == 0) dstTexId = itDst->second.colorAttachmentTex;
    if (srcTexId == 0 || dstTexId == 0) return;

    auto itSrcTex = textures_.find(srcTexId);
    auto itDstTex = textures_.find(dstTexId);
    if (itSrcTex == textures_.end() || itDstTex == textures_.end()) return;

    VkTextureResource& srcTex = itSrcTex->second;
    VkTextureResource& dstTex = itDstTex->second;
    if (!srcTex.isValid() || !dstTex.isValid()) return;

    if (&srcTex == &dstTex) return;  // overlapping self-blit is undefined in GL

    VkCommandBuffer cmd = transferCommands();
    transitionTexture(cmd, srcTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transitionTexture(cmd, dstTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageBlit blitRegion{};
    blitRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blitRegion.srcOffsets[0] = {srcX0, srcY0, 0};
    blitRegion.srcOffsets[1] = {srcX1, srcY1, 1};
    blitRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blitRegion.dstOffsets[0] = {dstX0, dstY0, 0};
    blitRegion.dstOffsets[1] = {dstX1, dstY1, 1};

    VkFilter vkFilter = (filter == GL_LINEAR) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    vkCmdBlitImage(cmd, srcTex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstTex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blitRegion, vkFilter);

    transitionTexture(cmd, srcTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionTexture(cmd, dstTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

WebGLRenderbuffer WebGLVkContext::createRenderbuffer() {
    GLuint id = nextRenderbufferId_++;
    renderbuffers_[id] = VkRenderbufferResource{};
    return {id};
}

void WebGLVkContext::deleteRenderbuffer(WebGLRenderbuffer rbo) {
    auto it = renderbuffers_.find(rbo.id);
    if (it != renderbuffers_.end()) {
        if (it->second.textureId != 0) {
            deleteTexture({it->second.textureId});
        }
        renderbuffers_.erase(it);
    }
    if (currentRenderbufferId_ == rbo.id) currentRenderbufferId_ = 0;
}

void WebGLVkContext::bindRenderbuffer(GLenum /*target*/, WebGLRenderbuffer rbo) {
    currentRenderbufferId_ = rbo.id;
    if (rbo.id != 0 && renderbuffers_.find(rbo.id) == renderbuffers_.end()) {
        renderbuffers_[rbo.id] = VkRenderbufferResource{};
    }
}

void WebGLVkContext::renderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    renderbufferStorageMultisample(target, 0, internalformat, width, height);
}

void WebGLVkContext::renderbufferStorageMultisample(GLenum /*target*/, GLsizei samples, GLenum internalformat,
                                                    GLsizei width, GLsizei height) {
    if (currentRenderbufferId_ == 0 || width <= 0 || height <= 0) return;
    VkRenderbufferResource& rb = renderbuffers_[currentRenderbufferId_];
    if (rb.textureId != 0) {
        deleteTexture({rb.textureId});
        rb.textureId = 0;
    }

    WebGLTexture tex = createTexture();
    rb.textureId = tex.id;
    rb.internalformat = internalformat;
    rb.width = width;
    rb.height = height;
    rb.samples = samples;

    GLuint savedUnit = activeTextureUnit_;
    GLuint savedTex = (savedUnit < boundTextures2D_.size()) ? boundTextures2D_[savedUnit] : 0;

    activeTexture(GL_TEXTURE0);
    bindTexture(GL_TEXTURE_2D, tex);

    GLenum format = GL_RGBA;
    GLenum type = GL_UNSIGNED_BYTE;
    if (internalformat == 0x81A5 /* DEPTH_COMPONENT16 */ || internalformat == 0x81A6 /* DEPTH_COMPONENT24 */) {
        format = 0x1902;
        type = GL_UNSIGNED_SHORT;
    } else if (internalformat == 0x88F0 /* DEPTH24_STENCIL8 */) {
        format = 0x84F9;
        type = 0x84FA;
    } else if (internalformat == GL_RGBA32F) {
        format = GL_RGBA;
        type = GL_FLOAT;
    }

    texImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalformat), width, height, 0, format, type, nullptr);

    bindTexture(GL_TEXTURE_2D, {savedTex});
    activeTexture(GL_TEXTURE0 + savedUnit);
}

void WebGLVkContext::readPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                                GLenum /*format*/, GLenum type, void* pixels) {
    if (!pixels || width <= 0 || height <= 0) return;

    GLuint fboId = (readFboId_ != 0) ? readFboId_ : currentFboId_;
    if (fboId == 0) {
        std::vector<uint8_t> canvasData;
        if (!readCanvasPixels(canvasData)) return;

        size_t canvasW = canvas_.width();
        size_t canvasH = canvas_.height();

        uint8_t* dst = static_cast<uint8_t*>(pixels);
        for (GLsizei row = 0; row < height; ++row) {
            GLint srcY = static_cast<GLint>(canvasH) - 1 - (y + row);
            if (srcY >= 0 && static_cast<size_t>(srcY) < canvasH) {
                GLint srcX = std::max(0, x);
                GLsizei copyW = std::min(width, static_cast<GLsizei>(canvasW - srcX));
                if (copyW > 0) {
                    const uint8_t* srcRow = canvasData.data() + (srcY * canvasW + srcX) * 4;
                    std::memcpy(dst + row * width * 4, srcRow, copyW * 4);
                }
            }
        }
    } else {
        auto itFbo = framebuffers_.find(fboId);
        if (itFbo == framebuffers_.end()) return;

        GLuint readTexId = itFbo->second.colorAttachmentTex;
        GLenum rb = itFbo->second.readBuffer;
        if (rb >= 0x8CE0 && rb <= 0x8CE7) {
            uint32_t idx = rb - 0x8CE0;
            if (idx < itFbo->second.colorAttachments.size() && itFbo->second.colorAttachments[idx] != 0) {
                readTexId = itFbo->second.colorAttachments[idx];
            }
        }
        if (readTexId == 0) return;

        auto itTex = textures_.find(readTexId);
        if (itTex == textures_.end() || !itTex->second.isValid()) return;

        VkTextureResource& tex = itTex->second;
        if (render::imageAspectFor(tex.format) != VK_IMAGE_ASPECT_COLOR_BIT) return;
        const VkDeviceSize imgSize = static_cast<VkDeviceSize>(tex.width) * tex.height * tex.bytesPerPixel;
        void* mapped = readbackMemory(imgSize);
        if (!mapped) return;

        VkCommandBuffer cmd = transferCommands();
        transitionTexture(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copyRegion.imageExtent = {tex.width, tex.height, 1};
        vkCmdCopyImageToBuffer(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_.buffer, 1, &copyRegion);
        render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        transitionTexture(cmd, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!waitForCommands()) return;

        const uint8_t* srcData = static_cast<const uint8_t*>(mapped);
        uint8_t* dst = static_cast<uint8_t*>(pixels);
        GLsizei pixelBytes = (type == GL_FLOAT) ? 16 : static_cast<GLsizei>(tex.bytesPerPixel);
        for (GLsizei row = 0; row < height; ++row) {
            GLint srcY = y + row;
            if (srcY >= 0 && static_cast<size_t>(srcY) < tex.height) {
                GLint srcX = std::max(0, x);
                GLsizei copyW = std::min(width, static_cast<GLsizei>(tex.width - srcX));
                if (copyW > 0) {
                    const uint8_t* srcRow = srcData + (srcY * tex.width + srcX) * tex.bytesPerPixel;
                    std::memcpy(dst + row * width * pixelBytes, srcRow, copyW * pixelBytes);
                }
            }
        }
    }
}

} // namespace bro::webgl::vk
