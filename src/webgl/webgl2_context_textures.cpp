#include "webgl/webgl2_context.h"
#include "webgl/glsl_translator.h"
#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl {

// ===========================================================================
// Textures
// ===========================================================================

WebGLTexture WebGL2RenderingContext::createTexture() {
    if (vkCtx_) return vkCtx_->createTexture();
    return {0};
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

void WebGL2RenderingContext::texParameteri(GLenum target, GLenum pname, GLint param) {
    if (vkCtx_) vkCtx_->texParameteri(target, pname, param);
}

void WebGL2RenderingContext::texParameterf(GLenum /*target*/, GLenum /*pname*/, GLfloat /*param*/) {}

// Translate WebGL2 unsized internal formats to sized formats
GLint translateInternalFormat(GLint internalformat, GLenum type) {
    switch (internalformat) {
        case 0x1908: // GL_RGBA
            switch (type) {
                case 0x1401: return 0x8058; // GL_RGBA8
                case 0x8363: return 0x8056; // GL_RGB5_A1
                case 0x8033: return 0x8057; // GL_RGBA4
                case 0x140B: return 0x8814; // GL_RGBA32F
                case 0x1402: return 0x8D7C; // GL_RGBA32I
                case 0x1404: return 0x8D70; // GL_RGBA32UI
                case 0x1406: return 0x881A; // GL_RGBA16F
                default:     return 0x8058;
            }
        case 0x1907: // GL_RGB
            switch (type) {
                case 0x1401: return 0x8051; // GL_RGB8
                case 0x8363: return 0x8050; // GL_RGB565
                case 0x140B: return 0x8815; // GL_RGB32F
                case 0x1406: return 0x881B; // GL_RGB16F
                default:     return 0x8051;
            }
        case 0x1909: // GL_LUMINANCE
            return (type == 0x140B) ? 0x8814 : 0x8229; // GL_R8
        case 0x190A: // GL_LUMINANCE_ALPHA
            return 0x8227; // GL_RG8
        case 0x1906: // GL_ALPHA
            return 0x8229; // GL_R8
        case 0x1902: // GL_DEPTH_COMPONENT
            switch (type) {
                case 0x1403: return 0x81A5; // GL_DEPTH_COMPONENT16
                case 0x1405: return 0x81A6; // GL_DEPTH_COMPONENT24
                case 0x140B: return 0x8CAC; // GL_DEPTH_COMPONENT32F
                default:     return 0x81A6;
            }
        case 0x84F9: // GL_DEPTH_STENCIL
            return 0x88F0; // GL_DEPTH24_STENCIL8
        default:
            return internalformat;
    }
}

int bytesPerPixel(GLenum format, GLenum type) {
    int bpc = 1;
    switch (type) {
        case 0x1400: case 0x1401: bpc = 1; break; // BYTE, UNSIGNED_BYTE
        case 0x1402: case 0x1403: case 0x1406: bpc = 2; break; // SHORT, UNSIGNED_SHORT, HALF_FLOAT
        case 0x1404: case 0x1405: case 0x140B: bpc = 4; break; // INT, UNSIGNED_INT, FLOAT
        case 0x8033: case 0x8034: case 0x8363: return 2; // packed 16-bit
        case 0x84FA: case 0x8C3E: return 4; // packed 32-bit
        default: break;
    }
    switch (format) {
        case 0x1908: case 0x8C92: return 4 * bpc; // RGBA
        case 0x1907: return 3 * bpc; // RGB
        case 0x8227: case 0x8228: return 2 * bpc; // RG
        case 0x1906: case 0x1909: case 0x1902: case 0x8229: return 1 * bpc; // RED, ALPHA, LUMINANCE
        default: return 4;
    }
}

void WebGL2RenderingContext::texImage2D(GLenum target, GLint level, GLint internalformat,
                                         GLsizei width, GLsizei height, GLint border,
                                         GLenum format, GLenum type, const void* pixels) {
    if (vkCtx_) {
        vkCtx_->texImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    }
}

void WebGL2RenderingContext::texSubImage2D(GLenum target, GLint level,
                                            GLint xoffset, GLint yoffset,
                                            GLsizei width, GLsizei height,
                                            GLenum format, GLenum type, const void* pixels) {
    if (vkCtx_) {
        vkCtx_->texSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    }
}

void WebGL2RenderingContext::copyTexImage2D(GLenum /*target*/, GLint /*level*/, GLenum /*internalformat*/,
                                             GLint /*x*/, GLint /*y*/, GLsizei /*width*/, GLsizei /*height*/,
                                             GLint /*border*/) {}

void WebGL2RenderingContext::copyTexSubImage2D(GLenum /*target*/, GLint /*level*/,
                                                GLint /*xoffset*/, GLint /*yoffset*/,
                                                GLint /*x*/, GLint /*y*/, GLsizei /*width*/, GLsizei /*height*/) {}

void WebGL2RenderingContext::generateMipmap(GLenum target) {
    if (vkCtx_) vkCtx_->generateMipmap(target);
}

void WebGL2RenderingContext::texStorage2D(GLenum /*target*/, GLsizei /*levels*/, GLenum /*internalformat*/,
                                           GLsizei /*width*/, GLsizei /*height*/) {}

void WebGL2RenderingContext::texStorage3D(GLenum /*target*/, GLsizei /*levels*/, GLenum /*internalformat*/,
                                           GLsizei /*width*/, GLsizei /*height*/, GLsizei /*depth*/) {}

bool WebGL2RenderingContext::isCompressedFormatSupported(GLenum /*format*/) const {
    return false;
}

void WebGL2RenderingContext::compressedTexImage2D(GLenum /*target*/, GLint /*level*/,
                                                   GLenum /*internalformat*/,
                                                   GLsizei /*width*/, GLsizei /*height*/, GLint /*border*/,
                                                   const void* /*data*/, size_t /*dataLen*/) {}

void WebGL2RenderingContext::compressedTexSubImage2D(GLenum /*target*/, GLint /*level*/,
                                                      GLint /*xoffset*/, GLint /*yoffset*/,
                                                      GLsizei /*width*/, GLsizei /*height*/,
                                                      GLenum /*format*/,
                                                      const void* /*data*/, size_t /*dataLen*/) {}

WebGLSampler WebGL2RenderingContext::createSampler() { return {0}; }
void WebGL2RenderingContext::deleteSampler(WebGLSampler /*s*/) {}
void WebGL2RenderingContext::bindSampler(GLuint /*unit*/, WebGLSampler /*s*/) {}
void WebGL2RenderingContext::samplerParameteri(WebGLSampler /*s*/, GLenum /*pname*/, GLint /*param*/) {}
void WebGL2RenderingContext::samplerParameterf(WebGLSampler /*s*/, GLenum /*pname*/, GLfloat /*param*/) {}
GLint WebGL2RenderingContext::getSamplerParameteri(WebGLSampler /*s*/, GLenum /*pname*/) { return 0; }
GLfloat WebGL2RenderingContext::getSamplerParameterf(WebGLSampler /*s*/, GLenum /*pname*/) { return 0.0f; }
GLboolean WebGL2RenderingContext::isSampler(WebGLSampler /*s*/) { return GL_FALSE; }

} // namespace bro::webgl
