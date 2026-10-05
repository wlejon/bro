// Framebuffer and renderbuffer objects: bindings (separate DRAW and READ),
// attachments, completeness, draw/read buffer selection, renderbuffer
// storage (multisampled when asked), and resolving what a pass renders into.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_formats.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>

namespace bro::webgl::vk {

namespace {

constexpr GLenum kTextureCubeMap = 0x8513;
constexpr GLenum kCubeFacePosX = 0x8515;
constexpr GLenum kCubeFaceNegZ = 0x851A;

// FRAMEBUFFER_ATTACHMENT_* parameter names.
constexpr GLenum kAttachmentObjectType = 0x8CD0;
constexpr GLenum kAttachmentObjectName = 0x8CD1;
constexpr GLenum kAttachmentTextureLevel = 0x8CD2;
constexpr GLenum kAttachmentTextureCubeMapFace = 0x8CD3;
constexpr GLenum kAttachmentTextureLayer = 0x8CD4;
constexpr GLenum kAttachmentColorEncoding = 0x8210;
constexpr GLenum kAttachmentComponentType = 0x8211;
constexpr GLenum kAttachmentRedSize = 0x8212;
constexpr GLenum kAttachmentStencilSize = 0x8217;
constexpr GLenum kTexture = 0x1702;
constexpr GLenum kLinear = 0x2601;
constexpr GLenum kSrgb = 0x8C40;
constexpr GLenum kUnsignedNormalized = 0x8C17;
constexpr GLenum kFloat = 0x1406;

// RENDERBUFFER_* parameter names.
constexpr GLenum kRenderbufferWidth = 0x8D42;
constexpr GLenum kRenderbufferHeight = 0x8D43;
constexpr GLenum kRenderbufferInternalFormat = 0x8D44;
constexpr GLenum kRenderbufferRedSize = 0x8D50;
constexpr GLenum kRenderbufferStencilSize = 0x8D55;
constexpr GLenum kRenderbufferSamples = 0x8CAB;

constexpr uint32_t kMaxColorAttachments = 8;

bool isColorAttachment(GLenum attachment) {
    return attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + kMaxColorAttachments;
}

bool isFramebufferTarget(GLenum target) {
    return target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER;
}

VkImageLayout attachmentLayout(VkFormat format) {
    return render::imageAspectFor(format) == VK_IMAGE_ASPECT_COLOR_BIT
               ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
               : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}

} // namespace

VkImageAspectFlags WebGLVkContext::Surface::aspects() const {
    return render::imageAspectFor(format);
}

// ---------------------------------------------------------------------------
// Framebuffer objects
// ---------------------------------------------------------------------------

WebGLFramebuffer WebGLVkContext::createFramebuffer() {
    GLuint id = nextFboId_++;
    framebuffers_[id] = VkFramebufferResource{};
    return {id};
}

void WebGLVkContext::deleteFramebuffer(WebGLFramebuffer fb) {
    if (fb.id == 0 || framebuffers_.find(fb.id) == framebuffers_.end()) return;
    // Deleting a bound framebuffer binds the canvas in its place.
    framebufferChanged(fb.id);
    if (drawFboId_ == fb.id) drawFboId_ = 0;
    if (readFboId_ == fb.id) readFboId_ = 0;
    framebuffers_.erase(fb.id);
}

void WebGLVkContext::bindFramebuffer(GLenum target, WebGLFramebuffer fb) {
    if (!isFramebufferTarget(target)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if (fb.id != 0 && framebuffers_.find(fb.id) == framebuffers_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);  // a deleted framebuffer
        return;
    }
    if (target != GL_READ_FRAMEBUFFER && drawFboId_ != fb.id) {
        endRendering();
        drawFboId_ = fb.id;
    }
    if (target != GL_DRAW_FRAMEBUFFER) readFboId_ = fb.id;
}

// The framebuffer object bound to `target` (and its id, 0 for the canvas),
// or null for the canvas or an invalid target (INVALID_ENUM).
VkFramebufferResource* WebGLVkContext::framebufferForTarget(GLenum target, GLuint& id) {
    if (!isFramebufferTarget(target)) {
        setSyntheticError(GL_INVALID_ENUM);
        id = 0;
        return nullptr;
    }
    id = target == GL_READ_FRAMEBUFFER ? readFboId_ : drawFboId_;
    auto it = id != 0 ? framebuffers_.find(id) : framebuffers_.end();
    return it != framebuffers_.end() ? &it->second : nullptr;
}

void WebGLVkContext::framebufferChanged(GLuint id) {
    if (id != 0 && id == drawFboId_) endRendering();
}

void WebGLVkContext::setAttachment(GLenum target, GLenum attachment, const VkFboAttachment& att) {
    GLuint id = 0;
    VkFramebufferResource* fbo = framebufferForTarget(target, id);
    if (!isFramebufferTarget(target)) return;
    if (!fbo) {
        setSyntheticError(GL_INVALID_OPERATION);  // the canvas's attachments are fixed
        return;
    }
    if (isColorAttachment(attachment)) {
        if (fbo->color[attachment - GL_COLOR_ATTACHMENT0] == att) return;
        framebufferChanged(id);
        fbo->color[attachment - GL_COLOR_ATTACHMENT0] = att;
    } else if (attachment == GL_DEPTH_ATTACHMENT) {
        framebufferChanged(id);
        fbo->depth = att;
    } else if (attachment == GL_STENCIL_ATTACHMENT) {
        framebufferChanged(id);
        fbo->stencil = att;
    } else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        framebufferChanged(id);
        fbo->depth = att;
        fbo->stencil = att;
    } else {
        setSyntheticError(attachment > GL_COLOR_ATTACHMENT0 && attachment <= 0x8CEF ? GL_INVALID_OPERATION
                                                                                     : GL_INVALID_ENUM);
    }
}

void WebGLVkContext::framebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                                          WebGLTexture tex, GLint level) {
    VkFboAttachment att;
    if (tex.id != 0) {
        auto it = textures_.find(tex.id);
        if (it == textures_.end()) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        const bool cubeFace = textarget >= kCubeFacePosX && textarget <= kCubeFaceNegZ;
        if (textarget != GL_TEXTURE_2D && !cubeFace) {
            setSyntheticError(GL_INVALID_ENUM);
            return;
        }
        const GLenum texTarget = it->second.target;
        if (it->second.isValid() && (texTarget == kTextureCubeMap) != cubeFace) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        if (level < 0 || level > 15) {
            setSyntheticError(GL_INVALID_VALUE);
            return;
        }
        att = {VkFboAttachment::Kind::Texture, tex.id, static_cast<uint32_t>(level),
               cubeFace ? textarget - kCubeFacePosX : 0u};
    }
    setAttachment(target, attachment, att);
}

void WebGLVkContext::framebufferTextureLayer(GLenum target, GLenum attachment, WebGLTexture tex, GLint level,
                                             GLint layer) {
    VkFboAttachment att;
    if (tex.id != 0) {
        auto it = textures_.find(tex.id);
        if (it == textures_.end()) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        if (it->second.target != GL_TEXTURE_2D_ARRAY && it->second.target != GL_TEXTURE_3D) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        if (level < 0 || layer < 0 || level > 15) {
            setSyntheticError(GL_INVALID_VALUE);
            return;
        }
        att = {VkFboAttachment::Kind::Texture, tex.id, static_cast<uint32_t>(level), static_cast<uint32_t>(layer)};
    }
    setAttachment(target, attachment, att);
}

void WebGLVkContext::framebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                                             WebGLRenderbuffer rbo) {
    if (renderbuffertarget != GL_RENDERBUFFER) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    VkFboAttachment att;
    if (rbo.id != 0) {
        if (renderbuffers_.find(rbo.id) == renderbuffers_.end()) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        att = {VkFboAttachment::Kind::Renderbuffer, rbo.id, 0, 0};
    }
    setAttachment(target, attachment, att);
}

// What an attachment renders into, or an empty surface when it names
// nothing usable (a deleted object, storage not yet defined, a level or
// layer the storage does not have).
WebGLVkContext::Surface WebGLVkContext::attachmentSurface(const VkFboAttachment& att) {
    Surface s;
    VkTextureResource* tex = nullptr;
    if (att.kind == VkFboAttachment::Kind::Texture) {
        auto it = textures_.find(att.id);
        if (it != textures_.end()) tex = &it->second;
    } else if (att.kind == VkFboAttachment::Kind::Renderbuffer) {
        auto it = renderbuffers_.find(att.id);
        if (it != renderbuffers_.end()) tex = &it->second.storage;
    }
    if (!tex || !tex->isValid() || att.level >= tex->mipLevels || att.layer >= tex->arrayLayers) return s;
    VkImageView view = attachmentView(*tex, att.level, att.layer);
    if (view == VK_NULL_HANDLE) return s;
    s.source = att.kind == VkFboAttachment::Kind::Texture ? Surface::Source::Texture : Surface::Source::Renderbuffer;
    s.tex = tex;
    s.image = tex->image;
    s.view = view;
    s.format = tex->format;
    s.width = std::max(1u, tex->width >> att.level);
    s.height = std::max(1u, tex->height >> att.level);
    s.level = att.level;
    s.layer = att.layer;
    s.samples = tex->samples;
    return s;
}

WebGLVkContext::Surface WebGLVkContext::canvasSurface(bool depth) {
    Surface s;
    if (!canvas_.isValid()) return s;
    s.source = depth ? Surface::Source::CanvasDepth : Surface::Source::CanvasColor;
    s.image = depth ? canvas_.depthImage() : canvas_.colorImage();
    s.view = depth ? canvas_.depthView() : canvas_.colorView();
    s.format = depth ? canvas_.depthFormat() : canvas_.colorFormat();
    s.width = canvas_.width();
    s.height = canvas_.height();
    if (s.image == VK_NULL_HANDLE) s.source = Surface::Source::None;
    return s;
}

// A single-level, single-layer view of `tex` with every aspect of its
// format, made on first use and destroyed with the texture's storage.
VkImageView WebGLVkContext::attachmentView(VkTextureResource& tex, uint32_t level, uint32_t layer) {
    const uint32_t key = level << 16 | layer;
    for (const auto& [k, view] : tex.attachmentViews)
        if (k == key) return view;
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = tex.image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = tex.format;
    info.subresourceRange = {render::imageAspectFor(tex.format), level, 1, layer, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(context_.device(), &info, nullptr, &view) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkContext: failed to create a framebuffer attachment view");
        return VK_NULL_HANDLE;
    }
    tex.attachmentViews.emplace_back(key, view);
    return view;
}

VkImageLayout WebGLVkContext::surfaceLayout(const Surface& s) const {
    switch (s.source) {
        case Surface::Source::Texture:
        case Surface::Source::Renderbuffer: return s.tex->currentLayout;
        case Surface::Source::CanvasColor: return canvas_.colorLayout();
        case Surface::Source::CanvasDepth: return canvas_.depthLayout();
        default: return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

void WebGLVkContext::transitionSurface(VkCommandBuffer cmd, const Surface& s, VkImageLayout layout) {
    switch (s.source) {
        case Surface::Source::Texture:
        case Surface::Source::Renderbuffer: transitionTexture(cmd, *s.tex, layout); break;
        case Surface::Source::CanvasColor: canvas_.transitionColor(cmd, layout); break;
        case Surface::Source::CanvasDepth: canvas_.transitionDepth(cmd, layout); break;
        default: break;
    }
}

// Completeness as WebGL 2 defines it: every attachment names defined
// storage of a format its attachment point can render; something is
// attached; one sample count throughout; and depth and stencil, when both
// attached, are the same image (WebGL's FRAMEBUFFER_UNSUPPORTED otherwise).
GLenum WebGLVkContext::framebufferStatus(GLuint id) {
    if (id == 0) return canvas_.isValid() ? GL_FRAMEBUFFER_COMPLETE : GL_FRAMEBUFFER_UNSUPPORTED;
    auto it = framebuffers_.find(id);
    if (it == framebuffers_.end()) return GL_FRAMEBUFFER_UNSUPPORTED;
    const VkFramebufferResource& fbo = it->second;

    bool any = false;
    int samples = -1;
    auto check = [&](const VkFboAttachment& att, VkImageAspectFlags needs) -> GLenum {
        if (att.kind == VkFboAttachment::Kind::None) return GL_FRAMEBUFFER_COMPLETE;
        const Surface s = attachmentSurface(att);
        if (!s || (s.aspects() & needs) == 0) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (needs == VK_IMAGE_ASPECT_COLOR_BIT) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), s.format, &props);
            if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
                return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        }
        if (samples >= 0 && samples != static_cast<int>(s.samples)) return GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE;
        samples = static_cast<int>(s.samples);
        any = true;
        return GL_FRAMEBUFFER_COMPLETE;
    };
    for (const VkFboAttachment& att : fbo.color)
        if (GLenum status = check(att, VK_IMAGE_ASPECT_COLOR_BIT); status != GL_FRAMEBUFFER_COMPLETE) return status;
    if (GLenum status = check(fbo.depth, VK_IMAGE_ASPECT_DEPTH_BIT); status != GL_FRAMEBUFFER_COMPLETE)
        return status;
    if (GLenum status = check(fbo.stencil, VK_IMAGE_ASPECT_STENCIL_BIT); status != GL_FRAMEBUFFER_COMPLETE)
        return status;
    if (!any) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    if (fbo.depth.kind != VkFboAttachment::Kind::None && fbo.stencil.kind != VkFboAttachment::Kind::None &&
        !(fbo.depth == fbo.stencil))
        return GL_FRAMEBUFFER_UNSUPPORTED;
    return GL_FRAMEBUFFER_COMPLETE;
}

GLenum WebGLVkContext::checkFramebufferStatus(GLenum target) {
    GLuint id = 0;
    framebufferForTarget(target, id);
    if (!isFramebufferTarget(target)) return 0;
    return framebufferStatus(id);
}

bool WebGLVkContext::drawTarget(RenderTarget& out) {
    out = RenderTarget{};
    if (framebufferStatus(drawFboId_) != GL_FRAMEBUFFER_COMPLETE) {
        setSyntheticError(GL_INVALID_FRAMEBUFFER_OPERATION);
        return false;
    }
    if (drawFboId_ == 0) {
        out.topDown = true;
        if (canvasDrawBuffer_ == GL_BACK) {
            out.color[0] = canvasSurface(false);
            out.colorCount = 1;
        }
        out.depth = canvasSurface(true);
        if (out.depth.aspects() & VK_IMAGE_ASPECT_STENCIL_BIT) out.stencil = out.depth;
        if (!(out.depth.aspects() & VK_IMAGE_ASPECT_DEPTH_BIT)) out.depth = Surface{};
        out.extent = {canvas_.width(), canvas_.height()};
        return true;
    }

    VkFramebufferResource& fbo = framebuffers_[drawFboId_];
    uint32_t width = UINT32_MAX, height = UINT32_MAX;
    auto fit = [&](const Surface& s) {
        width = std::min(width, s.width);
        height = std::min(height, s.height);
        out.samples = s.samples;
    };
    // Every attachment bounds the render area, drawn to or not.
    for (const VkFboAttachment& att : fbo.color)
        if (Surface s = attachmentSurface(att)) fit(s);
    for (uint32_t i = 0; i < kMaxColorAttachments; ++i) {
        if (fbo.drawBuffers[i] == GL_NONE) continue;
        out.color[i] = attachmentSurface(fbo.color[fbo.drawBuffers[i] - GL_COLOR_ATTACHMENT0]);
        if (out.color[i]) out.colorCount = i + 1;
    }
    if (Surface s = attachmentSurface(fbo.depth)) {
        fit(s);
        out.depth = s;
    }
    if (Surface s = attachmentSurface(fbo.stencil)) {
        fit(s);
        out.stencil = s;
    }
    out.extent = {width, height};
    return true;
}

bool WebGLVkContext::readColorSurface(Surface& out) {
    out = Surface{};
    if (framebufferStatus(readFboId_) != GL_FRAMEBUFFER_COMPLETE) {
        setSyntheticError(GL_INVALID_FRAMEBUFFER_OPERATION);
        return false;
    }
    if (readFboId_ == 0) {
        if (canvasReadBuffer_ == GL_BACK) out = canvasSurface(false);
        return true;
    }
    const VkFramebufferResource& fbo = framebuffers_[readFboId_];
    if (fbo.readBuffer != GL_NONE) out = attachmentSurface(fbo.color[fbo.readBuffer - GL_COLOR_ATTACHMENT0]);
    return true;
}

void WebGLVkContext::drawBuffers(GLsizei n, const GLenum* bufs) {
    if (n < 0 || static_cast<uint32_t>(n) > kMaxColorAttachments) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (drawFboId_ == 0) {
        // The canvas takes exactly one buffer: BACK or NONE.
        if (n != 1 || (bufs[0] != GL_BACK && bufs[0] != GL_NONE)) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        if (canvasDrawBuffer_ != bufs[0]) endRendering();
        canvasDrawBuffer_ = bufs[0];
        return;
    }
    // Buffer i is COLOR_ATTACHMENTi or NONE.
    for (GLsizei i = 0; i < n; ++i) {
        if (bufs[i] != GL_NONE && bufs[i] != GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i)) {
            setSyntheticError(bufs[i] == GL_BACK || isColorAttachment(bufs[i]) ? GL_INVALID_OPERATION
                                                                               : GL_INVALID_ENUM);
            return;
        }
    }
    VkFramebufferResource& fbo = framebuffers_[drawFboId_];
    std::array<GLenum, 8> next{};
    for (GLsizei i = 0; i < n; ++i) next[i] = bufs[i];
    if (next != fbo.drawBuffers) {
        endRendering();
        fbo.drawBuffers = next;
    }
}

void WebGLVkContext::readBuffer(GLenum src) {
    if (readFboId_ == 0) {
        if (src != GL_BACK && src != GL_NONE) {
            setSyntheticError(isColorAttachment(src) ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
            return;
        }
        canvasReadBuffer_ = src;
        return;
    }
    if (src != GL_NONE && !isColorAttachment(src)) {
        setSyntheticError(src == GL_BACK ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
        return;
    }
    framebuffers_[readFboId_].readBuffer = src;
}

GLenum WebGLVkContext::drawBufferState(GLuint i) const {
    if (i >= kMaxColorAttachments) return GL_NONE;
    if (drawFboId_ == 0) return i == 0 ? canvasDrawBuffer_ : GL_NONE;
    auto it = framebuffers_.find(drawFboId_);
    return it != framebuffers_.end() ? it->second.drawBuffers[i] : GL_NONE;
}

GLenum WebGLVkContext::readBufferState() const {
    if (readFboId_ == 0) return canvasReadBuffer_;
    auto it = framebuffers_.find(readFboId_);
    return it != framebuffers_.end() ? it->second.readBuffer : GL_NONE;
}

bool WebGLVkContext::getFramebufferAttachmentParameter(GLenum target, GLenum attachment, GLenum pname,
                                                       AttachmentParameter& out) {
    out = AttachmentParameter{};
    GLuint id = 0;
    VkFramebufferResource* fbo = framebufferForTarget(target, id);
    if (!isFramebufferTarget(target)) return false;

    Surface surface;
    VkFboAttachment att;
    if (!fbo) {
        // The canvas: BACK, DEPTH and STENCIL, all FRAMEBUFFER_DEFAULT.
        if (attachment == GL_BACK) surface = canvasSurface(false);
        else if (attachment == 0x1801 /* DEPTH */ || attachment == 0x1802 /* STENCIL */) surface = canvasSurface(true);
        else {
            setSyntheticError(GL_INVALID_ENUM);
            return false;
        }
        if (pname == kAttachmentObjectType) {
            out.value = 0x8218;  // FRAMEBUFFER_DEFAULT
            return true;
        }
    } else {
        if (isColorAttachment(attachment)) att = fbo->color[attachment - GL_COLOR_ATTACHMENT0];
        else if (attachment == GL_DEPTH_ATTACHMENT) att = fbo->depth;
        else if (attachment == GL_STENCIL_ATTACHMENT) att = fbo->stencil;
        else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            if (!(fbo->depth == fbo->stencil)) {
                setSyntheticError(GL_INVALID_OPERATION);
                return false;
            }
            att = fbo->depth;
        } else {
            setSyntheticError(GL_INVALID_ENUM);
            return false;
        }
        if (pname == kAttachmentObjectType) {
            out.value = att.kind == VkFboAttachment::Kind::Texture        ? kTexture
                        : att.kind == VkFboAttachment::Kind::Renderbuffer ? GL_RENDERBUFFER
                                                                          : GL_NONE;
            return true;
        }
        if (att.kind == VkFboAttachment::Kind::None) {
            if (pname == kAttachmentObjectName) {
                out.isNull = true;
                return true;
            }
            setSyntheticError(GL_INVALID_OPERATION);
            return false;
        }
        if (pname == kAttachmentObjectName) {
            out.objectType = att.kind == VkFboAttachment::Kind::Texture ? kTexture : GL_RENDERBUFFER;
            out.objectName = att.id;
            return true;
        }
        if (att.kind == VkFboAttachment::Kind::Texture) {
            const auto it = textures_.find(att.id);
            const bool cube = it != textures_.end() && it->second.target == kTextureCubeMap;
            if (pname == kAttachmentTextureLevel) {
                out.value = static_cast<GLint>(att.level);
                return true;
            }
            if (pname == kAttachmentTextureCubeMapFace) {
                out.value = cube ? static_cast<GLint>(kCubeFacePosX + att.layer) : 0;
                return true;
            }
            if (pname == kAttachmentTextureLayer) {
                out.value = cube ? 0 : static_cast<GLint>(att.layer);
                return true;
            }
        }
        surface = attachmentSurface(att);
    }

    const FormatBits bits = formatBits(surface.format);
    if (pname >= kAttachmentRedSize && pname <= kAttachmentStencilSize) {
        const int sizes[] = {bits.red, bits.green, bits.blue, bits.alpha, bits.depth, bits.stencil};
        out.value = sizes[pname - kAttachmentRedSize];
        return true;
    }
    if (pname == kAttachmentComponentType) {
        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            setSyntheticError(GL_INVALID_OPERATION);
            return false;
        }
        if (attachment == GL_STENCIL_ATTACHMENT || attachment == 0x1802) out.value = GL_UNSIGNED_INT;
        else if (isSignedIntegerFormat(surface.format)) out.value = GL_INT;
        else if (isIntegerFormat(surface.format)) out.value = GL_UNSIGNED_INT;
        else if (surface.format == VK_FORMAT_D32_SFLOAT || surface.format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                 (surface.aspects() == VK_IMAGE_ASPECT_COLOR_BIT && bits.red != 8 && bits.red != 10))
            out.value = kFloat;
        else out.value = kUnsignedNormalized;
        return true;
    }
    if (pname == kAttachmentColorEncoding) {
        out.value = surface.format == VK_FORMAT_R8G8B8A8_SRGB ? kSrgb : kLinear;
        return true;
    }
    setSyntheticError(GL_INVALID_ENUM);
    return false;
}

// ---------------------------------------------------------------------------
// Renderbuffers
// ---------------------------------------------------------------------------

WebGLRenderbuffer WebGLVkContext::createRenderbuffer() {
    GLuint id = nextRenderbufferId_++;
    renderbuffers_[id] = VkRenderbufferResource{};
    return {id};
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
    if (rbo.id != 0 && renderbuffers_.find(rbo.id) == renderbuffers_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
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
    tex.bytesPerPixel = 0;  // never uploaded to

    const VkImageSubresourceRange range{aspects, 0, 1, 0, 1};
    render::cmdTransitionImage(cmd, tex.image, range, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (color) {
        const VkClearColorValue clear{};
        vkCmdClearColorImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    } else {
        const VkClearDepthStencilValue clear{1.0f, 0};
        vkCmdClearDepthStencilImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }
    tex.currentLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    transitionTexture(cmd, tex, attachmentLayout(format));
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
    const FormatBits bits = formatBits(rb.storage.isValid() ? rb.storage.format : VK_FORMAT_UNDEFINED);
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
