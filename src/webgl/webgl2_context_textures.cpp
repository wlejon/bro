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
    WebGLTexture tex{0};
    if (vkCtx_) tex = vkCtx_->createTexture();
    return tex;
}

void WebGL2RenderingContext::deleteTexture(WebGLTexture tex) {
    validTextures_.erase(tex.id);
    for (int i = 0; i < 32; ++i) {
        if (sTex2D_[i] == tex.id) sTex2D_[i] = 0;
    }
    if (vkCtx_) vkCtx_->deleteTexture(tex);
}

void WebGL2RenderingContext::bindTexture(GLenum target, WebGLTexture tex) {
    if (tex.id != 0) validTextures_.insert(tex.id);
    GLuint unit = (sActiveTex_ >= GL_TEXTURE0 && sActiveTex_ < GL_TEXTURE0 + 32) ? (sActiveTex_ - GL_TEXTURE0) : 0;
    sTex2D_[unit] = tex.id;
    if (vkCtx_) vkCtx_->bindTexture(target, tex);
}

void WebGL2RenderingContext::activeTexture(GLenum texture) {
    sActiveTex_ = texture;
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
                case 0x1406: return 0x8814; // GL_RGBA32F (GL_FLOAT = 0x1406)
                case 0x140B: return 0x881A; // GL_RGBA16F (GL_HALF_FLOAT = 0x140B)
                case 0x1402: return 0x8D7C; // GL_RGBA32I
                case 0x1404: return 0x8D70; // GL_RGBA32UI
                default:     return 0x8058;
            }
        case 0x1907: // GL_RGB
            switch (type) {
                case 0x1401: return 0x8051; // GL_RGB8
                case 0x8363: return 0x8050; // GL_RGB565
                case 0x1406: return 0x8815; // GL_RGB32F (GL_FLOAT = 0x1406)
                case 0x140B: return 0x881B; // GL_RGB16F (GL_HALF_FLOAT = 0x140B)
                default:     return 0x8051;
            }
        case 0x1909: // GL_LUMINANCE
            return (type == 0x1406 || type == 0x140B) ? 0x8814 : 0x8229; // GL_R8
        case 0x190A: // GL_LUMINANCE_ALPHA
            return 0x8227; // GL_RG8
        case 0x1906: // GL_ALPHA
            return 0x8229; // GL_R8
        case 0x1902: // GL_DEPTH_COMPONENT
            switch (type) {
                case 0x1403: return 0x81A5; // GL_DEPTH_COMPONENT16
                case 0x1405: return 0x81A6; // GL_DEPTH_COMPONENT24
                case 0x1406: return 0x8CAC; // GL_DEPTH_COMPONENT32F (GL_FLOAT = 0x1406)
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
        case 0x1402: case 0x1403: case 0x140B: bpc = 2; break; // SHORT, UNSIGNED_SHORT, HALF_FLOAT (0x140B)
        case 0x1404: case 0x1405: case 0x1406: bpc = 4; break; // INT, UNSIGNED_INT, FLOAT (0x1406)
        case 0x8033: case 0x8034: case 0x8363: return 2; // packed 16-bit
        case 0x84FA: case 0x8C3E: return 4; // packed 32-bit
        default: break;
    }
    switch (format) {
        case 0x1908: case 0x8C92: return 4 * bpc; // RGBA
        case 0x1907: return 3 * bpc; // RGB
        case 0x8227: case 0x8228: return 2 * bpc; // RG
        case 0x1903: case 0x1906: case 0x1909: case 0x1902: case 0x8229: return 1 * bpc; // RED (0x1903), ALPHA, LUMINANCE, DEPTH
        default: return 4;
    }
}

void WebGL2RenderingContext::texImage2D(GLenum target, GLint level, GLint internalformat,
                                         GLsizei width, GLsizei height, GLint border,
                                         GLenum format, GLenum type, const void* pixels) {
    if (sPixelUnpack_ != 0 && pixels != nullptr) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (vkCtx_) {
        vkCtx_->texImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    }
}

void WebGL2RenderingContext::texSubImage2D(GLenum target, GLint level,
                                            GLint xoffset, GLint yoffset,
                                            GLsizei width, GLsizei height,
                                            GLenum format, GLenum type, const void* pixels) {
    if (sPixelUnpack_ != 0 && pixels != nullptr) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (vkCtx_) {
        vkCtx_->texSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    }
}

void WebGL2RenderingContext::copyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                                             GLint x, GLint y, GLsizei width, GLsizei height,
                                             GLint border) {
    if (vkCtx_) vkCtx_->copyTexImage2D(target, level, internalformat, x, y, width, height, border);
}

void WebGL2RenderingContext::copyTexSubImage2D(GLenum target, GLint level,
                                                GLint xoffset, GLint yoffset,
                                                GLint x, GLint y, GLsizei width, GLsizei height) {
    if (vkCtx_) vkCtx_->copyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
}

void WebGL2RenderingContext::generateMipmap(GLenum target) {
    if (vkCtx_) vkCtx_->generateMipmap(target);
}

void WebGL2RenderingContext::texStorage2D(GLenum target, GLsizei levels, GLenum internalformat,
                                           GLsizei width, GLsizei height) {
    if (vkCtx_) vkCtx_->texStorage2D(target, levels, internalformat, width, height);
}

void WebGL2RenderingContext::texStorage3D(GLenum target, GLsizei levels, GLenum internalformat,
                                           GLsizei width, GLsizei height, GLsizei depth) {
    if (vkCtx_) vkCtx_->texStorage3D(target, levels, internalformat, width, height, depth);
}

bool WebGL2RenderingContext::isCompressedFormatSupported(GLenum format) const {
    for (GLint f : compressedFormats_) {
        if (static_cast<GLenum>(f) == format) return true;
    }
    return false;
}

static void decompressBC(GLenum format, GLsizei width, GLsizei height,
                         const void* data, std::vector<uint8_t>& rgba) {
    rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    const uint8_t* src = static_cast<const uint8_t*>(data);
    GLsizei blocksW = (width + 3) / 4;
    GLsizei blocksH = (height + 3) / 4;

    if (format == 0x8DBB || format == 0x8DBC) { // RGTC1 (BC4 red)
        for (GLsizei by = 0; by < blocksH; ++by) {
            for (GLsizei bx = 0; bx < blocksW; ++bx) {
                const uint8_t* bsrc = src + (by * blocksW + bx) * 8;
                uint8_t r0 = bsrc[0];
                uint8_t r1 = bsrc[1];
                uint8_t pal[8];
                pal[0] = r0; pal[1] = r1;
                if (r0 > r1) {
                    for (int i = 1; i <= 6; ++i) pal[1 + i] = ((7 - i) * r0 + i * r1) / 7;
                } else {
                    for (int i = 1; i <= 4; ++i) pal[1 + i] = ((5 - i) * r0 + i * r1) / 5;
                    pal[6] = 0; pal[7] = 255;
                }
                uint64_t ind = 0;
                for (int i = 0; i < 6; ++i) ind |= (static_cast<uint64_t>(bsrc[2 + i]) << (i * 8));
                for (int py = 0; py < 4 && by * 4 + py < height; ++py) {
                    for (int px = 0; px < 4 && bx * 4 + px < width; ++px) {
                        int bitPos = (py * 4 + px) * 3;
                        int idx = (ind >> bitPos) & 0x7;
                        size_t dstIdx = ((by * 4 + py) * width + (bx * 4 + px)) * 4;
                        rgba[dstIdx + 0] = pal[idx];
                        rgba[dstIdx + 1] = 0;
                        rgba[dstIdx + 2] = 0;
                        rgba[dstIdx + 3] = 255;
                    }
                }
            }
        }
    } else if (format == 0x83F0 || format == 0x83F1) { // S3TC DXT1
        for (GLsizei by = 0; by < blocksH; ++by) {
            for (GLsizei bx = 0; bx < blocksW; ++bx) {
                const uint8_t* bsrc = src + (by * blocksW + bx) * 8;
                uint16_t c0 = bsrc[0] | (bsrc[1] << 8);
                uint16_t c1 = bsrc[2] | (bsrc[3] << 8);
                uint8_t r[4], g[4], b[4], a[4];
                r[0] = ((c0 >> 11) & 0x1F) * 255 / 31;
                g[0] = ((c0 >> 5) & 0x3F) * 255 / 63;
                b[0] = (c0 & 0x1F) * 255 / 31;
                a[0] = 255;
                r[1] = ((c1 >> 11) & 0x1F) * 255 / 31;
                g[1] = ((c1 >> 5) & 0x3F) * 255 / 63;
                b[1] = (c1 & 0x1F) * 255 / 31;
                a[1] = 255;
                if (c0 > c1) {
                    r[2] = (2 * r[0] + r[1]) / 3;
                    g[2] = (2 * g[0] + g[1]) / 3;
                    b[2] = (2 * b[0] + b[1]) / 3;
                    a[2] = 255;
                    r[3] = (r[0] + 2 * r[1]) / 3;
                    g[3] = (g[0] + 2 * g[1]) / 3;
                    b[3] = (b[0] + 2 * b[1]) / 3;
                    a[3] = 255;
                } else {
                    r[2] = (r[0] + r[1]) / 2;
                    g[2] = (g[0] + g[1]) / 2;
                    b[2] = (b[0] + b[1]) / 2;
                    a[2] = 255;
                    r[3] = 0; g[3] = 0; b[3] = 0; a[3] = 0;
                }
                uint32_t ind = bsrc[4] | (bsrc[5] << 8) | (bsrc[6] << 16) | (bsrc[7] << 24);
                for (int py = 0; py < 4 && by * 4 + py < height; ++py) {
                    for (int px = 0; px < 4 && bx * 4 + px < width; ++px) {
                        int idx = (ind >> ((py * 4 + px) * 2)) & 0x3;
                        size_t dstIdx = ((by * 4 + py) * width + (bx * 4 + px)) * 4;
                        rgba[dstIdx + 0] = r[idx];
                        rgba[dstIdx + 1] = g[idx];
                        rgba[dstIdx + 2] = b[idx];
                        rgba[dstIdx + 3] = a[idx];
                    }
                }
            }
        }
    }
}

void WebGL2RenderingContext::compressedTexImage2D(GLenum target, GLint level,
                                                   GLenum internalformat,
                                                   GLsizei width, GLsizei height, GLint border,
                                                   const void* data, size_t dataLen) {
    if (!isCompressedFormatSupported(internalformat)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if (width <= 0 || height <= 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    GLsizei blocksW = (width + 3) / 4;
    GLsizei blocksH = (height + 3) / 4;
    size_t blockSize = (internalformat == 0x8DBB || internalformat == 0x8DBC || internalformat == 0x83F0 || internalformat == 0x83F1) ? 8 : 16;
    size_t expectedSize = static_cast<size_t>(blocksW) * blocksH * blockSize;
    if (dataLen != expectedSize) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }

    std::vector<uint8_t> rgba;
    decompressBC(internalformat, width, height, data, rgba);
    texImage2D(target, level, GL_RGBA, width, height, border, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
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
    if (xoffset % 4 != 0 || yoffset % 4 != 0 || width % 4 != 0 || height % 4 != 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    GLsizei blocksW = width / 4;
    GLsizei blocksH = height / 4;
    size_t blockSize = (format == 0x8DBB || format == 0x8DBC || format == 0x83F0 || format == 0x83F1) ? 8 : 16;
    size_t expectedSize = static_cast<size_t>(blocksW) * blocksH * blockSize;
    if (dataLen != expectedSize) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }

    std::vector<uint8_t> rgba;
    decompressBC(format, width, height, data, rgba);
    texSubImage2D(target, level, xoffset, yoffset, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

WebGLSampler WebGL2RenderingContext::createSampler() {
    WebGLSampler s{0};
    if (vkCtx_) s = vkCtx_->createSampler();
    if (s.id != 0) validSamplers_.insert(s.id);
    return s;
}

void WebGL2RenderingContext::deleteSampler(WebGLSampler s) {
    validSamplers_.erase(s.id);
    for (int i = 0; i < 32; ++i) {
        if (sSampler_[i] == s.id) sSampler_[i] = 0;
    }
    if (vkCtx_) vkCtx_->deleteSampler(s);
}

void WebGL2RenderingContext::bindSampler(GLuint unit, WebGLSampler s) {
    if (unit < 32) sSampler_[unit] = s.id;
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
