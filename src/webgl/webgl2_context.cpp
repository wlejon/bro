#include "webgl/webgl2_context.h"
#include "webgl/glsl_translator.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl {

// ===========================================================================
// Construction / destruction
// ===========================================================================

WebGL2RenderingContext::WebGL2RenderingContext(int width, int height)
    : width_(width), height_(height) {
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
    glGenFramebuffers(1, &canvasFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, canvasFBO_);

    // Color attachment (RGBA8)
    glGenTextures(1, &colorTex_);
    glBindTexture(GL_TEXTURE_2D, colorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex_, 0);

    // Depth-stencil renderbuffer
    glGenRenderbuffers(1, &depthStencilRBO_);
    glBindRenderbuffer(GL_RENDERBUFFER, depthStencilRBO_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width_, height_);
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
    // restoreState() re-applies every piece of shadow state this context
    // tracks, including glBindFramebuffer(sFBO_) — which is exactly what
    // makes a second canvas draw into its own FBO rather than the first's.
    restoreState();
}

void WebGL2RenderingContext::bindCanvasFBO() {
    glBindFramebuffer(GL_FRAMEBUFFER, canvasFBO_);
}

void WebGL2RenderingContext::unbindCanvasFBO() {
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

// ===========================================================================
// State
// ===========================================================================

void WebGL2RenderingContext::viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    sViewport_[0] = x; sViewport_[1] = y; sViewport_[2] = w; sViewport_[3] = h;
    glViewport(x, y, w, h);
}
void WebGL2RenderingContext::scissor(GLint x, GLint y, GLsizei w, GLsizei h) { glScissor(x, y, w, h); }
void WebGL2RenderingContext::clearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    sClearR_ = r; sClearG_ = g; sClearB_ = b; sClearA_ = a;
    glClearColor(r, g, b, a);
}
void WebGL2RenderingContext::clearDepth(GLfloat depth) { glClearDepth(depth); }
void WebGL2RenderingContext::clearStencil(GLint s) { glClearStencil(s); }
void WebGL2RenderingContext::clear(GLbitfield mask) { glClear(mask); }

void WebGL2RenderingContext::enable(GLenum cap) {
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
GLboolean WebGL2RenderingContext::isEnabled(GLenum cap) { return glIsEnabled(cap); }
void WebGL2RenderingContext::depthFunc(GLenum func) { sDepthFunc_ = func; glDepthFunc(func); }
void WebGL2RenderingContext::depthMask(GLboolean flag) { sDepthMask_ = flag; glDepthMask(flag); }
void WebGL2RenderingContext::depthRange(GLfloat zNear, GLfloat zFar) { glDepthRange(zNear, zFar); }
void WebGL2RenderingContext::blendFunc(GLenum s, GLenum d) {
    sBlendSrcRGB_ = s; sBlendDstRGB_ = d; sBlendSrcA_ = s; sBlendDstA_ = d;
    glBlendFunc(s, d);
}
void WebGL2RenderingContext::blendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) {
    sBlendSrcRGB_ = sr; sBlendDstRGB_ = dr; sBlendSrcA_ = sa; sBlendDstA_ = da;
    glBlendFuncSeparate(sr, dr, sa, da);
}
void WebGL2RenderingContext::blendEquation(GLenum mode) {
    sBlendEqRGB_ = mode; sBlendEqA_ = mode;
    glBlendEquation(mode);
}
void WebGL2RenderingContext::blendEquationSeparate(GLenum modeRGB, GLenum modeA) {
    sBlendEqRGB_ = modeRGB; sBlendEqA_ = modeA;
    glBlendEquationSeparate(modeRGB, modeA);
}
void WebGL2RenderingContext::blendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { glBlendColor(r, g, b, a); }
void WebGL2RenderingContext::colorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    sColorMask_[0] = r; sColorMask_[1] = g; sColorMask_[2] = b; sColorMask_[3] = a;
    glColorMask(r, g, b, a);
}
void WebGL2RenderingContext::stencilFunc(GLenum f, GLint r, GLuint m) { glStencilFunc(f, r, m); }
void WebGL2RenderingContext::stencilFuncSeparate(GLenum face, GLenum f, GLint r, GLuint m) { glStencilFuncSeparate(face, f, r, m); }
void WebGL2RenderingContext::stencilOp(GLenum f, GLenum zf, GLenum zp) { glStencilOp(f, zf, zp); }
void WebGL2RenderingContext::stencilOpSeparate(GLenum face, GLenum f, GLenum zf, GLenum zp) { glStencilOpSeparate(face, f, zf, zp); }
void WebGL2RenderingContext::stencilMask(GLuint m) { glStencilMask(m); }
void WebGL2RenderingContext::stencilMaskSeparate(GLenum face, GLuint m) { glStencilMaskSeparate(face, m); }
void WebGL2RenderingContext::cullFace(GLenum mode) { sCullMode_ = mode; glCullFace(mode); }
void WebGL2RenderingContext::frontFace(GLenum mode) { sFrontFace_ = mode; glFrontFace(mode); }
void WebGL2RenderingContext::polygonOffset(GLfloat factor, GLfloat units) { glPolygonOffset(factor, units); }
void WebGL2RenderingContext::lineWidth(GLfloat width) { glLineWidth(width); }

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
    GLuint id = 0;
    glGenBuffers(1, &id);
    validBuffers_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteBuffer(WebGLBuffer buf) {
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
    if (target == GL_ARRAY_BUFFER) sArrayBuf_ = buf.id;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) sElementBuf_ = buf.id;
    else if (target == GL_PIXEL_PACK_BUFFER) sPixelPack_ = buf.id;
    else if (target == GL_PIXEL_UNPACK_BUFFER) sPixelUnpack_ = buf.id;
    glBindBuffer(target, buf.id);
}

void WebGL2RenderingContext::bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    glBufferData(target, size, data, usage);
}

void WebGL2RenderingContext::bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    glBufferSubData(target, offset, size, data);
}

void WebGL2RenderingContext::copyBufferSubData(GLenum readTarget, GLenum writeTarget,
                                                GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size) {
    glCopyBufferSubData(readTarget, writeTarget, readOffset, writeOffset, size);
}

void WebGL2RenderingContext::getBufferSubData(GLenum target, GLintptr srcByteOffset, void* dstData, GLsizeiptr length) {
    glGetBufferSubData(target, srcByteOffset, length, dstData);
}

// --- Buffer mapping (BRO_buffer_map) ---

GLuint WebGL2RenderingContext::boundBuffer(GLenum target) {
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

// ===========================================================================
// VAO
// ===========================================================================

WebGLVertexArrayObject WebGL2RenderingContext::createVertexArray() {
    GLuint id = 0;
    glGenVertexArrays(1, &id);
    validVAOs_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteVertexArray(WebGLVertexArrayObject vao) {
    if (vao.id && validVAOs_.erase(vao.id)) {
        if (sVAO_ == vao.id) sVAO_ = 0; // GL reverts to the default VAO
        glDeleteVertexArrays(1, &vao.id);
    }
}

void WebGL2RenderingContext::bindVertexArray(WebGLVertexArrayObject vao) {
    sVAO_ = vao.id;
    glBindVertexArray(vao.id);
}

// ===========================================================================
// Vertex attributes
// ===========================================================================

void WebGL2RenderingContext::vertexAttribPointer(GLuint index, GLint size, GLenum type,
                                                  GLboolean normalized, GLsizei stride, GLintptr offset) {
    glVertexAttribPointer(index, size, type, normalized, stride, (const void*)offset);
}

void WebGL2RenderingContext::vertexAttribIPointer(GLuint index, GLint size, GLenum type,
                                                   GLsizei stride, GLintptr offset) {
    glVertexAttribIPointer(index, size, type, stride, (const void*)offset);
}

void WebGL2RenderingContext::enableVertexAttribArray(GLuint index) { glEnableVertexAttribArray(index); }
void WebGL2RenderingContext::disableVertexAttribArray(GLuint index) { glDisableVertexAttribArray(index); }
void WebGL2RenderingContext::vertexAttribDivisor(GLuint index, GLuint divisor) { glVertexAttribDivisor(index, divisor); }
void WebGL2RenderingContext::vertexAttribI4i(GLuint index, GLint x, GLint y, GLint z, GLint w) { glVertexAttribI4i(index, x, y, z, w); }
void WebGL2RenderingContext::vertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w) { glVertexAttribI4ui(index, x, y, z, w); }
void WebGL2RenderingContext::vertexAttribI4iv(GLuint index, const GLint* v) { glVertexAttribI4iv(index, v); }
void WebGL2RenderingContext::vertexAttribI4uiv(GLuint index, const GLuint* v) { glVertexAttribI4uiv(index, v); }

// ===========================================================================
// Shaders
// ===========================================================================

WebGLShader WebGL2RenderingContext::createShader(GLenum type) {
    GLuint id = glCreateShader(type);
    validShaders_.insert(id);
    return {id, type};
}

void WebGL2RenderingContext::deleteShader(WebGLShader shader) {
    if (shader.id && validShaders_.erase(shader.id)) {
        glDeleteShader(shader.id);
    }
}

void WebGL2RenderingContext::shaderSource(WebGLShader shader, const std::string& source) {
    // Translate GLSL ES 3.00 → GLSL 3.30
    std::string translated = translateGLSL(source, shader.type);
    const char* src = translated.c_str();
    glShaderSource(shader.id, 1, &src, nullptr);
}

void WebGL2RenderingContext::compileShader(WebGLShader shader) {
    glCompileShader(shader.id);
}

GLboolean WebGL2RenderingContext::getShaderParameter_compileStatus(WebGLShader shader) {
    GLint ok = 0;
    glGetShaderiv(shader.id, GL_COMPILE_STATUS, &ok);
    return ok ? GL_TRUE : GL_FALSE;
}

std::string WebGL2RenderingContext::getShaderInfoLog(WebGLShader shader) {
    GLint len = 0;
    glGetShaderiv(shader.id, GL_INFO_LOG_LENGTH, &len);
    if (len <= 0) return "";
    std::string log(len, '\0');
    glGetShaderInfoLog(shader.id, len, nullptr, log.data());
    // Trim trailing null
    while (!log.empty() && log.back() == '\0') log.pop_back();
    return log;
}

// ===========================================================================
// Programs
// ===========================================================================

WebGLProgram WebGL2RenderingContext::createProgram() {
    GLuint id = glCreateProgram();
    validPrograms_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteProgram(WebGLProgram program) {
    if (program.id && validPrograms_.erase(program.id)) {
        glDeleteProgram(program.id);
    }
}

void WebGL2RenderingContext::attachShader(WebGLProgram program, WebGLShader shader) {
    glAttachShader(program.id, shader.id);
}

void WebGL2RenderingContext::detachShader(WebGLProgram program, WebGLShader shader) {
    glDetachShader(program.id, shader.id);
}

void WebGL2RenderingContext::linkProgram(WebGLProgram program) {
    glLinkProgram(program.id);
}

void WebGL2RenderingContext::useProgram(WebGLProgram program) {
    sProgram_ = program.id;
    glUseProgram(program.id);
}

GLboolean WebGL2RenderingContext::getProgramParameter_linkStatus(WebGLProgram program) {
    GLint ok = 0;
    glGetProgramiv(program.id, GL_LINK_STATUS, &ok);
    return ok ? GL_TRUE : GL_FALSE;
}

std::string WebGL2RenderingContext::getProgramInfoLog(WebGLProgram program) {
    GLint len = 0;
    glGetProgramiv(program.id, GL_INFO_LOG_LENGTH, &len);
    if (len <= 0) return "";
    std::string log(len, '\0');
    glGetProgramInfoLog(program.id, len, nullptr, log.data());
    while (!log.empty() && log.back() == '\0') log.pop_back();
    return log;
}

void WebGL2RenderingContext::bindAttribLocation(WebGLProgram program, GLuint index, const std::string& name) {
    glBindAttribLocation(program.id, index, name.c_str());
}

GLint WebGL2RenderingContext::getAttribLocation(WebGLProgram program, const std::string& name) {
    return glGetAttribLocation(program.id, name.c_str());
}

GLint WebGL2RenderingContext::getFragDataLocation(WebGLProgram program, const std::string& name) {
    return glGetFragDataLocation(program.id, name.c_str());
}

WebGLUniformLocation WebGL2RenderingContext::getUniformLocation(WebGLProgram program, const std::string& name) {
    GLint loc = glGetUniformLocation(program.id, name.c_str());
    return {loc, program.id};
}

WebGLActiveInfo WebGL2RenderingContext::getActiveAttrib(WebGLProgram program, GLuint index) {
    char name[256];
    GLsizei len = 0;
    GLint size = 0;
    GLenum type = 0;
    glGetActiveAttrib(program.id, index, sizeof(name), &len, &size, &type, name);
    return {std::string(name, len), type, size};
}

WebGLActiveInfo WebGL2RenderingContext::getActiveUniform(WebGLProgram program, GLuint index) {
    char name[256];
    GLsizei len = 0;
    GLint size = 0;
    GLenum type = 0;
    glGetActiveUniform(program.id, index, sizeof(name), &len, &size, &type, name);
    return {std::string(name, len), type, size};
}

GLint WebGL2RenderingContext::getProgramParameter_int(WebGLProgram program, GLenum pname) {
    GLint val = 0;
    glGetProgramiv(program.id, pname, &val);
    return val;
}

GLuint WebGL2RenderingContext::getUniformBlockIndex(WebGLProgram program, const std::string& name) {
    return glGetUniformBlockIndex(program.id, name.c_str());
}

void WebGL2RenderingContext::uniformBlockBinding(WebGLProgram program, GLuint blockIndex, GLuint blockBinding) {
    glUniformBlockBinding(program.id, blockIndex, blockBinding);
}

std::vector<GLuint> WebGL2RenderingContext::getUniformIndices(WebGLProgram program,
                                                              const std::vector<std::string>& names) {
    std::vector<const char*> ptrs(names.size());
    for (size_t i = 0; i < names.size(); i++) ptrs[i] = names[i].c_str();
    std::vector<GLuint> indices(names.size(), GL_INVALID_INDEX);
    if (!ptrs.empty()) {
        glGetUniformIndices(program.id, (GLsizei)ptrs.size(), ptrs.data(), indices.data());
    }
    return indices;
}

std::vector<GLint> WebGL2RenderingContext::getActiveUniforms(WebGLProgram program,
                                                             const std::vector<GLuint>& indices,
                                                             GLenum pname) {
    std::vector<GLint> params(indices.size(), 0);
    if (!indices.empty()) {
        glGetActiveUniformsiv(program.id, (GLsizei)indices.size(), indices.data(),
                              pname, params.data());
    }
    return params;
}

GLint WebGL2RenderingContext::getActiveUniformBlockParameteri(WebGLProgram program,
                                                              GLuint blockIndex, GLenum pname) {
    GLint v = 0;
    glGetActiveUniformBlockiv(program.id, blockIndex, pname, &v);
    return v;
}

std::vector<GLint> WebGL2RenderingContext::getActiveUniformBlockIndices(WebGLProgram program,
                                                                        GLuint blockIndex) {
    GLint count = 0;
    glGetActiveUniformBlockiv(program.id, blockIndex,
                              GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &count);
    if (count <= 0) return {};
    std::vector<GLint> indices(count, 0);
    glGetActiveUniformBlockiv(program.id, blockIndex,
                              GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, indices.data());
    return indices;
}

std::string WebGL2RenderingContext::getActiveUniformBlockName(WebGLProgram program,
                                                              GLuint blockIndex) {
    char name[256];
    GLsizei len = 0;
    glGetActiveUniformBlockName(program.id, blockIndex, sizeof(name), &len, name);
    return std::string(name, len);
}

// ===========================================================================
// Uniforms
// ===========================================================================

void WebGL2RenderingContext::uniform1f(WebGLUniformLocation loc, GLfloat x) { glUniform1f(loc.location, x); }
void WebGL2RenderingContext::uniform2f(WebGLUniformLocation loc, GLfloat x, GLfloat y) { glUniform2f(loc.location, x, y); }
void WebGL2RenderingContext::uniform3f(WebGLUniformLocation loc, GLfloat x, GLfloat y, GLfloat z) { glUniform3f(loc.location, x, y, z); }
void WebGL2RenderingContext::uniform4f(WebGLUniformLocation loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { glUniform4f(loc.location, x, y, z, w); }
void WebGL2RenderingContext::uniform1i(WebGLUniformLocation loc, GLint x) { glUniform1i(loc.location, x); }
void WebGL2RenderingContext::uniform2i(WebGLUniformLocation loc, GLint x, GLint y) { glUniform2i(loc.location, x, y); }
void WebGL2RenderingContext::uniform3i(WebGLUniformLocation loc, GLint x, GLint y, GLint z) { glUniform3i(loc.location, x, y, z); }
void WebGL2RenderingContext::uniform4i(WebGLUniformLocation loc, GLint x, GLint y, GLint z, GLint w) { glUniform4i(loc.location, x, y, z, w); }

void WebGL2RenderingContext::uniform1ui(WebGLUniformLocation loc, GLuint x) { glUniform1ui(loc.location, x); }
void WebGL2RenderingContext::uniform2ui(WebGLUniformLocation loc, GLuint x, GLuint y) { glUniform2ui(loc.location, x, y); }
void WebGL2RenderingContext::uniform3ui(WebGLUniformLocation loc, GLuint x, GLuint y, GLuint z) { glUniform3ui(loc.location, x, y, z); }
void WebGL2RenderingContext::uniform4ui(WebGLUniformLocation loc, GLuint x, GLuint y, GLuint z, GLuint w) { glUniform4ui(loc.location, x, y, z, w); }
void WebGL2RenderingContext::uniform1uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v) { glUniform1uiv(loc.location, count, v); }
void WebGL2RenderingContext::uniform2uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v) { glUniform2uiv(loc.location, count, v); }
void WebGL2RenderingContext::uniform3uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v) { glUniform3uiv(loc.location, count, v); }
void WebGL2RenderingContext::uniform4uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v) { glUniform4uiv(loc.location, count, v); }

void WebGL2RenderingContext::uniform1fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) { glUniform1fv(loc.location, count, v); }
void WebGL2RenderingContext::uniform2fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) { glUniform2fv(loc.location, count, v); }
void WebGL2RenderingContext::uniform3fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) { glUniform3fv(loc.location, count, v); }
void WebGL2RenderingContext::uniform4fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) { glUniform4fv(loc.location, count, v); }
void WebGL2RenderingContext::uniform1iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) { glUniform1iv(loc.location, count, v); }
void WebGL2RenderingContext::uniform2iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) { glUniform2iv(loc.location, count, v); }
void WebGL2RenderingContext::uniform3iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) { glUniform3iv(loc.location, count, v); }
void WebGL2RenderingContext::uniform4iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) { glUniform4iv(loc.location, count, v); }

void WebGL2RenderingContext::uniformMatrix2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix2fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix3fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix4fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix2x3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix2x3fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix3x2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix3x2fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix2x4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix2x4fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix4x2fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix4x2fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix3x4fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix3x4fv(loc.location, count, transpose, v); }
void WebGL2RenderingContext::uniformMatrix4x3fv(WebGLUniformLocation loc, GLsizei count, GLboolean transpose, const GLfloat* v) { glUniformMatrix4x3fv(loc.location, count, transpose, v); }

// ===========================================================================
// Textures
// ===========================================================================

WebGLTexture WebGL2RenderingContext::createTexture() {
    GLuint id = 0;
    glGenTextures(1, &id);
    validTextures_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteTexture(WebGLTexture tex) {
    if (tex.id && validTextures_.erase(tex.id)) {
        for (auto& slot : sTex2D_)
            if (slot == tex.id) slot = 0; // GL auto-unbinds deleted textures
        glDeleteTextures(1, &tex.id);
    }
}

void WebGL2RenderingContext::bindTexture(GLenum target, WebGLTexture tex) {
    if (target == GL_TEXTURE_2D) {
        unsigned unit = sActiveTex_ - GL_TEXTURE0;
        if (unit < 32) sTex2D_[unit] = tex.id;
    }
    glBindTexture(target, tex.id);
}

void WebGL2RenderingContext::activeTexture(GLenum texture) {
    sActiveTex_ = texture;
    glActiveTexture(texture);
}

void WebGL2RenderingContext::texParameteri(GLenum target, GLenum pname, GLint param) {
    glTexParameteri(target, pname, param);
}

void WebGL2RenderingContext::texParameterf(GLenum target, GLenum pname, GLfloat param) {
    glTexParameterf(target, pname, param);
}

// Translate WebGL2 unsized internal formats to GL 3.3 Core sized formats
static GLint translateInternalFormat(GLint internalformat, GLenum type) {
    switch (internalformat) {
        case 0x1908: // GL_RGBA
            switch (type) {
                case GL_UNSIGNED_BYTE: return GL_RGBA8;
                case GL_FLOAT: return GL_RGBA32F;
                case GL_HALF_FLOAT: return GL_RGBA16F;
                default: return GL_RGBA8;
            }
        case 0x1907: // GL_RGB
            switch (type) {
                case GL_UNSIGNED_BYTE: return GL_RGB8;
                case GL_FLOAT: return GL_RGB32F;
                case GL_HALF_FLOAT: return GL_RGB16F;
                default: return GL_RGB8;
            }
        case 0x190A: return GL_RG8;   // GL_LUMINANCE_ALPHA → approximate
        case 0x1909: return GL_R8;    // GL_LUMINANCE → approximate
        case 0x1906: return GL_R8;    // GL_ALPHA → approximate
        case GL_RED: return (type == GL_FLOAT) ? GL_R32F : GL_R8;
        case GL_RG: return (type == GL_FLOAT) ? GL_RG32F : GL_RG8;
        case GL_DEPTH_COMPONENT:
            switch (type) {
                case GL_UNSIGNED_SHORT: return GL_DEPTH_COMPONENT16;
                case GL_UNSIGNED_INT: return GL_DEPTH_COMPONENT24;
                case GL_FLOAT: return GL_DEPTH_COMPONENT32F;
                default: return GL_DEPTH_COMPONENT24;
            }
        case GL_DEPTH_STENCIL: return GL_DEPTH24_STENCIL8;
        default: return internalformat; // Already sized (e.g. GL_RGBA8, GL_R16F)
    }
}

// Bytes per pixel for the format/type pairs we can safely transform or
// bounds-check. Returns 0 for unknown/packed-special combinations.
static int bytesPerPixel(GLenum format, GLenum type) {
    int channels = 0;
    switch (format) {
        case GL_RGBA: case 0x8D99 /*RGBA_INTEGER*/: channels = 4; break;
        case GL_RGB:  case 0x8D98 /*RGB_INTEGER*/:  channels = 3; break;
        case GL_RG:   case 0x8228 /*RG_INTEGER*/:   channels = 2; break;
        case GL_RED:  case 0x8D94 /*RED_INTEGER*/:
        case 0x1906 /*ALPHA*/: case 0x1909 /*LUMINANCE*/:
        case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX: channels = 1; break;
        case 0x190A /*LUMINANCE_ALPHA*/: channels = 2; break;
        case GL_DEPTH_STENCIL: channels = 1; break;
        default: return 0;
    }
    switch (type) {
        case GL_UNSIGNED_BYTE: case GL_BYTE: return channels;
        case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: return channels * 2;
        case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return channels * 4;
        case GL_UNSIGNED_SHORT_5_6_5:
        case GL_UNSIGNED_SHORT_4_4_4_4:
        case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
        case GL_UNSIGNED_INT_2_10_10_10_REV:
        case GL_UNSIGNED_INT_24_8:
        case GL_UNSIGNED_INT_10F_11F_11F_REV:
        case GL_UNSIGNED_INT_5_9_9_9_REV: return 4;
        default: return 0;
    }
}

const void* WebGL2RenderingContext::applyUnpackTransforms(
        const void* pixels, GLsizei width, GLsizei height,
        GLenum format, GLenum type, std::vector<uint8_t>& tmp) const {
    if (!pixels || (!unpackFlipY_ && !unpackPremultiplyAlpha_)) return pixels;
    int bpp = bytesPerPixel(format, type);
    if (bpp <= 0 || width <= 0 || height <= 0) return pixels;

    // Row stride as GL will read it (honouring UNPACK_ALIGNMENT).
    size_t row = (size_t)width * bpp;
    size_t align = unpackAlignment_ > 0 ? (size_t)unpackAlignment_ : 4;
    size_t stride = (row + align - 1) / align * align;

    tmp.resize(stride * height);
    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    for (GLsizei y = 0; y < height; y++) {
        const uint8_t* s = src + (size_t)y * stride;
        uint8_t* d = tmp.data() + (unpackFlipY_ ? (size_t)(height - 1 - y) * stride
                                                : (size_t)y * stride);
        std::memcpy(d, s, row);
    }

    // Premultiply is only defined for 8-bit RGBA uploads here; other
    // format/type combinations pass through unchanged.
    if (unpackPremultiplyAlpha_ && format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
        for (GLsizei y = 0; y < height; y++) {
            uint8_t* p = tmp.data() + (size_t)y * stride;
            for (GLsizei x = 0; x < width; x++, p += 4) {
                unsigned a = p[3];
                p[0] = (uint8_t)((p[0] * a + 127) / 255);
                p[1] = (uint8_t)((p[1] * a + 127) / 255);
                p[2] = (uint8_t)((p[2] * a + 127) / 255);
            }
        }
    }
    return tmp.data();
}

void WebGL2RenderingContext::texImage2D(GLenum target, GLint level, GLint internalformat,
                                         GLsizei width, GLsizei height, GLint border,
                                         GLenum format, GLenum type, const void* pixels) {
    if (sPixelUnpack_) {
        // WebGL2: the client-memory overload is INVALID_OPERATION while a
        // PIXEL_UNPACK buffer is bound (raw GL would misread the pointer as
        // a PBO offset). null still means "allocate, no data" — unbind the
        // PBO around the call so GL doesn't source from offset 0.
        if (pixels) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glTexImage2D(target, level, translateInternalFormat(internalformat, type),
                     width, height, border, format, type, nullptr);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sPixelUnpack_);
        return;
    }
    std::vector<uint8_t> tmp;
    pixels = applyUnpackTransforms(pixels, width, height, format, type, tmp);
    glTexImage2D(target, level, translateInternalFormat(internalformat, type),
                 width, height, border, format, type, pixels);
}

void WebGL2RenderingContext::texSubImage2D(GLenum target, GLint level,
                                            GLint xoffset, GLint yoffset,
                                            GLsizei width, GLsizei height,
                                            GLenum format, GLenum type, const void* pixels) {
    if (sPixelUnpack_ && pixels) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    std::vector<uint8_t> tmp;
    pixels = applyUnpackTransforms(pixels, width, height, format, type, tmp);
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

void WebGL2RenderingContext::texImage3D(GLenum target, GLint level, GLint internalformat,
                                         GLsizei width, GLsizei height, GLsizei depth, GLint border,
                                         GLenum format, GLenum type, const void* pixels) {
    glTexImage3D(target, level, internalformat, width, height, depth, border, format, type, pixels);
}

void WebGL2RenderingContext::texSubImage3D(GLenum target, GLint level,
                                            GLint xoffset, GLint yoffset, GLint zoffset,
                                            GLsizei width, GLsizei height, GLsizei depth,
                                            GLenum format, GLenum type, const void* pixels) {
    glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
}

void WebGL2RenderingContext::generateMipmap(GLenum target) { glGenerateMipmap(target); }

void WebGL2RenderingContext::texStorage2D(GLenum target, GLsizei levels, GLenum internalformat,
                                           GLsizei width, GLsizei height) {
    // Emulate texStorage2D with texImage2D calls. three.js calls texImage2D(1x1)
    // as a placeholder, then texStorage2D to allocate the real size. Real
    // glTexStorage2D would fail because the texture already has mutable data.
    // Using texImage2D for each level avoids the immutability conflict.
    GLenum format, type;
    switch (internalformat) {
        case GL_RGBA8: case GL_SRGB8_ALPHA8: format = GL_RGBA; type = GL_UNSIGNED_BYTE; break;
        case GL_RGB8: case GL_SRGB8: format = GL_RGB; type = GL_UNSIGNED_BYTE; break;
        case GL_R8: format = GL_RED; type = GL_UNSIGNED_BYTE; break;
        case GL_RG8: format = GL_RG; type = GL_UNSIGNED_BYTE; break;
        case GL_RGBA16F: format = GL_RGBA; type = GL_HALF_FLOAT; break;
        case GL_RGB16F: format = GL_RGB; type = GL_HALF_FLOAT; break;
        case GL_RGBA32F: format = GL_RGBA; type = GL_FLOAT; break;
        case GL_RGB32F: format = GL_RGB; type = GL_FLOAT; break;
        case GL_R16F: format = GL_RED; type = GL_HALF_FLOAT; break;
        case GL_R32F: format = GL_RED; type = GL_FLOAT; break;
        case GL_DEPTH_COMPONENT16: format = GL_DEPTH_COMPONENT; type = GL_UNSIGNED_SHORT; break;
        case GL_DEPTH_COMPONENT24: format = GL_DEPTH_COMPONENT; type = GL_UNSIGNED_INT; break;
        case GL_DEPTH_COMPONENT32F: format = GL_DEPTH_COMPONENT; type = GL_FLOAT; break;
        case GL_DEPTH24_STENCIL8: format = GL_DEPTH_STENCIL; type = GL_UNSIGNED_INT_24_8; break;
        default: format = GL_RGBA; type = GL_UNSIGNED_BYTE; break;
    }

    if (target == GL_TEXTURE_CUBE_MAP) {
        for (GLsizei i = 0; i < levels; i++) {
            GLsizei w = std::max(1, width >> i), h = std::max(1, height >> i);
            for (int face = 0; face < 6; face++) {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, i,
                             internalformat, w, h, 0, format, type, nullptr);
            }
        }
    } else {
        for (GLsizei i = 0; i < levels; i++) {
            GLsizei w = std::max(1, width >> i), h = std::max(1, height >> i);
            glTexImage2D(target, i, internalformat, w, h, 0, format, type, nullptr);
        }
    }
}

void WebGL2RenderingContext::texStorage3D(GLenum target, GLsizei levels, GLenum internalformat,
                                           GLsizei width, GLsizei height, GLsizei depth) {
    glTexStorage3D(target, levels, internalformat, width, height, depth);
}

// ===========================================================================
// Compressed textures
// ===========================================================================

// All exposed families (S3TC, RGTC, BPTC) use 4x4 blocks; DXT1 and RGTC1 are
// 8 bytes per block, everything else 16. Returns 0 for unknown formats.
static int compressedBlockBytes(GLenum format) {
    switch (format) {
        case 0x83F0: case 0x83F1: // DXT1 / DXT1a
        case 0x8C4C: case 0x8C4D: // sRGB DXT1 variants
        case 0x8DBB: case 0x8DBC: // RGTC1 / signed
            return 8;
        case 0x83F2: case 0x83F3: // DXT3 / DXT5
        case 0x8C4E: case 0x8C4F: // sRGB DXT3 / DXT5
        case 0x8DBD: case 0x8DBE: // RGTC2 / signed
        case 0x8E8C: case 0x8E8D: case 0x8E8E: case 0x8E8F: // BPTC
            return 16;
        default: return 0;
    }
}

bool WebGL2RenderingContext::isCompressedFormatSupported(GLenum format) const {
    return std::find(compressedFormats_.begin(), compressedFormats_.end(),
                     (GLint)format) != compressedFormats_.end();
}

void WebGL2RenderingContext::compressedTexImage2D(GLenum target, GLint level,
                                                   GLenum internalformat,
                                                   GLsizei width, GLsizei height, GLint border,
                                                   const void* data, size_t dataLen) {
    if (!isCompressedFormatSupported(internalformat)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    // Block-size math per the WebGL compressed-texture extension specs: the
    // source must be exactly the block payload, or nothing is uploaded (this
    // is also the no-overread guard for the client memory).
    size_t expected = (size_t)((width + 3) / 4) * ((height + 3) / 4) *
                      compressedBlockBytes(internalformat);
    if (dataLen != expected) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    glCompressedTexImage2D(target, level, internalformat, width, height, border,
                           (GLsizei)dataLen, data);
}

void WebGL2RenderingContext::compressedTexSubImage2D(GLenum target, GLint level,
                                                      GLint xoffset, GLint yoffset,
                                                      GLsizei width, GLsizei height,
                                                      GLenum format,
                                                      const void* data, size_t dataLen) {
    if (!isCompressedFormatSupported(format)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    // Sub-rect origin must be block-aligned (4x4 for every exposed family).
    if (xoffset < 0 || yoffset < 0 || (xoffset % 4) || (yoffset % 4)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (width < 0 || height < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    size_t expected = (size_t)((width + 3) / 4) * ((height + 3) / 4) *
                      compressedBlockBytes(format);
    if (dataLen != expected) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format,
                              (GLsizei)dataLen, data);
}

// ===========================================================================
// Framebuffers
// ===========================================================================

WebGLFramebuffer WebGL2RenderingContext::createFramebuffer() {
    GLuint id = 0;
    glGenFramebuffers(1, &id);
    validFramebuffers_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteFramebuffer(WebGLFramebuffer fbo) {
    if (fbo.id && validFramebuffers_.erase(fbo.id)) {
        // Deleting the bound FBO reverts to the default framebuffer, which
        // for WebGL is the canvas FBO.
        if (sFBO_ == fbo.id) sFBO_ = canvasFBO_;
        glDeleteFramebuffers(1, &fbo.id);
    }
}

void WebGL2RenderingContext::bindFramebuffer(GLenum target, WebGLFramebuffer fbo) {
    // WebGL: null framebuffer = our canvas FBO (not the real default 0)
    GLuint id = fbo.id ? fbo.id : canvasFBO_;
    sFBO_ = id;
    glBindFramebuffer(target, id);
}


void WebGL2RenderingContext::framebufferTexture2D(GLenum target, GLenum attachment,
                                                   GLenum textarget, WebGLTexture tex, GLint level) {
    glFramebufferTexture2D(target, attachment, textarget, tex.id, level);
}

void WebGL2RenderingContext::framebufferRenderbuffer(GLenum target, GLenum attachment,
                                                      GLenum renderbuffertarget, WebGLRenderbuffer rbo) {
    glFramebufferRenderbuffer(target, attachment, renderbuffertarget, rbo.id);
}

GLenum WebGL2RenderingContext::checkFramebufferStatus(GLenum target) {
    return glCheckFramebufferStatus(target);
}

void WebGL2RenderingContext::readPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                                         GLenum format, GLenum type, void* pixels) {
    glReadPixels(x, y, width, height, format, type, pixels);
}

void WebGL2RenderingContext::readBuffer(GLenum src) {
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
}

void WebGL2RenderingContext::renderbufferStorageMultisample(GLenum target, GLsizei samples,
                                                            GLenum internalformat,
                                                            GLsizei width, GLsizei height) {
    glRenderbufferStorageMultisample(target, samples, internalformat, width, height);
}

// ===========================================================================
// Draw calls
// ===========================================================================

void WebGL2RenderingContext::drawArrays(GLenum mode, GLint first, GLsizei count) {
    glDrawArrays(mode, first, count);
}

void WebGL2RenderingContext::drawElements(GLenum mode, GLsizei count, GLenum type, GLintptr offset) {
    glDrawElements(mode, count, type, (const void*)offset);
}

void WebGL2RenderingContext::drawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instanceCount) {
    glDrawArraysInstanced(mode, first, count, instanceCount);
}

void WebGL2RenderingContext::drawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                                    GLintptr offset, GLsizei instanceCount) {
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
        "WEBGL_lose_context",
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
