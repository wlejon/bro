// Renderbuffer objects: storage (multisampled when asked, cleared as WebGL
// promises), its parameters, and the sample counts a format supports.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_formats.h"
#include "render/vulkan_util.h"
#include "util/log.h"

namespace bro::webgl::vk {

namespace {

// RENDERBUFFER_* parameter names.
constexpr GLenum kRenderbufferWidth = 0x8D42;
constexpr GLenum kRenderbufferHeight = 0x8D43;
constexpr GLenum kRenderbufferInternalFormat = 0x8D44;
constexpr GLenum kRenderbufferRedSize = 0x8D50;
constexpr GLenum kRenderbufferStencilSize = 0x8D55;
constexpr GLenum kRenderbufferSamples = 0x8CAB;

} // namespace

WebGLRenderbuffer WebGLVkContext::createRenderbuffer() {
    GLuint id = nextObjectId_++;
    renderbuffers_[id] = VkRenderbufferResource{};
    return {id};
}

GLboolean WebGLVkContext::isRenderbuffer(WebGLRenderbuffer rbo) const {
    auto it = rbo.id != 0 ? renderbuffers_.find(rbo.id) : renderbuffers_.end();
    return it != renderbuffers_.end() && it->second.everBound ? GL_TRUE : GL_FALSE;
}

void WebGLVkContext::deleteRenderbuffer(WebGLRenderbuffer rbo) {
    auto it = rbo.id != 0 ? renderbuffers_.find(rbo.id) : renderbuffers_.end();
    if (it == renderbuffers_.end()) return;
    // Detached from the bound framebuffers (GL leaves other framebuffers
    // naming it, now incomplete).
    const VkFboAttachment att{VkFboAttachment::Kind::Renderbuffer, rbo.id, 0, 0};
    for (GLuint fboId : {drawFboId_, readFboId_}) {
        auto fIt = fboId != 0 ? framebuffers_.find(fboId) : framebuffers_.end();
        if (fIt == framebuffers_.end()) continue;
        framebufferChanged(fboId);
        for (VkFboAttachment& c : fIt->second.color)
            if (c == att) c = {};
        if (fIt->second.depth == att) fIt->second.depth = {};
        if (fIt->second.stencil == att) fIt->second.stencil = {};
    }
    endRendering();
    releaseTexture(it->second.storage);
    renderbuffers_.erase(it);
    if (currentRenderbufferId_ == rbo.id) currentRenderbufferId_ = 0;
}

void WebGLVkContext::bindRenderbuffer(GLenum target, WebGLRenderbuffer rbo) {
    if (target != GL_RENDERBUFFER) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    auto it = rbo.id != 0 ? renderbuffers_.find(rbo.id) : renderbuffers_.end();
    if (rbo.id != 0 && it == renderbuffers_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (it != renderbuffers_.end()) it->second.everBound = true;
    currentRenderbufferId_ = rbo.id;
}

std::vector<GLint> WebGLVkContext::supportedSampleCounts(GLenum internalformat) {
    std::vector<GLint> counts;
    VkFormat format = colorRenderableFormat(internalformat);
    if (format == VK_FORMAT_UNDEFINED) format = depthStencilFormat(context_.physicalDevice(), internalformat);
    // ES 3.0: integer formats are never multisampled.
    if (format == VK_FORMAT_UNDEFINED || isIntegerFormat(format)) return counts;
    const VkImageAspectFlags aspects = render::imageAspectFor(format);
    const VkImageUsageFlags usage = (aspects == VK_IMAGE_ASPECT_COLOR_BIT
                                         ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                         : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImageFormatProperties props{};
    if (vkGetPhysicalDeviceImageFormatProperties(context_.physicalDevice(), format, VK_IMAGE_TYPE_2D,
                                                 VK_IMAGE_TILING_OPTIMAL, usage, 0, &props) != VK_SUCCESS)
        return counts;
    const VkPhysicalDeviceLimits& limits = context_.deviceProperties().limits;
    VkSampleCountFlags flags = props.sampleCounts;
    if (aspects & VK_IMAGE_ASPECT_COLOR_BIT) flags &= limits.framebufferColorSampleCounts;
    if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) flags &= limits.framebufferDepthSampleCounts;
    if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) flags &= limits.framebufferStencilSampleCounts;
    for (GLint n = 64; n > 1; n /= 2)
        if (flags & static_cast<VkSampleCountFlags>(n)) counts.push_back(n);
    return counts;
}

void WebGLVkContext::renderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    renderbufferStorageMultisample(target, 0, internalformat, width, height);
}

// Fresh storage for the bound renderbuffer, cleared as WebGL promises (color
// 0, depth 1, stencil 0) and left in attachment layout. The sample count is
// the smallest the device supports at or above the one asked for.
void WebGLVkContext::renderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat,
                                                    GLsizei width, GLsizei height) {
    if (target != GL_RENDERBUFFER) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    VkFormat format = colorRenderableFormat(internalformat);
    if (format == VK_FORMAT_UNDEFINED) format = depthStencilFormat(context_.physicalDevice(), internalformat);
    if (format == VK_FORMAT_UNDEFINED || internalformat == GL_DEPTH_STENCIL) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    const GLint maxSize = getParameterInt(GL_MAX_RENDERBUFFER_SIZE);
    if (samples < 0 || width < 0 || height < 0 || width > maxSize || height > maxSize) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (currentRenderbufferId_ == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    VkSampleCountFlagBits sampleBits = VK_SAMPLE_COUNT_1_BIT;
    if (samples > 0) {
        const std::vector<GLint> counts = supportedSampleCounts(internalformat);
        if (counts.empty() || samples > counts.front()) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        GLint chosen = counts.front();
        for (GLint n : counts)
            if (n >= samples) chosen = n;
        sampleBits = static_cast<VkSampleCountFlagBits>(chosen);
    }

    VkRenderbufferResource& rb = renderbuffers_[currentRenderbufferId_];
    // A pass may have it attached; the old image is destroyed once the GPU is done.
    VkCommandBuffer cmd = transferCommands();
    releaseTexture(rb.storage);
    rb.storage = VkTextureResource{};
    rb.internalformat = internalformat;
    if (width == 0 || height == 0) return;

    const VkImageAspectFlags aspects = render::imageAspectFor(format);
    const bool color = aspects == VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageUsageFlags usage = (color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                           : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkTextureResource& tex = rb.storage;
    if (!context_.createImage(static_cast<uint32_t>(width), static_cast<uint32_t>(height), format,
                              VK_IMAGE_TILING_OPTIMAL, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, tex.image,
                              tex.memory, tex.offset, tex.allocId, 1, 1, 0, sampleBits)) {
        LOG_ERROR("WebGLVkContext: failed to allocate a %dx%d renderbuffer", width, height);
        tex = VkTextureResource{};
        setSyntheticError(GL_OUT_OF_MEMORY);
        return;
    }
    tex.width = static_cast<uint32_t>(width);
    tex.height = static_cast<uint32_t>(height);
    tex.format = format;
    tex.samples = sampleBits;
    tex.target = GL_RENDERBUFFER;
    tex.mipLevels = tex.arrayLayers = 1;
    tex.layouts.assign(1, VK_IMAGE_LAYOUT_UNDEFINED);
    tex.tf.internalformat = internalformat;
    tex.tf.format = format;
    if (aspects == VK_IMAGE_ASPECT_COLOR_BIT)
        tex.tf.kind = isSignedIntegerFormat(format) ? TexKind::Int
                      : isIntegerFormat(format)     ? TexKind::Uint
                                                    : TexKind::Float;
    else if (aspects == VK_IMAGE_ASPECT_STENCIL_BIT) tex.tf.kind = TexKind::Stencil;
    else tex.tf.kind = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? TexKind::DepthStencil : TexKind::Depth;
    // RGB formats are stored as RGBA; their alpha reads as one.
    tex.tf.alphaOne = internalformat == GL_RGB8 || internalformat == GL_RGB565;

    const VkImageSubresourceRange range{aspects, 0, 1, 0, 1};
    transitionTexture(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (color) {
        VkClearColorValue clear{};
        if (tex.tf.alphaOne) clear.float32[3] = 1.0f;
        vkCmdClearColorImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    } else {
        const VkClearDepthStencilValue clear{1.0f, 0};
        vkCmdClearDepthStencilImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }
    transitionTexture(cmd, tex, color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                      : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

GLint WebGLVkContext::getRenderbufferParameter(GLenum target, GLenum pname) {
    if (target != GL_RENDERBUFFER) {
        setSyntheticError(GL_INVALID_ENUM);
        return 0;
    }
    auto it = currentRenderbufferId_ != 0 ? renderbuffers_.find(currentRenderbufferId_) : renderbuffers_.end();
    if (it == renderbuffers_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return 0;
    }
    const VkRenderbufferResource& rb = it->second;
    FormatBits bits = formatBits(rb.storage.isValid() ? rb.storage.format : VK_FORMAT_UNDEFINED);
    if (rb.storage.tf.alphaOne) bits.alpha = 0;
    switch (pname) {
        case kRenderbufferWidth: return static_cast<GLint>(rb.storage.width);
        case kRenderbufferHeight: return static_cast<GLint>(rb.storage.height);
        case kRenderbufferInternalFormat: return rb.internalformat != 0 ? static_cast<GLint>(rb.internalformat)
                                                                       : static_cast<GLint>(GL_RGBA4);
        case kRenderbufferSamples:
            return rb.storage.isValid() && rb.storage.samples > VK_SAMPLE_COUNT_1_BIT
                       ? static_cast<GLint>(rb.storage.samples) : 0;
        default: break;
    }
    if (pname >= kRenderbufferRedSize && pname <= kRenderbufferStencilSize) {
        const int sizes[] = {bits.red, bits.green, bits.blue, bits.alpha, bits.depth, bits.stencil};
        return sizes[pname - kRenderbufferRedSize];
    }
    setSyntheticError(GL_INVALID_ENUM);
    return 0;
}

} // namespace bro::webgl::vk
