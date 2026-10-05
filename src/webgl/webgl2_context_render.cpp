#include "webgl/webgl2_context.h"
#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl {

// ===========================================================================
// Framebuffers
// ===========================================================================

WebGLFramebuffer WebGL2RenderingContext::createFramebuffer() {
    WebGLFramebuffer fbo{0};
    if (vkCtx_) fbo = vkCtx_->createFramebuffer();
    return fbo;
}

void WebGL2RenderingContext::deleteFramebuffer(WebGLFramebuffer fbo) {
    validFramebuffers_.erase(fbo.id);
    if (vkCtx_) vkCtx_->deleteFramebuffer(fbo);
}

void WebGL2RenderingContext::bindFramebuffer(GLenum target, WebGLFramebuffer fbo) {
    if (fbo.id != 0) validFramebuffers_.insert(fbo.id);
    if (vkCtx_) vkCtx_->bindFramebuffer(target, fbo);
}

void WebGL2RenderingContext::framebufferTexture2D(GLenum target, GLenum attachment,
                                                   GLenum textarget, WebGLTexture tex, GLint level) {
    if (vkCtx_) vkCtx_->framebufferTexture2D(target, attachment, textarget, tex, level);
}

void WebGL2RenderingContext::framebufferRenderbuffer(GLenum target, GLenum attachment,
                                                      GLenum renderbuffertarget, WebGLRenderbuffer rbo) {
    if (vkCtx_) vkCtx_->framebufferRenderbuffer(target, attachment, renderbuffertarget, rbo);
}

void WebGL2RenderingContext::framebufferTextureLayer(GLenum target, GLenum attachment, WebGLTexture tex,
                                                     GLint level, GLint layer) {
    if (vkCtx_) vkCtx_->framebufferTextureLayer(target, attachment, tex, level, layer);
}

WebGLFramebuffer WebGL2RenderingContext::currentDrawFramebuffer() const {
    return {vkCtx_ ? vkCtx_->drawFramebufferBinding() : 0};
}
WebGLFramebuffer WebGL2RenderingContext::currentReadFramebuffer() const {
    return {vkCtx_ ? vkCtx_->readFramebufferBinding() : 0};
}
WebGLRenderbuffer WebGL2RenderingContext::currentRenderbuffer() const {
    return {vkCtx_ ? vkCtx_->renderbufferBinding() : 0};
}

bool WebGL2RenderingContext::getFramebufferAttachmentParameter(GLenum target, GLenum attachment, GLenum pname,
                                                               GLint& value, GLenum& objectType, GLuint& objectName,
                                                               bool& isNull) {
    vk::WebGLVkContext::AttachmentParameter out;
    if (!vkCtx_ || !vkCtx_->getFramebufferAttachmentParameter(target, attachment, pname, out)) return false;
    value = out.value;
    objectType = out.objectType;
    objectName = out.objectName;
    isNull = out.isNull;
    return true;
}
GLint WebGL2RenderingContext::getRenderbufferParameter(GLenum target, GLenum pname) {
    return vkCtx_ ? vkCtx_->getRenderbufferParameter(target, pname) : 0;
}
std::vector<GLint> WebGL2RenderingContext::supportedSampleCounts(GLenum internalformat) {
    return vkCtx_ ? vkCtx_->supportedSampleCounts(internalformat) : std::vector<GLint>{};
}

GLenum WebGL2RenderingContext::checkFramebufferStatus(GLenum target) {
    return vkCtx_ ? vkCtx_->checkFramebufferStatus(target) : GL_FRAMEBUFFER_UNSUPPORTED;
}

void WebGL2RenderingContext::readPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                                         GLenum format, GLenum type, void* pixels) {
    if (vkCtx_) vkCtx_->readPixels(x, y, width, height, format, type, pixels);
}

void WebGL2RenderingContext::drawBuffers(GLsizei n, const GLenum* bufs) {
    if (vkCtx_) vkCtx_->drawBuffers(n, bufs);
}
void WebGL2RenderingContext::readBuffer(GLenum src) {
    if (vkCtx_) vkCtx_->readBuffer(src);
}

void WebGL2RenderingContext::blitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                             GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                             GLbitfield mask, GLenum filter) {
    if (vkCtx_) vkCtx_->blitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

// ===========================================================================
// Renderbuffers
// ===========================================================================

WebGLRenderbuffer WebGL2RenderingContext::createRenderbuffer() {
    WebGLRenderbuffer rbo{0};
    if (vkCtx_) rbo = vkCtx_->createRenderbuffer();
    else {
        static GLuint s_nextRboId = 1;
        rbo = {s_nextRboId++};
    }
    return rbo;
}
void WebGL2RenderingContext::deleteRenderbuffer(WebGLRenderbuffer rbo) {
    validRenderbuffers_.erase(rbo.id);
    if (vkCtx_) vkCtx_->deleteRenderbuffer(rbo);
}
void WebGL2RenderingContext::bindRenderbuffer(GLenum target, WebGLRenderbuffer rbo) {
    if (rbo.id != 0) validRenderbuffers_.insert(rbo.id);
    if (vkCtx_) vkCtx_->bindRenderbuffer(target, rbo);
}
void WebGL2RenderingContext::renderbufferStorage(GLenum target, GLenum internalformat,
                                                 GLsizei width, GLsizei height) {
    if (vkCtx_) vkCtx_->renderbufferStorage(target, internalformat, width, height);
}
void WebGL2RenderingContext::renderbufferStorageMultisample(GLenum target, GLsizei samples,
                                                            GLenum internalformat,
                                                            GLsizei width, GLsizei height) {
    if (vkCtx_) vkCtx_->renderbufferStorageMultisample(target, samples, internalformat, width, height);
}

// ===========================================================================
// Drawing
// ===========================================================================

void WebGL2RenderingContext::drawArrays(GLenum mode, GLint first, GLsizei count) {
    if (vkCtx_) vkCtx_->drawArrays(mode, first, count);
}

void WebGL2RenderingContext::drawElements(GLenum mode, GLsizei count, GLenum type, GLintptr offset) {
    if (vkCtx_) vkCtx_->drawElements(mode, count, type, static_cast<uintptr_t>(offset));
}

void WebGL2RenderingContext::drawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instanceCount) {
    if (vkCtx_) vkCtx_->drawArraysInstanced(mode, first, count, instanceCount);
}

void WebGL2RenderingContext::drawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                                   GLintptr offset, GLsizei instanceCount) {
    if (vkCtx_) vkCtx_->drawElementsInstanced(mode, count, type, static_cast<uintptr_t>(offset), instanceCount);
}

void WebGL2RenderingContext::drawRangeElements(GLenum mode, GLuint /*start*/, GLuint /*end*/,
                                               GLsizei count, GLenum type, GLintptr offset) {
    drawElements(mode, count, type, offset);
}

void WebGL2RenderingContext::flush() {
    if (vkCtx_) vkCtx_->flush();
}

void WebGL2RenderingContext::finish() {
    if (vkCtx_) vkCtx_->finish();
}

// ===========================================================================
// Sync
// ===========================================================================

WebGLSync WebGL2RenderingContext::fenceSync(GLenum condition, GLbitfield flags) {
    if (condition != 0x9117 /* GL_SYNC_GPU_COMMANDS_COMPLETE */ || flags != 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return {nullptr};
    }
    // The sync object holds the queue ticket its fence completes with.
    const uint64_t ticket = vkCtx_ ? vkCtx_->insertFence() : 0;
    GLsync sync = reinterpret_cast<GLsync>(new uint64_t(ticket));
    validSyncs_.insert(sync);
    return {sync};
}

void WebGL2RenderingContext::deleteSync(WebGLSync s) {
    if (!s.sync) return;
    auto it = validSyncs_.find(s.sync);
    if (it != validSyncs_.end()) {
        validSyncs_.erase(it);
        delete reinterpret_cast<uint64_t*>(s.sync);
    }
}

GLenum WebGL2RenderingContext::clientWaitSync(WebGLSync s, GLbitfield flags, double timeoutNs) {
    if (timeoutNs < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return 0x911D; /* GL_WAIT_FAILED */
    }
    if (timeoutNs > kMaxClientWaitTimeoutNs) {
        setSyntheticError(GL_INVALID_OPERATION);
        return 0x911D; /* GL_WAIT_FAILED */
    }
    if (!s.sync || validSyncs_.count(s.sync) == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return 0x911D; /* GL_WAIT_FAILED */
    }
    if (flags & 0x00000001 /* GL_SYNC_FLUSH_COMMANDS_BIT */) {
        flush();
    }
    const uint64_t ticket = *reinterpret_cast<uint64_t*>(s.sync);
    if (!vkCtx_ || vkCtx_->isFenceSignaled(ticket)) return 0x911A; /* GL_ALREADY_SIGNALED */
    if (timeoutNs == 0) return 0x911B; /* GL_TIMEOUT_EXPIRED */
    return vkCtx_->waitFence(ticket, static_cast<uint64_t>(timeoutNs))
               ? 0x911C  /* GL_CONDITION_SATISFIED */
               : 0x911B; /* GL_TIMEOUT_EXPIRED */
}

void WebGL2RenderingContext::waitSync(WebGLSync s, GLbitfield flags, double timeoutNs) {
    if (flags != 0 || (timeoutNs != -1.0 && static_cast<int64_t>(timeoutNs) != -1)) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (!s.sync || validSyncs_.count(s.sync) == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
}

GLint WebGL2RenderingContext::getSyncParameter(WebGLSync s, GLenum pname) {
    if (!s.sync || validSyncs_.count(s.sync) == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return 0;
    }
    switch (pname) {
        case 0x9112: /* GL_OBJECT_TYPE */ return 0x9116; /* GL_SYNC_FENCE */
        case 0x9113: /* GL_SYNC_CONDITION */ return 0x9117; /* GL_SYNC_GPU_COMMANDS_COMPLETE */
        case 0x9115: /* GL_SYNC_FLAGS */ return 0;
        case 0x9114: /* GL_SYNC_STATUS */
            return (!vkCtx_ || vkCtx_->isFenceSignaled(*reinterpret_cast<uint64_t*>(s.sync)))
                       ? 0x9119   /* GL_SIGNALED */
                       : 0x9118;  /* GL_UNSIGNALED */
        default: return 0;
    }
}

GLboolean WebGL2RenderingContext::isSync(WebGLSync s) {
    return (s.sync != nullptr && validSyncs_.count(s.sync) > 0) ? GL_TRUE : GL_FALSE;
}

void WebGL2RenderingContext::restoreState() {}

} // namespace bro::webgl
