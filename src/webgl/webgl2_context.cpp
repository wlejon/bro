#include "webgl/webgl2_context.h"
#include "webgl/glsl_translator.h"
#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl {

// ===========================================================================
// Construction / destruction
// ===========================================================================

WebGL2RenderingContext::WebGL2RenderingContext(int width, int height, render::VulkanContext* vkContext)
    : width_(width), height_(height) {
    if (!vkContext) vkContext = defaultVulkanContext_;
    if (vkContext) {
        vkCtx_ = std::make_unique<vk::WebGLVkContext>(width, height, *vkContext);
        sViewport_[2] = width_;
        sViewport_[3] = height_;
        LOG_INFO("WebGL2RenderingContext created with Vulkan backend (%dx%d)", width, height);
        return;
    }

    createCanvasFBO();

    // WebGL semantics that desktop GL 3.3 core does not default to:
    // gl_PointSize only takes effect with PROGRAM_POINT_SIZE enabled, and
    // WebGL2 cube map sampling is always seamless.
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    // A fresh WebGL context presents spec-default state (blend off, depth
    // func LESS, clear color transparent black, viewport = canvas, the canvas
    // FBO bound as the "default framebuffer", ...) regardless of what state
    // the engine's shared GL context happens to be in. The shadow-state
    // members already hold those defaults; push them into GL now.
    sFBO_ = canvasFBO_;
    sViewport_[2] = width_;
    sViewport_[3] = height_;
    restoreState();

    // Probe the driver's compressed-texture support once. Desktop GL 3.3 has
    // no ETC2 (that's 4.3 / ARB_ES3_compatibility's *format list*, which
    // drivers typically decompress in software or reject); we expose the
    // desktop-native families only: S3TC / RGTC / BPTC.
    if (GLAD_GL_EXT_texture_compression_s3tc) {
        compressedFormats_.insert(compressedFormats_.end(),
            {0x83F0, 0x83F1, 0x83F2, 0x83F3}); // DXT1 / DXT1a / DXT3 / DXT5
        if (GLAD_GL_EXT_texture_sRGB) {
            compressedFormats_.insert(compressedFormats_.end(),
                {0x8C4C, 0x8C4D, 0x8C4E, 0x8C4F}); // sRGB S3TC variants
        }
    }
    // RGTC is core since GL 3.0 — always present on a 3.3 context.
    compressedFormats_.insert(compressedFormats_.end(),
        {0x8DBB, 0x8DBC, 0x8DBD, 0x8DBE}); // RGTC1 / signed / RGTC2 / signed
    if (GLAD_GL_ARB_texture_compression_bptc) {
        compressedFormats_.insert(compressedFormats_.end(),
            {0x8E8C, 0x8E8D, 0x8E8E, 0x8E8F}); // BPTC unorm / srgb / sf / uf
    }
    glScissor(0, 0, width_, height_);
    glClearDepth(1.0);
    glClearStencil(0);

    LOG_INFO("WebGL2RenderingContext created (%dx%d)", width, height);
}

WebGL2RenderingContext::~WebGL2RenderingContext() {
    // A destroyed context must not stay the "live" one, or the next
    // makeCurrent() on another context would skip restoring its state.
    if (current_ == this) current_ = nullptr;

    for (auto& cb : teardownCallbacks_) {
        if (cb) cb(this);
    }
    teardownCallbacks_.clear();

    if (vkCtx_) {
        vkCtx_.reset();
        return;
    }

    // Delete all tracked objects
    for (GLuint id : validBuffers_) glDeleteBuffers(1, &id);
    for (GLuint id : validTextures_) glDeleteTextures(1, &id);
    for (GLuint id : validPrograms_) glDeleteProgram(id);
    for (GLuint id : validShaders_) glDeleteShader(id);
    for (GLuint id : validFramebuffers_) glDeleteFramebuffers(1, &id);
    for (GLuint id : validRenderbuffers_) glDeleteRenderbuffers(1, &id);
    for (GLuint id : validVAOs_) glDeleteVertexArrays(1, &id);
    for (GLuint id : validSamplers_) glDeleteSamplers(1, &id);
    for (GLuint id : validQueries_) glDeleteQueries(1, &id);
    for (GLsync s : validSyncs_) glDeleteSync(s);
    for (GLuint id : validTransformFeedbacks_) glDeleteTransformFeedbacks(1, &id);
    destroyCanvasFBO();
}

void WebGL2RenderingContext::createCanvasFBO() {
    int w = std::max(1, width_);
    int h = std::max(1, height_);
    glGenFramebuffers(1, &canvasFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, canvasFBO_);

    // Color attachment (RGBA8)
    glGenTextures(1, &colorTex_);
    glBindTexture(GL_TEXTURE_2D, colorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex_, 0);

    // Depth-stencil renderbuffer
    glGenRenderbuffers(1, &depthStencilRBO_);
    glBindRenderbuffer(GL_RENDERBUFFER, depthStencilRBO_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, depthStencilRBO_);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("WebGL canvas FBO incomplete: 0x%x", status);
    }

    // WebGL drawing buffers start as transparent black, not undefined memory.
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFFFFFFFFu);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

void WebGL2RenderingContext::destroyCanvasFBO() {
    if (depthStencilRBO_) { glDeleteRenderbuffers(1, &depthStencilRBO_); depthStencilRBO_ = 0; }
    if (colorTex_) { glDeleteTextures(1, &colorTex_); colorTex_ = 0; }
    if (canvasFBO_) { glDeleteFramebuffers(1, &canvasFBO_); canvasFBO_ = 0; }
}

void WebGL2RenderingContext::resize(int width, int height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    if (vkCtx_) {
        vkCtx_->resize(width, height);
        return;
    }
    GLuint oldCanvasFBO = canvasFBO_;
    destroyCanvasFBO();
    createCanvasFBO();
    // createCanvasFBO clobbers clear color / masks / scissor enable, and the
    // old canvas FBO id is gone. If the app had the "default framebuffer"
    // (null → old canvas FBO) bound, re-point shadow state at the new one,
    // then reapply the app's shadow-tracked state.
    if (sFBO_ == oldCanvasFBO || sFBO_ == 0) sFBO_ = canvasFBO_;
    restoreState();
}

WebGL2RenderingContext* WebGL2RenderingContext::current_ = nullptr;

void WebGL2RenderingContext::makeCurrent() {
    if (current_ == this) return;
    current_ = this;
    if (vkCtx_) return;
    // restoreState() re-applies every piece of shadow state this context
    // tracks, including glBindFramebuffer(sFBO_) — which is exactly what
    // makes a second canvas draw into its own FBO rather than the first's.
    restoreState();
}

void WebGL2RenderingContext::bindCanvasFBO() {
    if (vkCtx_) { vkCtx_->bindCanvasFBO(); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, canvasFBO_);
}

void WebGL2RenderingContext::unbindCanvasFBO() {
    if (vkCtx_) { vkCtx_->unbindCanvasFBO(); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // Neutralize WebGL state that would corrupt the engine's own GL work
    // (compositing, screenshot readback). restoreState() re-applies it before
    // control returns to the app.
    for (unsigned u = 0; u < 32; u++) {
        if (sSampler_[u]) glBindSampler(u, 0);
    }
    // A transform feedback left active would capture (or reject) the
    // compositor's own draws; a leaked RASTERIZER_DISCARD would blank them.
    if (tfActive_ && !tfPaused_ && transformFeedbackObjectsSupported()) {
        glPauseTransformFeedback();
    }
    if (sRasterizerDiscard_) glDisable(GL_RASTERIZER_DISCARD);
    // A bound PIXEL_PACK buffer would swallow the engine's screenshot /
    // getPixel readbacks (glReadPixels writes into the PBO instead of client
    // memory); a bound PIXEL_UNPACK buffer would corrupt engine texture
    // uploads the same way.
    if (sPixelPack_) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (sPixelUnpack_) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

bool WebGL2RenderingContext::readCanvasPixels(std::vector<uint8_t>& out) {
    if (vkCtx_) {
        return vkCtx_->readCanvasPixels(out);
    }
    if (!canvasFBO_ || width_ <= 0 || height_ <= 0) return false;

    // Read straight from the canvas FBO rather than whatever the app last
    // bound: toDataURL() is defined on the canvas's own drawing buffer, and an
    // app that leaves a render target bound (three.js does, mid-frame) would
    // otherwise get that target's contents back instead.
    glBindFramebuffer(GL_FRAMEBUFFER, canvasFBO_);
    // A bound PIXEL_PACK buffer would send the read into the buffer object
    // instead of `out`; the alignment default of 4 mis-strides odd widths.
    if (sPixelPack_) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    const size_t rowBytes = static_cast<size_t>(width_) * 4;
    out.assign(rowBytes * static_cast<size_t>(height_), 0);
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, out.data());

    // GL hands back rows bottom-up; encoders and the 2D surface are top-down.
    std::vector<uint8_t> row(rowBytes);
    for (int y = 0; y < height_ / 2; ++y) {
        uint8_t* a = out.data() + static_cast<size_t>(y) * rowBytes;
        uint8_t* b = out.data() + static_cast<size_t>(height_ - 1 - y) * rowBytes;
        std::memcpy(row.data(), a, rowBytes);
        std::memcpy(a, b, rowBytes);
        std::memcpy(b, row.data(), rowBytes);
    }

    glPixelStorei(GL_PACK_ALIGNMENT, packAlignment_);
    restoreState();
    return true;
}

// ===========================================================================
// State
// ===========================================================================

void WebGL2RenderingContext::viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (vkCtx_) { vkCtx_->viewport(x, y, w, h); return; }
    sViewport_[0] = x; sViewport_[1] = y; sViewport_[2] = w; sViewport_[3] = h;
    glViewport(x, y, w, h);
}
void WebGL2RenderingContext::scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (vkCtx_) { vkCtx_->scissor(x, y, w, h); return; }
    glScissor(x, y, w, h);
}
void WebGL2RenderingContext::clearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    if (vkCtx_) { vkCtx_->clearColor(r, g, b, a); return; }
    sClearR_ = r; sClearG_ = g; sClearB_ = b; sClearA_ = a;
    glClearColor(r, g, b, a);
}
void WebGL2RenderingContext::clearDepth(GLfloat depth) {
    if (vkCtx_) { vkCtx_->clearDepth(depth); return; }
    glClearDepth(depth);
}
void WebGL2RenderingContext::clearStencil(GLint s) {
    if (vkCtx_) { vkCtx_->clearStencil(s); return; }
    glClearStencil(s);
}
void WebGL2RenderingContext::clear(GLbitfield mask) {
    if (vkCtx_) { vkCtx_->clear(mask); return; }
    glClear(mask);
}

// The clearBuffer* family targets attachments by index, so unlike clear() it
// needs no BACK→COLOR_ATTACHMENT0 mapping on the canvas FBO: drawbuffer 0 IS
// that attachment there.
void WebGL2RenderingContext::clearBufferfv(GLenum buffer, GLint drawbuffer,
                                           const GLfloat* values) {
    if (vkCtx_) { vkCtx_->clearBufferfv(buffer, drawbuffer, values); return; }
    glClearBufferfv(buffer, drawbuffer, values);
}
void WebGL2RenderingContext::clearBufferiv(GLenum buffer, GLint drawbuffer,
                                           const GLint* values) {
    if (vkCtx_) { vkCtx_->clearBufferiv(buffer, drawbuffer, values); return; }
    glClearBufferiv(buffer, drawbuffer, values);
}
void WebGL2RenderingContext::clearBufferuiv(GLenum buffer, GLint drawbuffer,
                                            const GLuint* values) {
    if (vkCtx_) { vkCtx_->clearBufferuiv(buffer, drawbuffer, values); return; }
    glClearBufferuiv(buffer, drawbuffer, values);
}
void WebGL2RenderingContext::clearBufferfi(GLenum buffer, GLint drawbuffer,
                                           GLfloat depth, GLint stencil) {
    if (vkCtx_) { vkCtx_->clearBufferfi(buffer, drawbuffer, depth, stencil); return; }
    glClearBufferfi(buffer, drawbuffer, depth, stencil);
}

void WebGL2RenderingContext::enable(GLenum cap) {
    if (vkCtx_) { vkCtx_->enable(cap); return; }
    switch (cap) {
        case GL_BLEND: sBlend_ = true; break;
        case GL_DEPTH_TEST: sDepthTest_ = true; break;
        case GL_CULL_FACE: sCullFace_ = true; break;
        case GL_SCISSOR_TEST: sScissorTest_ = true; break;
        case GL_STENCIL_TEST: sStencilTest_ = true; break;
        case GL_RASTERIZER_DISCARD: sRasterizerDiscard_ = true; break;
    }
    glEnable(cap);
}
void WebGL2RenderingContext::disable(GLenum cap) {
    if (vkCtx_) { vkCtx_->disable(cap); return; }
    switch (cap) {
        case GL_BLEND: sBlend_ = false; break;
        case GL_DEPTH_TEST: sDepthTest_ = false; break;
        case GL_CULL_FACE: sCullFace_ = false; break;
        case GL_SCISSOR_TEST: sScissorTest_ = false; break;
        case GL_STENCIL_TEST: sStencilTest_ = false; break;
        case GL_RASTERIZER_DISCARD: sRasterizerDiscard_ = false; break;
    }
    glDisable(cap);
}
GLboolean WebGL2RenderingContext::isEnabled(GLenum cap) {
    if (vkCtx_) return vkCtx_->isEnabled(cap);
    return glIsEnabled(cap);
}
void WebGL2RenderingContext::depthFunc(GLenum func) {
    if (vkCtx_) { vkCtx_->depthFunc(func); return; }
    sDepthFunc_ = func; glDepthFunc(func);
}
void WebGL2RenderingContext::depthMask(GLboolean flag) {
    if (vkCtx_) { vkCtx_->depthMask(flag); return; }
    sDepthMask_ = flag; glDepthMask(flag);
}
void WebGL2RenderingContext::depthRange(GLfloat zNear, GLfloat zFar) {
    if (vkCtx_) { vkCtx_->depthRange(zNear, zFar); return; }
    glDepthRange(zNear, zFar);
}
void WebGL2RenderingContext::blendFunc(GLenum s, GLenum d) {
    if (vkCtx_) { vkCtx_->blendFunc(s, d); return; }
    sBlendSrcRGB_ = s; sBlendDstRGB_ = d; sBlendSrcA_ = s; sBlendDstA_ = d;
    glBlendFunc(s, d);
}
void WebGL2RenderingContext::blendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) {
    if (vkCtx_) { vkCtx_->blendFuncSeparate(sr, dr, sa, da); return; }
    sBlendSrcRGB_ = sr; sBlendDstRGB_ = dr; sBlendSrcA_ = sa; sBlendDstA_ = da;
    glBlendFuncSeparate(sr, dr, sa, da);
}
void WebGL2RenderingContext::blendEquation(GLenum mode) {
    if (vkCtx_) { vkCtx_->blendEquation(mode); return; }
    sBlendEqRGB_ = mode; sBlendEqA_ = mode;
    glBlendEquation(mode);
}
void WebGL2RenderingContext::blendEquationSeparate(GLenum modeRGB, GLenum modeA) {
    if (vkCtx_) { vkCtx_->blendEquationSeparate(modeRGB, modeA); return; }
    sBlendEqRGB_ = modeRGB; sBlendEqA_ = modeA;
    glBlendEquationSeparate(modeRGB, modeA);
}
void WebGL2RenderingContext::blendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    if (vkCtx_) { vkCtx_->blendColor(r, g, b, a); return; }
    glBlendColor(r, g, b, a);
}
void WebGL2RenderingContext::colorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    if (vkCtx_) { vkCtx_->colorMask(r, g, b, a); return; }
    sColorMask_[0] = r; sColorMask_[1] = g; sColorMask_[2] = b; sColorMask_[3] = a;
    glColorMask(r, g, b, a);
}
void WebGL2RenderingContext::stencilFunc(GLenum f, GLint r, GLuint m) { glStencilFunc(f, r, m); }
void WebGL2RenderingContext::stencilFuncSeparate(GLenum face, GLenum f, GLint r, GLuint m) { glStencilFuncSeparate(face, f, r, m); }
void WebGL2RenderingContext::stencilOp(GLenum f, GLenum zf, GLenum zp) { glStencilOp(f, zf, zp); }
void WebGL2RenderingContext::stencilOpSeparate(GLenum face, GLenum f, GLenum zf, GLenum zp) { glStencilOpSeparate(face, f, zf, zp); }
void WebGL2RenderingContext::stencilMask(GLuint m) { glStencilMask(m); }
void WebGL2RenderingContext::stencilMaskSeparate(GLenum face, GLuint m) { glStencilMaskSeparate(face, m); }
void WebGL2RenderingContext::cullFace(GLenum mode) {
    if (vkCtx_) { vkCtx_->cullFace(mode); return; }
    sCullMode_ = mode; glCullFace(mode);
}
void WebGL2RenderingContext::frontFace(GLenum mode) {
    if (vkCtx_) { vkCtx_->frontFace(mode); return; }
    sFrontFace_ = mode; glFrontFace(mode);
}
void WebGL2RenderingContext::polygonOffset(GLfloat factor, GLfloat units) {
    if (vkCtx_) { vkCtx_->polygonOffset(factor, units); return; }
    glPolygonOffset(factor, units);
}
void WebGL2RenderingContext::lineWidth(GLfloat width) {
    if (vkCtx_) { vkCtx_->lineWidth(width); return; }
    glLineWidth(width);
}

void WebGL2RenderingContext::pixelStorei(GLenum pname, GLint param) {
    switch (pname) {
        case GL_UNPACK_ALIGNMENT: unpackAlignment_ = param; glPixelStorei(pname, param); break;
        case GL_PACK_ALIGNMENT:   packAlignment_ = param;   glPixelStorei(pname, param); break;
        // WebGL-specific (not real GL enums — must not reach glPixelStorei)
        case 0x9240: unpackFlipY_ = param ? GL_TRUE : GL_FALSE; break;              // UNPACK_FLIP_Y_WEBGL
        case 0x9241: unpackPremultiplyAlpha_ = param ? GL_TRUE : GL_FALSE; break;    // UNPACK_PREMULTIPLY_ALPHA_WEBGL
        case 0x9243: unpackColorspace_ = param; break;                               // UNPACK_COLORSPACE_CONVERSION_WEBGL
        default: glPixelStorei(pname, param); break;
    }
}

GLenum WebGL2RenderingContext::getError() {
    if (syntheticError_ != GL_NO_ERROR) {
        GLenum e = syntheticError_;
        syntheticError_ = GL_NO_ERROR;
        return e;
    }
    return glGetError();
}

void WebGL2RenderingContext::setSyntheticError(GLenum err) {
    if (syntheticError_ == GL_NO_ERROR) syntheticError_ = err;
}

// ===========================================================================
// Buffers
// ===========================================================================

WebGLBuffer WebGL2RenderingContext::createBuffer() {
    if (vkCtx_) return vkCtx_->createBuffer();
    GLuint id = 0;
    glGenBuffers(1, &id);
    validBuffers_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteBuffer(WebGLBuffer buf) {
    if (vkCtx_) { vkCtx_->deleteBuffer(buf); return; }
    if (buf.id && validBuffers_.erase(buf.id)) {
        // GL unbinds a deleted buffer from the context; a deleted name must
        // never be re-bound from shadow state (INVALID_OPERATION).
        if (sArrayBuf_ == buf.id) sArrayBuf_ = 0;
        if (sElementBuf_ == buf.id) sElementBuf_ = 0;
        if (sPixelPack_ == buf.id) sPixelPack_ = 0;
        if (sPixelUnpack_ == buf.id) sPixelUnpack_ = 0;
        // Deleting implicitly unmaps, so drop the bookkeeping too — otherwise
        // a recycled id would look permanently mapped.
        mappedBuffers_.erase(buf.id);
        glDeleteBuffers(1, &buf.id);
    }
}

void WebGL2RenderingContext::bindBuffer(GLenum target, WebGLBuffer buf) {
    if (vkCtx_) { vkCtx_->bindBuffer(target, buf); return; }
    if (target == GL_ARRAY_BUFFER) sArrayBuf_ = buf.id;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) sElementBuf_ = buf.id;
    else if (target == GL_PIXEL_PACK_BUFFER) sPixelPack_ = buf.id;
    else if (target == GL_PIXEL_UNPACK_BUFFER) sPixelUnpack_ = buf.id;
    glBindBuffer(target, buf.id);
}

void WebGL2RenderingContext::bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    if (vkCtx_) { vkCtx_->bufferData(target, size, data, usage); return; }
    glBufferData(target, size, data, usage);
}

void WebGL2RenderingContext::bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    if (vkCtx_) { vkCtx_->bufferSubData(target, offset, size, data); return; }
    glBufferSubData(target, offset, size, data);
}

void WebGL2RenderingContext::copyBufferSubData(GLenum readTarget, GLenum writeTarget,
                                                GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size) {
    glCopyBufferSubData(readTarget, writeTarget, readOffset, writeOffset, size);
}

void WebGL2RenderingContext::getBufferSubData(GLenum target, GLintptr srcByteOffset, void* dstData, GLsizeiptr length) {
    if (vkCtx_) { vkCtx_->getBufferSubData(target, srcByteOffset, dstData, length); return; }
    glGetBufferSubData(target, srcByteOffset, length, dstData);
}

// --- Buffer mapping (BRO_buffer_map) ---

GLuint WebGL2RenderingContext::boundBuffer(GLenum target) {
    if (vkCtx_) return vkCtx_->boundBuffer(target);
    GLenum pname;
    switch (target) {
        case GL_ARRAY_BUFFER:              pname = GL_ARRAY_BUFFER_BINDING; break;
        case GL_ELEMENT_ARRAY_BUFFER:      pname = GL_ELEMENT_ARRAY_BUFFER_BINDING; break;
        case GL_PIXEL_PACK_BUFFER:         pname = GL_PIXEL_PACK_BUFFER_BINDING; break;
        case GL_PIXEL_UNPACK_BUFFER:       pname = GL_PIXEL_UNPACK_BUFFER_BINDING; break;
        case GL_UNIFORM_BUFFER:            pname = GL_UNIFORM_BUFFER_BINDING; break;
        case GL_TRANSFORM_FEEDBACK_BUFFER: pname = GL_TRANSFORM_FEEDBACK_BUFFER_BINDING; break;
        // GL_COPY_{READ,WRITE}_BUFFER_BINDING share their target's value and
        // are not in glad's 3.3 core header; the target enum queries the same
        // state.
        case GL_COPY_READ_BUFFER:          pname = GL_COPY_READ_BUFFER; break;
        case GL_COPY_WRITE_BUFFER:         pname = GL_COPY_WRITE_BUFFER; break;
        default: return 0;
    }
    GLint id = 0;
    glGetIntegerv(pname, &id);
    return (GLuint)id;
}

void* WebGL2RenderingContext::mapBufferRange(GLenum target, GLintptr offset,
                                             GLsizeiptr length, GLbitfield access) {
    if (vkCtx_) return vkCtx_->mapBufferRange(target, offset, length, access);
    GLuint id = boundBuffer(target);
    if (!id) { // unknown target, or nothing bound to it
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    if (mappedBuffers_.count(id)) { // GL forbids mapping an already-mapped buffer
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    if (offset < 0 || length <= 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return nullptr;
    }
    constexpr GLbitfield kKnownBits =
        GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT |
        GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT;
    if ((access & ~kKnownBits) || !(access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT))) {
        setSyntheticError(GL_INVALID_VALUE);
        return nullptr;
    }
    // Per glMapBufferRange: READ rules out the discard/flush bits (they all
    // describe what happens to writes), and FLUSH_EXPLICIT needs WRITE.
    // UNSYNCHRONIZED is deliberately not in that set — it is legal with READ.
    if (((access & GL_MAP_READ_BIT) &&
         (access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT |
                    GL_MAP_FLUSH_EXPLICIT_BIT))) ||
        ((access & GL_MAP_FLUSH_EXPLICIT_BIT) && !(access & GL_MAP_WRITE_BIT))) {
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    void* ptr = glMapBufferRange(target, offset, length, access);
    if (ptr) mappedBuffers_[id] = ptr;
    return ptr;
}

bool WebGL2RenderingContext::unmapBuffer(GLenum target) {
    if (vkCtx_) return vkCtx_->unmapBuffer(target);
    GLuint id = boundBuffer(target);
    auto it = mappedBuffers_.find(id);
    if (!id || it == mappedBuffers_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    mappedBuffers_.erase(it);
    return glUnmapBuffer(target) == GL_TRUE;
}

void WebGL2RenderingContext::flushMappedBufferRange(GLenum target, GLintptr offset,
                                                    GLsizeiptr length) {
    if (vkCtx_) { vkCtx_->flushMappedBufferRange(target, offset, length); return; }
    if (!mappedBuffers_.count(boundBuffer(target))) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    glFlushMappedBufferRange(target, offset, length);
}

void WebGL2RenderingContext::bindBufferBase(GLenum target, GLuint index, WebGLBuffer buf) {
    glBindBufferBase(target, index, buf.id);
}

void WebGL2RenderingContext::bindBufferRange(GLenum target, GLuint index, WebGLBuffer buf,
                                              GLintptr offset, GLsizeiptr size) {
    glBindBufferRange(target, index, buf.id, offset, size);
}

// ===========================================================================
// Sampler objects
// ===========================================================================

WebGLSampler WebGL2RenderingContext::createSampler() {
    GLuint id = 0;
    glGenSamplers(1, &id);
    validSamplers_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteSampler(WebGLSampler s) {
    if (s.id && validSamplers_.erase(s.id)) {
        // GL auto-unbinds a deleted sampler from every unit it is bound to;
        // mirror that in the shadow state so restoreState never rebinds a
        // dead name.
        for (auto& slot : sSampler_)
            if (slot == s.id) slot = 0;
        glDeleteSamplers(1, &s.id);
    }
}

void WebGL2RenderingContext::bindSampler(GLuint unit, WebGLSampler s) {
    if (unit < 32) sSampler_[unit] = s.id;
    glBindSampler(unit, s.id);
}

void WebGL2RenderingContext::samplerParameteri(WebGLSampler s, GLenum pname, GLint param) {
    glSamplerParameteri(s.id, pname, param);
}

void WebGL2RenderingContext::samplerParameterf(WebGLSampler s, GLenum pname, GLfloat param) {
    glSamplerParameterf(s.id, pname, param);
}

GLint WebGL2RenderingContext::getSamplerParameteri(WebGLSampler s, GLenum pname) {
    GLint v = 0;
    glGetSamplerParameteriv(s.id, pname, &v);
    return v;
}

GLfloat WebGL2RenderingContext::getSamplerParameterf(WebGLSampler s, GLenum pname) {
    GLfloat v = 0;
    glGetSamplerParameterfv(s.id, pname, &v);
    return v;
}

GLboolean WebGL2RenderingContext::isSampler(WebGLSampler s) {
    if (!s.id || !validSamplers_.count(s.id)) return GL_FALSE;
    return glIsSampler(s.id);
}

// ===========================================================================
// Sync objects
// ===========================================================================

WebGLSync WebGL2RenderingContext::fenceSync(GLenum condition, GLbitfield flags) {
    GLsync sync = glFenceSync(condition, flags);
    if (sync) validSyncs_.insert(sync);
    return {sync};
}

void WebGL2RenderingContext::deleteSync(WebGLSync s) {
    if (s.sync && validSyncs_.erase(s.sync)) {
        glDeleteSync(s.sync);
    }
}

GLenum WebGL2RenderingContext::clientWaitSync(WebGLSync s, GLbitfield flags, double timeoutNs) {
    if (!s.sync || !validSyncs_.count(s.sync)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return 0x911D; // WAIT_FAILED
    }
    // WebGL2: timeouts above MAX_CLIENT_WAIT_TIMEOUT_WEBGL are an error, not
    // an unbounded block of the JS thread.
    if (timeoutNs < 0 || timeoutNs > kMaxClientWaitTimeoutNs) {
        setSyntheticError(timeoutNs < 0 ? GL_INVALID_VALUE : GL_INVALID_OPERATION);
        return 0x911D; // WAIT_FAILED
    }
    return glClientWaitSync(s.sync, flags, (GLuint64)timeoutNs);
}

void WebGL2RenderingContext::waitSync(WebGLSync s, GLbitfield flags, double timeoutNs) {
    if (!s.sync || !validSyncs_.count(s.sync)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    // WebGL2 mandates flags == 0 and timeout == TIMEOUT_IGNORED (-1).
    if (flags != 0 || timeoutNs != -1.0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    glWaitSync(s.sync, 0, GL_TIMEOUT_IGNORED);
}

GLint WebGL2RenderingContext::getSyncParameter(WebGLSync s, GLenum pname) {
    if (!s.sync || !validSyncs_.count(s.sync)) return 0;
    GLint v = 0;
    GLsizei len = 0;
    glGetSynciv(s.sync, pname, 1, &len, &v);
    return v;
}

GLboolean WebGL2RenderingContext::isSync(WebGLSync s) {
    if (!s.sync || !validSyncs_.count(s.sync)) return GL_FALSE;
    return glIsSync(s.sync);
}

// ===========================================================================
// Query objects
// ===========================================================================

// ANY_SAMPLES_PASSED_CONSERVATIVE is GL 4.3 / ARB_ES3_compatibility (glad
// here is generated for 3.3 core + extensions); without the extension answer
// it with the exact ANY_SAMPLES_PASSED query (an exact answer is a valid
// conservative one).
static GLenum mapQueryTarget(GLenum target) {
    if (target == 0x8D6A /* ANY_SAMPLES_PASSED_CONSERVATIVE */ &&
        !GLAD_GL_ARB_ES3_compatibility) {
        return 0x8C2F; // ANY_SAMPLES_PASSED
    }
    return target;
}

WebGLQuery WebGL2RenderingContext::createQuery() {
    GLuint id = 0;
    glGenQueries(1, &id);
    validQueries_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteQuery(WebGLQuery q) {
    if (q.id && validQueries_.erase(q.id)) {
        glDeleteQueries(1, &q.id);
    }
}

void WebGL2RenderingContext::beginQuery(GLenum target, WebGLQuery q) {
    if (!q.id || !validQueries_.count(q.id)) {
        // WebGL2: beginQuery with a deleted/invalid query object.
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    glBeginQuery(mapQueryTarget(target), q.id);
}

void WebGL2RenderingContext::endQuery(GLenum target) {
    glEndQuery(mapQueryTarget(target));
}

GLuint WebGL2RenderingContext::getQueryParameteru(WebGLQuery q, GLenum pname) {
    if (!q.id || !validQueries_.count(q.id)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return 0;
    }
    // QUERY_RESULT_AVAILABLE never stalls; QUERY_RESULT is only meaningful
    // once available (WebGL apps must poll availability first).
    GLuint v = 0;
    glGetQueryObjectuiv(q.id, pname, &v);
    return v;
}

GLboolean WebGL2RenderingContext::isQuery(WebGLQuery q) {
    if (!q.id || !validQueries_.count(q.id)) return GL_FALSE;
    return glIsQuery(q.id);
}

// ===========================================================================
// Transform feedback
// ===========================================================================

bool WebGL2RenderingContext::transformFeedbackObjectsSupported() const {
    return GLAD_GL_ARB_transform_feedback2 && glad_glGenTransformFeedbacks != nullptr;
}

WebGLTransformFeedback WebGL2RenderingContext::createTransformFeedback() {
    if (!transformFeedbackObjectsSupported()) return {0};
    GLuint id = 0;
    glGenTransformFeedbacks(1, &id);
    validTransformFeedbacks_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteTransformFeedback(WebGLTransformFeedback tf) {
    if (tf.id && validTransformFeedbacks_.erase(tf.id)) {
        // GL reverts to the default TF object if the deleted one was bound.
        if (sTransformFeedback_ == tf.id) sTransformFeedback_ = 0;
        glDeleteTransformFeedbacks(1, &tf.id);
    }
}

void WebGL2RenderingContext::bindTransformFeedback(GLenum target, WebGLTransformFeedback tf) {
    if (!transformFeedbackObjectsSupported()) {
        if (tf.id) setSyntheticError(GL_INVALID_OPERATION);
        return; // default TF object is implicitly bound
    }
    sTransformFeedback_ = tf.id;
    glBindTransformFeedback(target, tf.id);
}

void WebGL2RenderingContext::beginTransformFeedback(GLenum primitiveMode) {
    glBeginTransformFeedback(primitiveMode);
    tfActive_ = true;
    tfPaused_ = false;
}

void WebGL2RenderingContext::endTransformFeedback() {
    glEndTransformFeedback();
    tfActive_ = false;
    tfPaused_ = false;
}

void WebGL2RenderingContext::pauseTransformFeedback() {
    if (!transformFeedbackObjectsSupported()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    glPauseTransformFeedback();
    if (tfActive_) tfPaused_ = true;
}

void WebGL2RenderingContext::resumeTransformFeedback() {
    if (!transformFeedbackObjectsSupported()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    glResumeTransformFeedback();
    if (tfActive_) tfPaused_ = false;
}

void WebGL2RenderingContext::transformFeedbackVaryings(WebGLProgram program,
                                                       const std::vector<std::string>& varyings,
                                                       GLenum bufferMode) {
    std::vector<const char*> ptrs(varyings.size());
    for (size_t i = 0; i < varyings.size(); i++) ptrs[i] = varyings[i].c_str();
    glTransformFeedbackVaryings(program.id, (GLsizei)ptrs.size(),
                                ptrs.empty() ? nullptr : ptrs.data(), bufferMode);
}

WebGLActiveInfo WebGL2RenderingContext::getTransformFeedbackVarying(WebGLProgram program, GLuint index) {
    char name[256];
    GLsizei len = 0;
    GLsizei size = 0;
    GLenum type = 0;
    glGetTransformFeedbackVarying(program.id, index, sizeof(name), &len, &size, &type, name);
    return {std::string(name, len), type, (GLint)size};
}

GLboolean WebGL2RenderingContext::isTransformFeedback(WebGLTransformFeedback tf) {
    if (!tf.id || !validTransformFeedbacks_.count(tf.id)) return GL_FALSE;
    return glIsTransformFeedback(tf.id);
}

int64_t WebGL2RenderingContext::getIndexedParameterInt64(GLenum pname, GLuint index) {
    GLint64 v = 0;
    glGetInteger64i_v(pname, index, &v);
    return (int64_t)v;
}


} // namespace bro::webgl
