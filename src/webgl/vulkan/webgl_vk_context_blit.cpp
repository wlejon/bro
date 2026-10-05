// blitFramebuffer: GL ES 3.0 validation, then per buffer a copy (exact,
// any format) or a vkCmdBlitImage (scaling, filtering, format conversion),
// with the canvas's top-down rows and GL's bottom-up ones reconciled.
// A multisampled read buffer is resolved into a scratch image first: color
// with vkCmdResolveImage, depth/stencil with a dynamic-rendering resolve.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_formats.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace bro::webgl::vk {

namespace {

constexpr GLbitfield kBlitBits = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;

// A blit rectangle in GL coordinates; corners may be in either order.
struct BlitRect {
    int32_t sx0, sy0, sx1, sy1;
    int32_t dx0, dy0, dx1, dy1;
};

// Clip one axis of a blit: the destination keeps the pixels inside
// [dlo, dhi) whose centers map inside the source's [0, slimit). False
// when nothing remains. The source edges are rounded to whole texels.
bool clipAxis(int32_t& s0, int32_t& s1, int32_t& d0, int32_t& d1, int32_t slimit, int32_t dlo, int32_t dhi) {
    if (s0 == s1 || d0 == d1) return false;
    const double scale = static_cast<double>(s1 - s0) / (d1 - d0);
    auto srcAt = [&](double t) { return s0 + (t - d0) * scale; };
    auto dstAt = [&](double s) { return d0 + (s - s0) / scale; };
    const double tA = dstAt(0.0), tB = dstAt(slimit);
    const double tmin = std::min(tA, tB), tmax = std::max(tA, tB);
    int32_t lo = std::max({std::min(d0, d1), dlo, static_cast<int32_t>(std::ceil(tmin - 0.5))});
    int32_t hi = std::min({std::max(d0, d1), dhi, static_cast<int32_t>(std::ceil(tmax - 0.5))});
    if (hi <= lo) return false;
    const int32_t nd0 = d0 < d1 ? lo : hi, nd1 = d0 < d1 ? hi : lo;
    const auto ns0 = static_cast<int32_t>(std::lround(srcAt(nd0)));
    const auto ns1 = static_cast<int32_t>(std::lround(srcAt(nd1)));
    if (ns0 == ns1) return false;
    s0 = std::clamp(ns0, 0, slimit);
    s1 = std::clamp(ns1, 0, slimit);
    d0 = nd0;
    d1 = nd1;
    return s0 != s1;
}

bool formatHas(VkPhysicalDevice device, VkFormat format, VkFormatFeatureFlags features) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(device, format, &props);
    return (props.optimalTilingFeatures & features) == features;
}

} // namespace

bool WebGLVkContext::scratchImage(VkFormat format, uint32_t width, uint32_t height, VkTextureResource& out) {
    out = VkTextureResource{};
    const bool color = render::imageAspectFor(format) == VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    (color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                           : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    if (!context_.createImage(width, height, format, VK_IMAGE_TILING_OPTIMAL, usage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.image, out.memory, out.offset, out.allocId)) {
        LOG_ERROR("WebGLVkContext: failed to allocate a %ux%u resolve image", width, height);
        out = VkTextureResource{};
        setSyntheticError(GL_OUT_OF_MEMORY);
        return false;
    }
    out.width = width;
    out.height = height;
    out.format = format;
    out.target = GL_RENDERBUFFER;
    out.mipLevels = out.arrayLayers = 1;
    out.layouts.assign(1, VK_IMAGE_LAYOUT_UNDEFINED);
    return true;
}

void WebGLVkContext::releaseScratch(VkTextureResource& tex) {
    releaseTexture(tex);
}

void WebGLVkContext::blitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                     GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                     GLbitfield mask, GLenum filter) {
    if (mask & ~kBlitBits) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (filter != GL_NEAREST && filter != GL_LINEAR) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) && filter == GL_LINEAR) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    RenderTarget dst;
    Surface srcColor;
    if (!readColorSurface(srcColor) || !drawTarget(dst)) return;
    // Blitting a level of an image onto itself is an error; onto another
    // level (or layer, or 3D slice) of the same image it is a copy like any other.
    auto sameSubresource = [](const Surface& a, const Surface& b) {
        return a.image == b.image && a.level == b.level && a.layer == b.layer && a.z == b.z;
    };

    // The read framebuffer's depth and stencil, and its sample count.
    Surface srcDepth, srcStencil;
    VkSampleCountFlagBits srcSamples = VK_SAMPLE_COUNT_1_BIT;
    if (readFboId_ == 0) {
        const Surface ds = canvasSurface(true);
        if (ds.aspects() & VK_IMAGE_ASPECT_DEPTH_BIT) srcDepth = ds;
        if (ds.aspects() & VK_IMAGE_ASPECT_STENCIL_BIT) srcStencil = ds;
    } else {
        const VkFramebufferResource& fbo = framebuffers_[readFboId_];
        srcDepth = attachmentSurface(fbo.depth);
        srcStencil = attachmentSurface(fbo.stencil);
        for (const VkFboAttachment& att : fbo.color)
            if (Surface s = attachmentSurface(att)) srcSamples = s.samples;
        if (srcDepth) srcSamples = srcDepth.samples;
        if (srcStencil) srcSamples = srcStencil.samples;
    }

    // A buffer missing on either side is not blitted.
    if (!srcColor || dst.colorCount == 0) mask &= ~GL_COLOR_BUFFER_BIT;
    if (!srcDepth || !dst.depth) mask &= ~GL_DEPTH_BUFFER_BIT;
    if (!srcStencil || !dst.stencil) mask &= ~GL_STENCIL_BUFFER_BIT;

    if (dst.samples > VK_SAMPLE_COUNT_1_BIT) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (srcSamples > VK_SAMPLE_COUNT_1_BIT && (srcX1 - srcX0 != dstX1 - dstX0 || srcY1 - srcY0 != dstY1 - dstY0)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (mask & GL_COLOR_BUFFER_BIT) {
        const bool srcInt = isIntegerFormat(srcColor.format);
        if (srcInt && filter == GL_LINEAR) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        for (uint32_t i = 0; i < dst.colorCount; ++i) {
            const Surface& d = dst.color[i];
            if (!d) continue;
            if (isIntegerFormat(d.format) != srcInt ||
                isSignedIntegerFormat(d.format) != isSignedIntegerFormat(srcColor.format) ||
                (srcSamples > VK_SAMPLE_COUNT_1_BIT && d.format != srcColor.format) ||
                sameSubresource(d, srcColor)) {
                setSyntheticError(GL_INVALID_OPERATION);
                return;
            }
        }
    }
    if (((mask & GL_DEPTH_BUFFER_BIT) && (srcDepth.format != dst.depth.format || sameSubresource(srcDepth, dst.depth))) ||
        ((mask & GL_STENCIL_BUFFER_BIT) &&
         (srcStencil.format != dst.stencil.format || sameSubresource(srcStencil, dst.stencil)))) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (mask == 0) return;

    // Clip to both framebuffers and the scissor box.
    BlitRect r{srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1};
    int32_t clipX0 = 0, clipY0 = 0;
    int32_t clipX1 = static_cast<int32_t>(dst.extent.width), clipY1 = static_cast<int32_t>(dst.extent.height);
    if (scissorTest_) {
        clipX0 = std::max(clipX0, scissor_.offset.x);
        clipY0 = std::max(clipY0, scissor_.offset.y);
        clipX1 = std::min(clipX1, scissor_.offset.x + static_cast<int32_t>(scissor_.extent.width));
        clipY1 = std::min(clipY1, scissor_.offset.y + static_cast<int32_t>(scissor_.extent.height));
    }
    // The read framebuffer's size: its smallest attachment.
    uint32_t srcW = UINT32_MAX, srcH = UINT32_MAX;
    for (const Surface* s : {&srcColor, &srcDepth, &srcStencil}) {
        if (!*s) continue;
        srcW = std::min(srcW, s->width);
        srcH = std::min(srcH, s->height);
    }
    if (!clipAxis(r.sx0, r.sx1, r.dx0, r.dx1, static_cast<int32_t>(srcW), clipX0, clipX1) ||
        !clipAxis(r.sy0, r.sy1, r.dy0, r.dy1, static_cast<int32_t>(srcH), clipY0, clipY1))
        return;

    VkCommandBuffer cmd = transferCommands();
    std::vector<VkTextureResource> scratch;
    scratch.reserve(3);  // color, depth, stencil: references stay valid

    // A multisampled source becomes a single-sample scratch copy of itself.
    auto resolved = [&](const Surface& s) -> Surface {
        if (s.samples == VK_SAMPLE_COUNT_1_BIT) return s;
        scratch.emplace_back();
        VkTextureResource& tex = scratch.back();
        if (!scratchImage(s.format, s.width, s.height, tex)) return Surface{};
        Surface out = s;
        out.source = Surface::Source::Renderbuffer;
        out.tex = &tex;
        out.image = tex.image;
        out.level = out.layer = 0;
        out.z = 0;
        out.samples = VK_SAMPLE_COUNT_1_BIT;
        if (s.aspects() == VK_IMAGE_ASPECT_COLOR_BIT) {
            transitionSurface(cmd, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            transitionTexture(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageResolve region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, s.level, s.layer, 1};
            region.srcOffset = {std::min(r.sx0, r.sx1), std::min(r.sy0, r.sy1), 0};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstOffset = region.srcOffset;
            region.extent = {static_cast<uint32_t>(std::abs(r.sx1 - r.sx0)),
                             static_cast<uint32_t>(std::abs(r.sy1 - r.sy0)), 1};
            vkCmdResolveImage(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex.image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        } else {
            // Vulkan resolves depth/stencil only at the end of a pass:
            // sample zero of each pixel, which ES 3.0 allows.
            out.view = attachmentView(tex, 0, 0);
            transitionSurface(cmd, s, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            transitionTexture(cmd, tex, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo att{};
            att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            att.imageView = s.view;
            att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            att.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            att.resolveImageView = out.view;
            att.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            info.renderArea.extent = {s.width, s.height};
            info.layerCount = 1;
            info.pDepthAttachment = (s.aspects() & VK_IMAGE_ASPECT_DEPTH_BIT) ? &att : nullptr;
            info.pStencilAttachment = (s.aspects() & VK_IMAGE_ASPECT_STENCIL_BIT) ? &att : nullptr;
            vkCmdBeginRendering(cmd, &info);
            vkCmdEndRendering(cmd);
            render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }
        return out;
    };

    // Copy or blit `aspects` of `src` onto `d` over the clipped rectangle.
    auto blit = [&](const Surface& srcIn, const Surface& d, VkImageAspectFlags aspects) {
        const Surface s = resolved(srcIn);
        if (!s) return;
        auto imageY = [](const Surface& surf, int32_t y) {
            return surf.topDown() ? static_cast<int32_t>(surf.height) - y : y;
        };
        const int32_t sy0 = imageY(s, r.sy0), sy1 = imageY(s, r.sy1);
        const int32_t dy0 = imageY(d, r.dy0), dy1 = imageY(d, r.dy1);
        const bool unscaled = std::abs(r.sx1 - r.sx0) == std::abs(r.dx1 - r.dx0) &&
                              std::abs(sy1 - sy0) == std::abs(dy1 - dy0);
        const bool mirrorX = (r.sx1 > r.sx0) != (r.dx1 > r.dx0);
        const bool mirrorY = (sy1 > sy0) != (dy1 > dy0);
        const VkFormatFeatureFlags linear =
            filter == GL_LINEAR ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0;
        const bool canCopy = unscaled && !mirrorX && s.format == d.format;
        const bool canBlit = formatHas(context_.physicalDevice(), s.format, VK_FORMAT_FEATURE_BLIT_SRC_BIT | linear) &&
                             formatHas(context_.physicalDevice(), d.format, VK_FORMAT_FEATURE_BLIT_DST_BIT);
        if (!canCopy && !canBlit) {
            LOG_ERROR("WebGLVkContext: blitFramebuffer cannot %s format %d to %d on this device",
                      unscaled ? "mirror" : "scale", static_cast<int>(s.format), static_cast<int>(d.format));
            return;
        }
        transitionSurface(cmd, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transitionSurface(cmd, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageSubresourceLayers srcLayers{aspects, s.level, s.layer, 1};
        const VkImageSubresourceLayers dstLayers{aspects, d.level, d.layer, 1};
        // Unscaled, LINEAR samples texel centers exactly: a copy is the same.
        if (canCopy) {
            const uint32_t w = static_cast<uint32_t>(std::abs(r.sx1 - r.sx0));
            const uint32_t h = static_cast<uint32_t>(std::abs(sy1 - sy0));
            const int32_t sx = std::min(r.sx0, r.sx1), dx = std::min(r.dx0, r.dx1);
            const int32_t sy = std::min(sy0, sy1), dy = std::min(dy0, dy1);
            std::vector<VkImageCopy> regions;
            // Rows flip one region per row; otherwise one region.
            for (uint32_t row = 0; row < (mirrorY ? h : 1u); ++row) {
                VkImageCopy c{};
                c.srcSubresource = srcLayers;
                c.dstSubresource = dstLayers;
                c.srcOffset = {sx, sy + static_cast<int32_t>(row), s.z};
                c.dstOffset = {dx, mirrorY ? dy + static_cast<int32_t>(h - 1 - row) : dy, d.z};
                c.extent = {w, mirrorY ? 1u : h, 1};
                regions.push_back(c);
            }
            vkCmdCopyImage(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()),
                           regions.data());
        } else {
            VkImageBlit b{};
            b.srcSubresource = srcLayers;
            b.srcOffsets[0] = {r.sx0, sy0, s.z};
            b.srcOffsets[1] = {r.sx1, sy1, s.z + 1};
            b.dstSubresource = dstLayers;
            b.dstOffsets[0] = {r.dx0, dy0, d.z};
            b.dstOffsets[1] = {r.dx1, dy1, d.z + 1};
            vkCmdBlitImage(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                           filter == GL_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
        render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    };

    if (mask & GL_COLOR_BUFFER_BIT) {
        // Resolved once, however many draw buffers it lands in.
        const Surface src = resolved(srcColor);
        for (uint32_t i = 0; src && i < dst.colorCount; ++i)
            if (dst.color[i]) blit(src, dst.color[i], VK_IMAGE_ASPECT_COLOR_BIT);
    }
    const bool depth = mask & GL_DEPTH_BUFFER_BIT, stencil = mask & GL_STENCIL_BUFFER_BIT;
    if (depth && stencil && srcDepth.image == srcStencil.image && dst.depth.image == dst.stencil.image) {
        blit(srcDepth, dst.depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    } else {
        if (depth) blit(srcDepth, dst.depth, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (stencil) blit(srcStencil, dst.stencil, VK_IMAGE_ASPECT_STENCIL_BIT);
    }

    // Textures go back to sampleable; scratch images are released.
    for (const Surface* s : {&srcColor, &srcDepth, &srcStencil})
        if (s->source == Surface::Source::Texture) transitionSurface(cmd, *s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (uint32_t i = 0; i < dst.colorCount; ++i)
        if (dst.color[i].source == Surface::Source::Texture)
            transitionSurface(cmd, dst.color[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (const Surface* s : {&dst.depth, &dst.stencil})
        if (s->source == Surface::Source::Texture) transitionSurface(cmd, *s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (VkTextureResource& tex : scratch) releaseScratch(tex);
}

} // namespace bro::webgl::vk
