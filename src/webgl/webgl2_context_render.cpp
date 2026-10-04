#include "webgl/webgl2_context.h"
#include "webgl/glsl_translator.h"
#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl {

// ===========================================================================
// Framebuffers
// ===========================================================================

WebGLFramebuffer WebGL2RenderingContext::createFramebuffer() {
    if (vkCtx_) return vkCtx_->createFramebuffer();
    GLuint id = 0;
    glGenFramebuffers(1, &id);
    validFramebuffers_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteFramebuffer(WebGLFramebuffer fbo) {
    if (vkCtx_) { vkCtx_->deleteFramebuffer(fbo); return; }
    if (fbo.id && validFramebuffers_.erase(fbo.id)) {
        // Deleting the bound FBO reverts to the default framebuffer, which
        // for WebGL is the canvas FBO.
        if (sFBO_ == fbo.id) sFBO_ = canvasFBO_;
        glDeleteFramebuffers(1, &fbo.id);
    }
}

void WebGL2RenderingContext::bindFramebuffer(GLenum target, WebGLFramebuffer fbo) {
    if (vkCtx_) { vkCtx_->bindFramebuffer(target, fbo); return; }
    // WebGL: null framebuffer = our canvas FBO (not the real default 0)
    GLuint id = fbo.id ? fbo.id : canvasFBO_;
    sFBO_ = id;
    glBindFramebuffer(target, id);
}


void WebGL2RenderingContext::framebufferTexture2D(GLenum target, GLenum attachment,
                                                   GLenum textarget, WebGLTexture tex, GLint level) {
    if (vkCtx_) { vkCtx_->framebufferTexture2D(target, attachment, textarget, tex, level); return; }
    glFramebufferTexture2D(target, attachment, textarget, tex.id, level);
}

void WebGL2RenderingContext::framebufferRenderbuffer(GLenum target, GLenum attachment,
                                                      GLenum renderbuffertarget, WebGLRenderbuffer rbo) {
    glFramebufferRenderbuffer(target, attachment, renderbuffertarget, rbo.id);
}

GLenum WebGL2RenderingContext::checkFramebufferStatus(GLenum target) {
    if (vkCtx_) return vkCtx_->checkFramebufferStatus(target);
    return glCheckFramebufferStatus(target);
}

void WebGL2RenderingContext::readPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                                         GLenum format, GLenum type, void* pixels) {
    if (vkCtx_) { vkCtx_->readPixels(x, y, width, height, format, type, pixels); return; }
    glReadPixels(x, y, width, height, format, type, pixels);
}

void WebGL2RenderingContext::readBuffer(GLenum src) {
    // Same emulation as drawBuffers: on the canvas FBO the page names BACK,
    // which a real FBO does not have.
    if (sFBO_ == canvasFBO_ && (src == GL_BACK || src == GL_FRONT))
        src = GL_COLOR_ATTACHMENT0;
    glReadBuffer(src);
}

bool WebGL2RenderingContext::validateReadPixels(GLsizei width, GLsizei height,
                                                GLenum format, GLenum type, size_t dstLen) {
    if (width < 0 || height < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return false;
    }
    int bpp = bytesPerPixel(format, type);
    if (bpp <= 0) return true; // unknown combo — let GL validate/reject it
    size_t row = (size_t)width * bpp;
    size_t align = packAlignment_ > 0 ? (size_t)packAlignment_ : 4;
    size_t stride = (row + align - 1) / align * align;
    size_t required = height > 0 ? stride * (height - 1) + row : 0;
    if (dstLen < required) {
        // WebGL: destination buffer too small → INVALID_OPERATION, no write.
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    return true;
}

int64_t WebGL2RenderingContext::boundBufferSize(GLenum target) {
    GLint64 sz = 0;
    glGetBufferParameteri64v(target, GL_BUFFER_SIZE, &sz);
    return (int64_t)sz;
}

void WebGL2RenderingContext::readPixelsToPBO(GLint x, GLint y, GLsizei width, GLsizei height,
                                              GLenum format, GLenum type, GLintptr offset) {
    if (!sPixelPack_) {
        // Offset overload without a PIXEL_PACK buffer bound.
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (offset < 0 || width < 0 || height < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    int bpp = bytesPerPixel(format, type);
    if (bpp > 0) {
        // Same no-overflow guarantee as the client-memory path, but against
        // the PBO's byte size.
        size_t row = (size_t)width * bpp;
        size_t align = packAlignment_ > 0 ? (size_t)packAlignment_ : 4;
        size_t stride = (row + align - 1) / align * align;
        size_t required = height > 0 ? stride * (height - 1) + row : 0;
        int64_t avail = boundBufferSize(GL_PIXEL_PACK_BUFFER) - (int64_t)offset;
        if (avail < 0 || (size_t)avail < required) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
    }
    glReadPixels(x, y, width, height, format, type, (void*)offset);
}

void WebGL2RenderingContext::texImage2DFromPBO(GLenum target, GLint level, GLint internalformat,
                                                GLsizei width, GLsizei height, GLint border,
                                                GLenum format, GLenum type, GLintptr offset) {
    if (!sPixelUnpack_ || offset < 0) {
        setSyntheticError(!sPixelUnpack_ ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
        return;
    }
    // WebGL2: FLIP_Y / PREMULTIPLY_ALPHA only apply to client-memory uploads;
    // uploading from a PBO with either set is INVALID_OPERATION, not silent
    // untransformed data.
    if (unpackFlipY_ || unpackPremultiplyAlpha_) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    // GL bounds-checks the read against the PBO size itself (INVALID_OPERATION).
    glTexImage2D(target, level, translateInternalFormat(internalformat, type),
                 width, height, border, format, type, (const void*)offset);
}

void WebGL2RenderingContext::texSubImage2DFromPBO(GLenum target, GLint level,
                                                   GLint xoffset, GLint yoffset,
                                                   GLsizei width, GLsizei height,
                                                   GLenum format, GLenum type, GLintptr offset) {
    if (!sPixelUnpack_ || offset < 0) {
        setSyntheticError(!sPixelUnpack_ ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
        return;
    }
    if (unpackFlipY_ || unpackPremultiplyAlpha_) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type,
                    (const void*)offset);
}

void WebGL2RenderingContext::copyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                                             GLint x, GLint y, GLsizei width, GLsizei height,
                                             GLint border) {
    glCopyTexImage2D(target, level,
                     (GLenum)translateInternalFormat((GLint)internalformat, GL_UNSIGNED_BYTE),
                     x, y, width, height, border);
}

void WebGL2RenderingContext::copyTexSubImage2D(GLenum target, GLint level,
                                                GLint xoffset, GLint yoffset,
                                                GLint x, GLint y, GLsizei width, GLsizei height) {
    glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
}

void WebGL2RenderingContext::drawBuffers(GLsizei n, const GLenum* bufs) {
    // WebGL's "default framebuffer" is our canvas FBO, so the page's `BACK` —
    // the only colour buffer the spec lets it name there — has to become the
    // attachment that FBO actually has. Passing BACK to a real FBO is
    // GL_INVALID_ENUM, which silently leaves the draw-buffer state from
    // whatever render target ran last.
    if (sFBO_ == canvasFBO_) {
        std::vector<GLenum> mapped(bufs, bufs + n);
        for (auto& b : mapped)
            if (b == GL_BACK || b == GL_FRONT || b == GL_FRONT_AND_BACK)
                b = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(n, mapped.data());
        return;
    }
    glDrawBuffers(n, bufs);
}

void WebGL2RenderingContext::blitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                              GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                              GLbitfield mask, GLenum filter) {
    glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

// ===========================================================================
// Renderbuffers
// ===========================================================================

WebGLRenderbuffer WebGL2RenderingContext::createRenderbuffer() {
    GLuint id = 0;
    glGenRenderbuffers(1, &id);
    validRenderbuffers_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteRenderbuffer(WebGLRenderbuffer rbo) {
    if (rbo.id && validRenderbuffers_.erase(rbo.id)) {
        glDeleteRenderbuffers(1, &rbo.id);
    }
}

void WebGL2RenderingContext::bindRenderbuffer(GLenum target, WebGLRenderbuffer rbo) {
    glBindRenderbuffer(target, rbo.id);
}

void WebGL2RenderingContext::renderbufferStorage(GLenum target, GLenum internalformat,
                                                  GLsizei width, GLsizei height) {
    glRenderbufferStorage(target, internalformat, width, height);
    initializeRenderbuffer(internalformat);
}

void WebGL2RenderingContext::renderbufferStorageMultisample(GLenum target, GLsizei samples,
                                                            GLenum internalformat,
                                                            GLsizei width, GLsizei height) {
    glRenderbufferStorageMultisample(target, samples, internalformat, width, height);
    initializeRenderbuffer(internalformat);
}

// WebGL §4.1: a freshly allocated framebuffer attachment reads as the *default
// clear values* — colour (0,0,0,0), depth 1.0, stencil 0 — where plain GL
// leaves it undefined. Depth is the one that matters: GL hands back a buffer
// that in practice reads as 0.0, so with the default `LESS` test every fragment
// drawn before the first clear is rejected and the target comes out black.
//
// That is not a corner case. three.js's PMREMGenerator renders its six cube
// faces with `autoClear = false`, trusting the spec's 1.0 — so on an
// uninitialized buffer the environment map is entirely black, and every
// material lit by it renders black with no error anywhere. Clear it here, at
// allocation, which is where the spec's guarantee begins.
void WebGL2RenderingContext::initializeRenderbuffer(GLenum internalformat) {
    bool hasDepth = false, hasStencil = false;
    switch (internalformat) {
        case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24:
        case GL_DEPTH_COMPONENT32F:
            hasDepth = true; break;
        case GL_DEPTH24_STENCIL8: case GL_DEPTH32F_STENCIL8:
            hasDepth = true; hasStencil = true; break;
        case GL_STENCIL_INDEX8:
            hasStencil = true; break;
        default:
            // Colour renderbuffers already read as zero on every driver we
            // target, and clearing them here would cost a pass per allocation.
            return;
    }

    GLint boundRbo = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &boundRbo);
    if (boundRbo == 0) return;

    GLint prevDraw = 0, prevRead = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);

    const bool freshFbo = (initFbo_ == 0);
    if (freshFbo) glGenFramebuffers(1, &initFbo_);
    // Bound to BOTH targets: glDrawBuffers/glReadBuffer below act on whichever
    // framebuffer is bound to that target, so binding only the draw target
    // would point the *caller's* read framebuffer at GL_NONE and break the
    // next readPixels it does.
    glBindFramebuffer(GL_FRAMEBUFFER, initFbo_);
    if (freshFbo) {
        // Depth/stencil only: without this the default draw buffer of
        // COLOR_ATTACHMENT0 names an attachment that will never exist, and the
        // framebuffer is INCOMPLETE_DRAW_BUFFER — so the clear below would be
        // skipped and the whole exercise would silently do nothing.
        glDrawBuffers(0, nullptr);
        glReadBuffer(GL_NONE);
    }
    const GLenum attachment = (hasDepth && hasStencil) ? GL_DEPTH_STENCIL_ATTACHMENT
                            : hasDepth                 ? GL_DEPTH_ATTACHMENT
                                                       : GL_STENCIL_ATTACHMENT;
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, attachment, GL_RENDERBUFFER,
                              static_cast<GLuint>(boundRbo));

    // A clear obeys the scissor test and the depth/stencil write masks, any of
    // which the page may have left in a state that would skip part or all of
    // the buffer. Force them open for the clear and put them back after.
    GLboolean scissorOn = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean depthMask = GL_TRUE;
    GLint stencilMaskFront = 0xFF, stencilMaskBack = 0xFF;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilMaskFront);
    glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &stencilMaskBack);
    if (scissorOn) glDisable(GL_SCISSOR_TEST);
    if (!depthMask) glDepthMask(GL_TRUE);
    glStencilMask(0xFF);

    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        if (hasDepth && hasStencil)  glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
        else if (hasDepth)         { const GLfloat one = 1.0f; glClearBufferfv(GL_DEPTH, 0, &one); }
        else                       { const GLint zero = 0;     glClearBufferiv(GL_STENCIL, 0, &zero); }
    }

    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, attachment, GL_RENDERBUFFER, 0);
    if (scissorOn) glEnable(GL_SCISSOR_TEST);
    if (!depthMask) glDepthMask(GL_FALSE);
    glStencilMaskSeparate(GL_FRONT, static_cast<GLuint>(stencilMaskFront));
    glStencilMaskSeparate(GL_BACK, static_cast<GLuint>(stencilMaskBack));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDraw));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
}

// ===========================================================================
// Draw calls
// ===========================================================================

void WebGL2RenderingContext::drawArrays(GLenum mode, GLint first, GLsizei count) {
    if (vkCtx_) { vkCtx_->drawArrays(mode, first, count); return; }
    glDrawArrays(mode, first, count);
}

void WebGL2RenderingContext::drawElements(GLenum mode, GLsizei count, GLenum type, GLintptr offset) {
    if (vkCtx_) { vkCtx_->drawElements(mode, count, type, static_cast<uintptr_t>(offset)); return; }
    glDrawElements(mode, count, type, (const void*)offset);
}

void WebGL2RenderingContext::drawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instanceCount) {
    if (vkCtx_) { vkCtx_->drawArraysInstanced(mode, first, count, instanceCount); return; }
    glDrawArraysInstanced(mode, first, count, instanceCount);
}

void WebGL2RenderingContext::drawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                                    GLintptr offset, GLsizei instanceCount) {
    if (vkCtx_) { vkCtx_->drawElementsInstanced(mode, count, type, static_cast<uintptr_t>(offset), instanceCount); return; }
    glDrawElementsInstanced(mode, count, type, (const void*)offset, instanceCount);
}

void WebGL2RenderingContext::drawRangeElements(GLenum mode, GLuint start, GLuint end,
                                                GLsizei count, GLenum type, GLintptr offset) {
    glDrawRangeElements(mode, start, end, count, type, (const void*)offset);
}

// ===========================================================================
// Queries / parameters
// ===========================================================================

GLint WebGL2RenderingContext::getParameterInt(GLenum pname) {
    GLint val = 0;
    glGetIntegerv(pname, &val);
    return val;
}

GLfloat WebGL2RenderingContext::getParameterFloat(GLenum pname) {
    GLfloat val = 0;
    glGetFloatv(pname, &val);
    return val;
}

GLboolean WebGL2RenderingContext::getParameterBool(GLenum pname) {
    GLboolean val = GL_FALSE;
    glGetBooleanv(pname, &val);
    return val;
}

std::string WebGL2RenderingContext::getParameterString(GLenum pname) {
    const GLubyte* str = glGetString(pname);
    return str ? std::string(reinterpret_cast<const char*>(str)) : "";
}

std::string WebGL2RenderingContext::getShadingLanguageVersion() {
    return "WebGL GLSL ES 3.00";
}

std::vector<std::string> WebGL2RenderingContext::getSupportedExtensions() {
    // Extensions that WebGL2 typically exposes; the base list is all core in
    // GL 3.3, compressed-texture families are gated on the driver's actual
    // extension support (probed at context creation — no ETC2 on desktop GL).
    std::vector<std::string> exts = {
        "EXT_color_buffer_float",
        "EXT_float_blend",
        "OES_texture_float_linear",
        "EXT_texture_filter_anisotropic",
        "EXT_blend_minmax",
        "OES_vertex_array_object",
        "OES_element_index_uint",
        "OES_standard_derivatives",
        "OES_fbo_render_mipmap",
        "WEBGL_depth_texture",
        "WEBGL_draw_buffers",
        "EXT_shader_texture_lod",
        "EXT_sRGB",
        "EXT_frag_depth",
        "ANGLE_instanced_arrays",
        "OES_texture_half_float",
        "OES_texture_half_float_linear",
        // WEBGL_lose_context is deliberately NOT here: its object is two
        // METHODS (loseContext/restoreContext), and this engine has no
        // context-loss machinery to put behind them (isContextLost is
        // hard false). Advertising it hands callers a method-less object —
        // pixi's isWebGLSupported calls loseContext() on it, catches the
        // TypeError, and concludes WebGL itself is unsupported.
        // bro extension, not a WebGL one: desktop GL 3.0 buffer mapping, which
        // WebGL cannot offer because it must not hand a page a raw pointer into
        // driver memory. Advertised so apps can feature-detect rather than
        // sniff for the methods.
        "BRO_buffer_map",
    };
    if (GLAD_GL_EXT_texture_compression_s3tc) {
        exts.push_back("WEBGL_compressed_texture_s3tc");
        if (GLAD_GL_EXT_texture_sRGB)
            exts.push_back("WEBGL_compressed_texture_s3tc_srgb");
    }
    exts.push_back("EXT_texture_compression_rgtc"); // core since GL 3.0
    if (GLAD_GL_ARB_texture_compression_bptc)
        exts.push_back("EXT_texture_compression_bptc");
    return exts;
}

bool WebGL2RenderingContext::getExtension(const std::string& name) {
    // Desktop GL 3.3 natively supports most WebGL2 extensions
    auto exts = getSupportedExtensions();
    for (auto& ext : exts) {
        if (ext == name) return true;
    }
    return false;
}

// ===========================================================================
// Object predicates
// ===========================================================================
// The valid-set check guards against GL id reuse after delete (a stale
// wrapper must answer false even if the driver handed the id to a new
// object); glIs* then supplies the created-on-first-bind semantics.

GLboolean WebGL2RenderingContext::isBuffer(WebGLBuffer buf) {
    if (!buf.id || !validBuffers_.count(buf.id)) return GL_FALSE;
    return glIsBuffer(buf.id);
}

GLboolean WebGL2RenderingContext::isTexture(WebGLTexture tex) {
    if (!tex.id || !validTextures_.count(tex.id)) return GL_FALSE;
    return glIsTexture(tex.id);
}

GLboolean WebGL2RenderingContext::isFramebuffer(WebGLFramebuffer fbo) {
    if (!fbo.id || !validFramebuffers_.count(fbo.id)) return GL_FALSE;
    return glIsFramebuffer(fbo.id);
}

GLboolean WebGL2RenderingContext::isRenderbuffer(WebGLRenderbuffer rbo) {
    if (!rbo.id || !validRenderbuffers_.count(rbo.id)) return GL_FALSE;
    return glIsRenderbuffer(rbo.id);
}

GLboolean WebGL2RenderingContext::isProgram(WebGLProgram program) {
    if (!program.id || !validPrograms_.count(program.id)) return GL_FALSE;
    return glIsProgram(program.id);
}

GLboolean WebGL2RenderingContext::isShader(WebGLShader shader) {
    if (!shader.id || !validShaders_.count(shader.id)) return GL_FALSE;
    return glIsShader(shader.id);
}

GLboolean WebGL2RenderingContext::isVertexArray(WebGLVertexArrayObject vao) {
    if (!vao.id || !validVAOs_.count(vao.id)) return GL_FALSE;
    return glIsVertexArray(vao.id);
}

// ===========================================================================
// Misc
// ===========================================================================

void WebGL2RenderingContext::flush() { glFlush(); }
void WebGL2RenderingContext::finish() { glFinish(); }
void WebGL2RenderingContext::hint(GLenum target, GLenum mode) { glHint(target, mode); }

// ===========================================================================
// Shadow state restore — called after engine compositing to undo all GL
// state changes without any glGet* queries.
// ===========================================================================

void WebGL2RenderingContext::restoreState() {
    glUseProgram(sProgram_);
    glBindVertexArray(sVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, sArrayBuf_);
    // GL_ELEMENT_ARRAY_BUFFER is part of VAO state — binding it here would
    // overwrite the VAO's captured element buffer. Only restore when the
    // default VAO (0) is active, where EAB is context-level state.
    if (sVAO_ == 0) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sElementBuf_);
    }
    if (sPixelPack_) glBindBuffer(GL_PIXEL_PACK_BUFFER, sPixelPack_);
    if (sPixelUnpack_) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sPixelUnpack_);

    // Restore sampler objects (unbound around compositing in unbindCanvasFBO)
    for (unsigned u = 0; u < 32; u++) {
        if (sSampler_[u]) glBindSampler(u, sSampler_[u]);
    }

    // Restore texture bindings — unit 0 is the most commonly modified
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sTex2D_[0]);
    if (sActiveTex_ != GL_TEXTURE0) {
        glActiveTexture(sActiveTex_);
        unsigned unit = sActiveTex_ - GL_TEXTURE0;
        if (unit < 32) glBindTexture(GL_TEXTURE_2D, sTex2D_[unit]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, sFBO_);
    glClearColor(sClearR_, sClearG_, sClearB_, sClearA_);
    glViewport(sViewport_[0], sViewport_[1], sViewport_[2], sViewport_[3]);

    if (sBlend_) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (sDepthTest_) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (sCullFace_) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (sScissorTest_) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (sStencilTest_) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (sRasterizerDiscard_) glEnable(GL_RASTERIZER_DISCARD);

    // Re-bind the app's transform feedback object and resume a TF that
    // unbindCanvasFBO paused around engine compositing.
    if (transformFeedbackObjectsSupported()) {
        glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, sTransformFeedback_);
        if (tfActive_ && !tfPaused_) glResumeTransformFeedback();
    }

    glBlendFuncSeparate(sBlendSrcRGB_, sBlendDstRGB_, sBlendSrcA_, sBlendDstA_);
    glBlendEquationSeparate(sBlendEqRGB_, sBlendEqA_);
    glDepthFunc(sDepthFunc_);
    glDepthMask(sDepthMask_);
    glColorMask(sColorMask_[0], sColorMask_[1], sColorMask_[2], sColorMask_[3]);
    glCullFace(sCullMode_);
    glFrontFace(sFrontFace_);
}

} // namespace bro::webgl
