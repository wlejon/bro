// clear() and clearBuffer*: attachment clears inside the draw framebuffer's
// pass, bounded by the scissor box, typed by the buffer they clear.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_formats.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl::vk {

namespace {

constexpr GLbitfield kClearBits = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;

void bitsOf(const float f[4], uint32_t out[4]) { std::memcpy(out, f, 16); }

} // namespace

// How the color and stencil write masks let a clear through: not at all,
// wholly (an attachment clear), or in part (a masked draw).
enum class MaskReach { None, Whole, Part };

static MaskReach colorReach(const GLboolean mask[4]) {
    const int n = (mask[0] ? 1 : 0) + (mask[1] ? 1 : 0) + (mask[2] ? 1 : 0) + (mask[3] ? 1 : 0);
    return n == 0 ? MaskReach::None : n == 4 ? MaskReach::Whole : MaskReach::Part;
}

static MaskReach stencilReach(GLuint writeMask) {
    const GLuint m = writeMask & 0xFF;
    return m == 0 ? MaskReach::None : m == 0xFF ? MaskReach::Whole : MaskReach::Part;
}

// Record `count` clears over the pass's render area, cut to the scissor box
// when the scissor test is on. The pass is open.
void WebGLVkContext::clearAttachments(const VkClearAttachment* atts, uint32_t count) {
    if (count == 0) return;
    const VkExtent2D extent = pass_.extent;
    int32_t x0 = 0, y0 = 0;
    int32_t x1 = static_cast<int32_t>(extent.width), y1 = static_cast<int32_t>(extent.height);
    if (scissorTest_) {
        x0 = std::max(x0, scissor_.offset.x);
        x1 = std::min(x1, scissor_.offset.x + static_cast<int32_t>(scissor_.extent.width));
        const int32_t top = scissorTop(extent);
        y0 = std::max(y0, top);
        y1 = std::min(y1, top + static_cast<int32_t>(scissor_.extent.height));
    }
    if (x1 <= x0 || y1 <= y0) return;
    VkClearRect rect{};
    rect.rect.offset = {x0, y0};
    rect.rect.extent = {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
    rect.layerCount = 1;
    vkCmdClearAttachments(commands(), count, atts, 1, &rect);
}

// The depth and/or stencil clear `mask` asks for, as the write masks allow:
// a buffer GL would not write is left alone.
static VkClearAttachment depthStencilClear(bool depth, bool stencil, float depthValue, int32_t stencilValue) {
    VkClearAttachment att{};
    if (depth) att.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (stencil) att.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    att.clearValue.depthStencil = {std::clamp(depthValue, 0.0f, 1.0f), static_cast<uint32_t>(stencilValue)};
    return att;
}

void WebGLVkContext::clear(GLbitfield mask) {
    if (mask & ~kClearBits) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    // RASTERIZER_DISCARD discards clears too (ES 3.0 4.2.3).
    if (rasterizerDiscardEnabled_) return;
    beginRendering();
    if (!inRenderPass_) return;

    const MaskReach color = colorReach(colorMask_);
    const MaskReach stencilMask = stencilReach(stencilWriteMaskFront_);
    uint32_t maskedColor = 0;
    VkClearAttachment atts[9]{};
    uint32_t count = 0;
    if (mask & GL_COLOR_BUFFER_BIT) {
        // WebGL 2: clear() of an integer color buffer is INVALID_OPERATION
        // (clearBufferiv/uiv clear those).
        for (uint32_t i = 0; i < pass_.colorCount; ++i) {
            if (pass_.color[i] && isIntegerFormat(pass_.color[i].format)) {
                setSyntheticError(GL_INVALID_OPERATION);
                return;
            }
        }
        for (uint32_t i = 0; color != MaskReach::None && i < pass_.colorCount; ++i) {
            if (!pass_.color[i]) continue;
            if (color == MaskReach::Part) {
                maskedColor |= 1u << i;
                continue;
            }
            atts[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            atts[count].colorAttachment = i;
            atts[count].clearValue.color = {{clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]}};
            ++count;
        }
    }
    const bool depth = (mask & GL_DEPTH_BUFFER_BIT) && pass_.depth && depthMask_;
    const bool stencil = (mask & GL_STENCIL_BUFFER_BIT) && pass_.stencil && stencilMask != MaskReach::None;
    const bool wholeStencil = stencil && stencilMask == MaskReach::Whole;
    if (depth || wholeStencil) atts[count++] = depthStencilClear(depth, wholeStencil, clearDepth_, clearStencil_);
    clearAttachments(atts, count);
    uint32_t bits[4];
    bitsOf(clearColor_, bits);
    clearMasked(maskedColor, bits, stencil && !wholeStencil, clearStencil_);
}

// The pass's color attachment for clearBuffer's `drawbuffer`, after the
// checks every clearBuffer* makes: an empty surface means no-op.
static bool clearBufferIndex(GLint drawbuffer, GLint maxDrawBuffers, GLenum& error) {
    if (drawbuffer < 0 || drawbuffer >= maxDrawBuffers) {
        error = GL_INVALID_VALUE;
        return false;
    }
    return true;
}

void WebGLVkContext::clearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* values) {
    if (buffer != GL_COLOR && buffer != GL_DEPTH) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    GLenum error = GL_NO_ERROR;
    if (!clearBufferIndex(drawbuffer, buffer == GL_COLOR ? getParameterInt(GL_MAX_DRAW_BUFFERS) : 1, error)) {
        setSyntheticError(error);
        return;
    }
    if (rasterizerDiscardEnabled_) return;
    beginRendering();
    if (!inRenderPass_) return;
    VkClearAttachment att{};
    uint32_t count = 0;
    if (buffer == GL_DEPTH) {
        att = depthStencilClear(pass_.depth && depthMask_, false, values[0], 0);
        count = att.aspectMask ? 1 : 0;
    } else if (static_cast<uint32_t>(drawbuffer) < pass_.colorCount && pass_.color[drawbuffer]) {
        if (isIntegerFormat(pass_.color[drawbuffer].format)) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        const MaskReach reach = colorReach(colorMask_);
        if (reach == MaskReach::None) return;
        if (reach == MaskReach::Part) {
            uint32_t bits[4];
            bitsOf(values, bits);
            clearMasked(1u << drawbuffer, bits, false, 0);
            return;
        }
        att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att.colorAttachment = static_cast<uint32_t>(drawbuffer);
        att.clearValue.color = {{values[0], values[1], values[2], values[3]}};
        count = 1;
    }
    clearAttachments(&att, count);
}

void WebGLVkContext::clearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* values) {
    if (buffer != GL_COLOR && buffer != GL_STENCIL) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    GLenum error = GL_NO_ERROR;
    if (!clearBufferIndex(drawbuffer, buffer == GL_COLOR ? getParameterInt(GL_MAX_DRAW_BUFFERS) : 1, error)) {
        setSyntheticError(error);
        return;
    }
    if (rasterizerDiscardEnabled_) return;
    beginRendering();
    if (!inRenderPass_) return;
    VkClearAttachment att{};
    uint32_t count = 0;
    if (buffer == GL_STENCIL) {
        const MaskReach reach = stencilReach(stencilWriteMaskFront_);
        if (!pass_.stencil || reach == MaskReach::None) return;
        if (reach == MaskReach::Part) {
            const uint32_t none[4]{};
            clearMasked(0, none, true, values[0]);
            return;
        }
        att = depthStencilClear(false, true, 0.0f, values[0]);
        count = 1;
    } else if (static_cast<uint32_t>(drawbuffer) < pass_.colorCount && pass_.color[drawbuffer]) {
        if (!isSignedIntegerFormat(pass_.color[drawbuffer].format)) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        const MaskReach reach = colorReach(colorMask_);
        if (reach == MaskReach::None) return;
        if (reach == MaskReach::Part) {
            uint32_t bits[4];
            std::memcpy(bits, values, 16);
            clearMasked(1u << drawbuffer, bits, false, 0);
            return;
        }
        att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att.colorAttachment = static_cast<uint32_t>(drawbuffer);
        att.clearValue.color.int32[0] = values[0];
        att.clearValue.color.int32[1] = values[1];
        att.clearValue.color.int32[2] = values[2];
        att.clearValue.color.int32[3] = values[3];
        count = 1;
    }
    clearAttachments(&att, count);
}

void WebGLVkContext::clearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* values) {
    if (buffer != GL_COLOR) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    GLenum error = GL_NO_ERROR;
    if (!clearBufferIndex(drawbuffer, getParameterInt(GL_MAX_DRAW_BUFFERS), error)) {
        setSyntheticError(error);
        return;
    }
    if (rasterizerDiscardEnabled_) return;
    beginRendering();
    if (!inRenderPass_) return;
    if (static_cast<uint32_t>(drawbuffer) >= pass_.colorCount || !pass_.color[drawbuffer]) return;
    const VkFormat format = pass_.color[drawbuffer].format;
    if (!isIntegerFormat(format) || isSignedIntegerFormat(format)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const MaskReach reach = colorReach(colorMask_);
    if (reach == MaskReach::None) return;
    if (reach == MaskReach::Part) {
        clearMasked(1u << drawbuffer, values, false, 0);
        return;
    }
    VkClearAttachment att{};
    att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    att.colorAttachment = static_cast<uint32_t>(drawbuffer);
    for (int i = 0; i < 4; ++i) att.clearValue.color.uint32[i] = values[i];
    clearAttachments(&att, 1);
}

void WebGLVkContext::clearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
    if (buffer != GL_DEPTH_STENCIL) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if (drawbuffer != 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (rasterizerDiscardEnabled_) return;
    beginRendering();
    if (!inRenderPass_) return;
    const MaskReach reach = stencilReach(stencilWriteMaskFront_);
    const bool clearsStencil = pass_.stencil && reach != MaskReach::None;
    const VkClearAttachment att = depthStencilClear(pass_.depth && depthMask_,
                                                    clearsStencil && reach == MaskReach::Whole, depth, stencil);
    clearAttachments(&att, att.aspectMask ? 1 : 0);
    const uint32_t none[4]{};
    if (clearsStencil && reach == MaskReach::Part) clearMasked(0, none, true, stencil);
}

} // namespace bro::webgl::vk
