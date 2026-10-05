#include "webgl/webgl2_context.h"
#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"


namespace bro::webgl {

// ===========================================================================
// Textures
// ===========================================================================

WebGLTexture WebGL2RenderingContext::createTexture() {
    WebGLTexture tex{0};
    if (vkCtx_) tex = vkCtx_->createTexture();
    return tex;
}

void WebGL2RenderingContext::deleteTexture(WebGLTexture tex) {
    if (vkCtx_) vkCtx_->deleteTexture(tex);
}

void WebGL2RenderingContext::bindTexture(GLenum target, WebGLTexture tex) {
    if (vkCtx_) vkCtx_->bindTexture(target, tex);
}

void WebGL2RenderingContext::activeTexture(GLenum texture) {
    if (vkCtx_) vkCtx_->activeTexture(texture);
}

GLuint WebGL2RenderingContext::activeTextureUnit() const {
    return vkCtx_ ? static_cast<GLuint>(vkCtx_->getParameterInt(GL_ACTIVE_TEXTURE)) - GL_TEXTURE0 : 0;
}

WebGLTexture WebGL2RenderingContext::boundTexture(GLenum target) const {
    return {vkCtx_ ? vkCtx_->boundTexture(target) : 0};
}

GLboolean WebGL2RenderingContext::isTexture(WebGLTexture tex) {
    return vkCtx_ ? vkCtx_->isTexture(tex) : GL_FALSE;
}

void WebGL2RenderingContext::texParameteri(GLenum target, GLenum pname, GLint param) {
    if (vkCtx_) vkCtx_->texParameteri(target, pname, param);
}

void WebGL2RenderingContext::texParameterf(GLenum target, GLenum pname, GLfloat param) {
    if (vkCtx_) vkCtx_->texParameterf(target, pname, param);
}

bool WebGL2RenderingContext::getTexParameter(GLenum target, GLenum pname, TexParameterValue& out) {
    return vkCtx_ && vkCtx_->getTexParameter(target, pname, out);
}

void WebGL2RenderingContext::texImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                        GLsizei height, GLint border, GLenum format, GLenum type,
                                        const void* pixels, size_t size) {
    if (vkCtx_) vkCtx_->texImage2D(target, level, internalformat, width, height, border, format, type, pixels, size);
}

void WebGL2RenderingContext::texSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                           GLsizei width, GLsizei height, GLenum format, GLenum type,
                                           const void* pixels, size_t size) {
    if (vkCtx_) vkCtx_->texSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels, size);
}

void WebGL2RenderingContext::texImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                        GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type,
                                        const void* pixels, size_t size) {
    if (vkCtx_)
        vkCtx_->texImage3D(target, level, internalformat, width, height, depth, border, format, type, pixels, size);
}

void WebGL2RenderingContext::texSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                           GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                                           GLenum format, GLenum type, const void* pixels, size_t size) {
    if (vkCtx_)
        vkCtx_->texSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels,
                              size);
}

void WebGL2RenderingContext::texImage2DSource(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                              GLsizei height, GLenum format, GLenum type, const uint8_t* rgba,
                                              uint32_t srcWidth, uint32_t srcHeight) {
    if (vkCtx_)
        vkCtx_->texImage2DSource(target, level, internalformat, width, height, format, type, rgba, srcWidth,
                                 srcHeight);
}

void WebGL2RenderingContext::texSubImage2DSource(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                                 GLsizei width, GLsizei height, GLenum format, GLenum type,
                                                 const uint8_t* rgba, uint32_t srcWidth, uint32_t srcHeight) {
    if (vkCtx_)
        vkCtx_->texSubImage2DSource(target, level, xoffset, yoffset, width, height, format, type, rgba, srcWidth,
                                    srcHeight);
}

void WebGL2RenderingContext::texImage2DFromPBO(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                               GLsizei height, GLint border, GLenum format, GLenum type,
                                               GLintptr offset) {
    if (vkCtx_)
        vkCtx_->texImage2DFromPBO(target, level, internalformat, width, height, border, format, type, offset);
}

void WebGL2RenderingContext::texSubImage2DFromPBO(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                                  GLsizei width, GLsizei height, GLenum format, GLenum type,
                                                  GLintptr offset) {
    if (vkCtx_)
        vkCtx_->texSubImage2DFromPBO(target, level, xoffset, yoffset, width, height, format, type, offset);
}

void WebGL2RenderingContext::texImage3DFromPBO(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                               GLsizei height, GLsizei depth, GLint border, GLenum format,
                                               GLenum type, GLintptr offset) {
    if (vkCtx_)
        vkCtx_->texImage3DFromPBO(target, level, internalformat, width, height, depth, border, format, type,
                                  offset);
}

void WebGL2RenderingContext::texSubImage3DFromPBO(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                                  GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                                                  GLenum format, GLenum type, GLintptr offset) {
    if (vkCtx_)
        vkCtx_->texSubImage3DFromPBO(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type,
                                     offset);
}

void WebGL2RenderingContext::copyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
                                            GLsizei width, GLsizei height, GLint border) {
    if (vkCtx_) vkCtx_->copyTexImage2D(target, level, internalformat, x, y, width, height, border);
}

void WebGL2RenderingContext::copyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x,
                                               GLint y, GLsizei width, GLsizei height) {
    if (vkCtx_) vkCtx_->copyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
}

void WebGL2RenderingContext::copyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                               GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height) {
    if (vkCtx_) vkCtx_->copyTexSubImage3D(target, level, xoffset, yoffset, zoffset, x, y, width, height);
}

void WebGL2RenderingContext::generateMipmap(GLenum target) {
    if (vkCtx_) vkCtx_->generateMipmap(target);
}

void WebGL2RenderingContext::texStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width,
                                          GLsizei height) {
    if (vkCtx_) vkCtx_->texStorage2D(target, levels, internalformat, width, height);
}

void WebGL2RenderingContext::texStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width,
                                          GLsizei height, GLsizei depth) {
    if (vkCtx_) vkCtx_->texStorage3D(target, levels, internalformat, width, height, depth);
}

std::vector<GLint> WebGL2RenderingContext::compressedTextureFormats() const {
    return vkCtx_ ? vkCtx_->compressedTextureFormats() : std::vector<GLint>{};
}

void WebGL2RenderingContext::compressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                                                  GLsizei width, GLsizei height, GLint border, const void* data,
                                                  size_t dataLen) {
    if (vkCtx_) vkCtx_->compressedTexImage2D(target, level, internalformat, width, height, border, data, dataLen);
}

void WebGL2RenderingContext::compressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                                     GLsizei width, GLsizei height, GLenum format,
                                                     const void* data, size_t dataLen) {
    if (vkCtx_)
        vkCtx_->compressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, data, dataLen);
}

void WebGL2RenderingContext::compressedTexImage3D(GLenum target, GLint level, GLenum internalformat,
                                                  GLsizei width, GLsizei height, GLsizei depth, GLint border,
                                                  const void* data, size_t dataLen) {
    if (vkCtx_)
        vkCtx_->compressedTexImage3D(target, level, internalformat, width, height, depth, border, data, dataLen);
}

void WebGL2RenderingContext::compressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                                     GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                                                     GLenum format, const void* data, size_t dataLen) {
    if (vkCtx_)
        vkCtx_->compressedTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format,
                                        data, dataLen);
}

void WebGL2RenderingContext::compressedTexImageFromPBO(GLenum target, GLint level, GLenum internalformat,
                                                       GLsizei width, GLsizei height, GLsizei depth, GLint border,
                                                       GLsizei size, GLintptr offset, bool is3D) {
    if (vkCtx_)
        vkCtx_->compressedTexImageFromPBO(target, level, internalformat, width, height, depth, border, size, offset,
                                          is3D);
}

void WebGL2RenderingContext::compressedTexSubImageFromPBO(GLenum target, GLint level, GLint xoffset,
                                                          GLint yoffset, GLint zoffset, GLsizei width,
                                                          GLsizei height, GLsizei depth, GLenum format,
                                                          GLsizei size, GLintptr offset, bool is3D) {
    if (vkCtx_)
        vkCtx_->compressedTexSubImageFromPBO(target, level, xoffset, yoffset, zoffset, width, height, depth, format,
                                             size, offset, is3D);
}

WebGLSampler WebGL2RenderingContext::createSampler() {
    WebGLSampler s{0};
    if (vkCtx_) s = vkCtx_->createSampler();
    if (s.id != 0) validSamplers_.insert(s.id);
    return s;
}

void WebGL2RenderingContext::deleteSampler(WebGLSampler s) {
    validSamplers_.erase(s.id);
    if (vkCtx_) vkCtx_->deleteSampler(s);
}

void WebGL2RenderingContext::bindSampler(GLuint unit, WebGLSampler s) {
    if (vkCtx_) vkCtx_->bindSampler(unit, s);
}

void WebGL2RenderingContext::samplerParameteri(WebGLSampler s, GLenum pname, GLint param) {
    if (vkCtx_) vkCtx_->samplerParameteri(s, pname, param);
}

void WebGL2RenderingContext::samplerParameterf(WebGLSampler s, GLenum pname, GLfloat param) {
    if (vkCtx_) vkCtx_->samplerParameterf(s, pname, param);
}

GLint WebGL2RenderingContext::getSamplerParameteri(WebGLSampler s, GLenum pname) {
    if (vkCtx_) return vkCtx_->getSamplerParameteri(s, pname);
    return 0;
}

GLfloat WebGL2RenderingContext::getSamplerParameterf(WebGLSampler s, GLenum pname) {
    if (vkCtx_) return vkCtx_->getSamplerParameterf(s, pname);
    return 0.0f;
}

GLboolean WebGL2RenderingContext::isSampler(WebGLSampler s) {
    return (s.id != 0 && validSamplers_.count(s.id) > 0) ? GL_TRUE : GL_FALSE;
}

} // namespace bro::webgl
