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
    return {0};
}

void WebGL2RenderingContext::deleteFramebuffer(WebGLFramebuffer fbo) {
    if (vkCtx_) vkCtx_->deleteFramebuffer(fbo);
}

void WebGL2RenderingContext::bindFramebuffer(GLenum target, WebGLFramebuffer fbo) {
    if (vkCtx_) vkCtx_->bindFramebuffer(target, fbo);
}

void WebGL2RenderingContext::framebufferTexture2D(GLenum target, GLenum attachment,
                                                   GLenum textarget, WebGLTexture tex, GLint level) {
    if (vkCtx_) vkCtx_->framebufferTexture2D(target, attachment, textarget, tex, level);
}

void WebGL2RenderingContext::framebufferRenderbuffer(GLenum /*target*/, GLenum /*attachment*/,
                                                      GLenum /*renderbuffertarget*/, WebGLRenderbuffer /*rbo*/) {}

GLenum WebGL2RenderingContext::checkFramebufferStatus(GLenum target) {
    if (vkCtx_) return vkCtx_->checkFramebufferStatus(target);
    return 0x8CD5; // GL_FRAMEBUFFER_COMPLETE
}

void WebGL2RenderingContext::readPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                                         GLenum format, GLenum type, void* pixels) {
    if (vkCtx_) vkCtx_->readPixels(x, y, width, height, format, type, pixels);
}

void WebGL2RenderingContext::drawBuffers(GLsizei /*n*/, const GLenum* /*bufs*/) {}
void WebGL2RenderingContext::readBuffer(GLenum /*src*/) {}

void WebGL2RenderingContext::blitFramebuffer(GLint /*srcX0*/, GLint /*srcY0*/, GLint /*srcX1*/, GLint /*srcY1*/,
                                             GLint /*dstX0*/, GLint /*dstY0*/, GLint /*dstX1*/, GLint /*dstY1*/,
                                             GLbitfield /*mask*/, GLenum /*filter*/) {}

// ===========================================================================
// Renderbuffers
// ===========================================================================

WebGLRenderbuffer WebGL2RenderingContext::createRenderbuffer() { return {0}; }
void WebGL2RenderingContext::deleteRenderbuffer(WebGLRenderbuffer /*rbo*/) {}
void WebGL2RenderingContext::bindRenderbuffer(GLenum /*target*/, WebGLRenderbuffer /*rbo*/) {}
void WebGL2RenderingContext::renderbufferStorage(GLenum /*target*/, GLenum /*internalformat*/,
                                                 GLsizei /*width*/, GLsizei /*height*/) {}
void WebGL2RenderingContext::renderbufferStorageMultisample(GLenum /*target*/, GLsizei /*samples*/,
                                                            GLenum /*internalformat*/,
                                                            GLsizei /*width*/, GLsizei /*height*/) {}

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

WebGLSync WebGL2RenderingContext::fenceSync(GLenum /*condition*/, GLbitfield /*flags*/) { return {nullptr}; }
void WebGL2RenderingContext::deleteSync(WebGLSync /*s*/) {}
GLenum WebGL2RenderingContext::clientWaitSync(WebGLSync /*s*/, GLbitfield /*flags*/, double /*timeoutNs*/) { return 0x911A; /* GL_ALREADY_SIGNALED */ }
void WebGL2RenderingContext::waitSync(WebGLSync /*s*/, GLbitfield /*flags*/, double /*timeoutNs*/) {}
GLint WebGL2RenderingContext::getSyncParameter(WebGLSync /*s*/, GLenum /*pname*/) { return 0x9118; /* GL_SIGNALED */ }
GLboolean WebGL2RenderingContext::isSync(WebGLSync /*s*/) { return GL_FALSE; }

void WebGL2RenderingContext::restoreState() {}

} // namespace bro::webgl
