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
    GLuint id = 0;
    glGenTextures(1, &id);
    validTextures_.insert(id);
    return {id};
}

void WebGL2RenderingContext::deleteTexture(WebGLTexture tex) {
    if (vkCtx_) { vkCtx_->deleteTexture(tex); return; }
    if (tex.id && validTextures_.erase(tex.id)) {
        for (auto& slot : sTex2D_)
            if (slot == tex.id) slot = 0; // GL auto-unbinds deleted textures
        glDeleteTextures(1, &tex.id);
    }
}

void WebGL2RenderingContext::bindTexture(GLenum target, WebGLTexture tex) {
    if (vkCtx_) { vkCtx_->bindTexture(target, tex); return; }
    if (target == GL_TEXTURE_2D) {
        unsigned unit = sActiveTex_ - GL_TEXTURE0;
        if (unit < 32) sTex2D_[unit] = tex.id;
    }
    glBindTexture(target, tex.id);
}

void WebGL2RenderingContext::activeTexture(GLenum texture) {
    if (vkCtx_) { vkCtx_->activeTexture(texture); return; }
    sActiveTex_ = texture;
    glActiveTexture(texture);
}

void WebGL2RenderingContext::texParameteri(GLenum target, GLenum pname, GLint param) {
    if (vkCtx_) { vkCtx_->texParameteri(target, pname, param); return; }
    glTexParameteri(target, pname, param);
}

void WebGL2RenderingContext::texParameterf(GLenum target, GLenum pname, GLfloat param) {
    glTexParameterf(target, pname, param);
}

// Translate WebGL2 unsized internal formats to GL 3.3 Core sized formats
GLint translateInternalFormat(GLint internalformat, GLenum type) {
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
int bytesPerPixel(GLenum format, GLenum type) {
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
    if (vkCtx_) {
        vkCtx_->texImage2D(target, level, internalformat, width, height, border, format, type, pixels);
        return;
    }
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


} // namespace bro::webgl
