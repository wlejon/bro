// Texture objects: creation, binding (a texture keeps the target it was
// first bound to), texture parameters, and completeness — what a sampler
// reads of a texture, or whether it reads the placeholder instead.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_formats.h"

#include <algorithm>
#include <cmath>

namespace bro::webgl::vk {

namespace {

bool isCubeFace(GLenum target) {
    return target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
}

bool isTextureTarget(GLenum target) {
    return target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_3D ||
           target == GL_TEXTURE_2D_ARRAY;
}

bool validFilter(GLenum pname, GLint v) {
    if (v == GL_NEAREST || v == GL_LINEAR) return true;
    return pname == GL_TEXTURE_MIN_FILTER && (v == GL_NEAREST_MIPMAP_NEAREST || v == GL_LINEAR_MIPMAP_NEAREST ||
                                              v == GL_NEAREST_MIPMAP_LINEAR || v == GL_LINEAR_MIPMAP_LINEAR);
}

bool validCompareFunc(GLint v) { return v >= GL_NEVER && v <= GL_ALWAYS; }

} // namespace

WebGLTexture WebGLVkContext::createTexture() {
    GLuint id = nextObjectId_++;
    textures_[id] = VkTextureResource{};
    return {id};
}

void WebGLVkContext::deleteTexture(WebGLTexture tex) {
    auto it = tex.id != 0 ? textures_.find(tex.id) : textures_.end();
    if (it == textures_.end()) return;
    // Detached from the bound framebuffers (GL leaves other framebuffers
    // naming it, now incomplete); the open pass may render into it.
    for (GLuint fboId : {drawFboId_, readFboId_}) {
        auto fIt = fboId != 0 ? framebuffers_.find(fboId) : framebuffers_.end();
        if (fIt == framebuffers_.end()) continue;
        auto detach = [&](VkFboAttachment& att) {
            if (att.kind != VkFboAttachment::Kind::Texture || att.id != tex.id) return;
            framebufferChanged(fboId);
            att = {};
        };
        for (VkFboAttachment& c : fIt->second.color) detach(c);
        detach(fIt->second.depth);
        detach(fIt->second.stencil);
    }
    // And unbound from every texture unit.
    for (auto* units : {&boundTextures2D_, &boundTexturesCubeMap_, &boundTextures2DArray_, &boundTextures3D_})
        for (GLuint& id : *units)
            if (id == tex.id) id = 0;
    endRendering();
    releaseTexture(it->second);
    textures_.erase(it);
}

void WebGLVkContext::bindTexture(GLenum target, WebGLTexture tex) {
    if (!isTextureTarget(target)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if (tex.id != 0) {
        auto it = textures_.find(tex.id);
        if (it == textures_.end()) {
            setSyntheticError(GL_INVALID_OPERATION);  // deleted
            return;
        }
        if (it->second.target != 0 && it->second.target != target) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        it->second.target = target;
    }
    switch (target) {
        case GL_TEXTURE_CUBE_MAP: boundTexturesCubeMap_[activeTextureUnit_] = tex.id; break;
        case GL_TEXTURE_2D_ARRAY: boundTextures2DArray_[activeTextureUnit_] = tex.id; break;
        case GL_TEXTURE_3D: boundTextures3D_[activeTextureUnit_] = tex.id; break;
        default: boundTextures2D_[activeTextureUnit_] = tex.id; break;
    }
}

void WebGLVkContext::activeTexture(GLenum texture) {
    const GLint units = getParameterInt(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS);
    if (texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + static_cast<GLenum>(units)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    activeTextureUnit_ = texture - GL_TEXTURE0;
}

GLboolean WebGLVkContext::isTexture(WebGLTexture tex) const {
    auto it = tex.id != 0 ? textures_.find(tex.id) : textures_.end();
    return it != textures_.end() && it->second.target != 0 ? GL_TRUE : GL_FALSE;
}

GLuint WebGLVkContext::boundTexture(GLenum target) const {
    switch (target) {
        case GL_TEXTURE_2D: return boundTextures2D_[activeTextureUnit_];
        case GL_TEXTURE_CUBE_MAP: return boundTexturesCubeMap_[activeTextureUnit_];
        case GL_TEXTURE_2D_ARRAY: return boundTextures2DArray_[activeTextureUnit_];
        case GL_TEXTURE_3D: return boundTextures3D_[activeTextureUnit_];
        default: return 0;
    }
}

VkTextureResource* WebGLVkContext::textureForTarget(GLenum target, uint8_t allowed) {
    const bool ok = ((allowed & k2DTargets) && (target == GL_TEXTURE_2D || isCubeFace(target))) ||
                    ((allowed & k3DTargets) && (target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY)) ||
                    ((allowed & kTextureTargets) && isTextureTarget(target));
    if (!ok) {
        setSyntheticError(GL_INVALID_ENUM);
        return nullptr;
    }
    const GLuint id = boundTexture(isCubeFace(target) ? GL_TEXTURE_CUBE_MAP : target);
    auto it = id != 0 ? textures_.find(id) : textures_.end();
    if (it == textures_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    return &it->second;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

// One sampling parameter of a texture or sampler object; false (with the GL
// error) for a name or value GL refuses.
bool WebGLVkContext::setSamplerParameter(SamplerState& state, GLenum pname, GLint i, GLfloat f, bool isFloat) {
    // Enum-valued names take the float form's value rounded, as GL does.
    const GLint v = isFloat ? static_cast<GLint>(std::lround(f)) : i;
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER:
        case GL_TEXTURE_MAG_FILTER:
            if (!validFilter(pname, v)) break;
            (pname == GL_TEXTURE_MIN_FILTER ? state.minFilter : state.magFilter) = static_cast<GLenum>(v);
            return true;
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
        case GL_TEXTURE_WRAP_R:
            if (v != GL_REPEAT && v != GL_CLAMP_TO_EDGE && v != GL_MIRRORED_REPEAT) break;
            (pname == GL_TEXTURE_WRAP_S ? state.wrapS : pname == GL_TEXTURE_WRAP_T ? state.wrapT : state.wrapR) =
                static_cast<GLenum>(v);
            return true;
        case GL_TEXTURE_MIN_LOD: state.minLod = isFloat ? f : static_cast<GLfloat>(i); return true;
        case GL_TEXTURE_MAX_LOD: state.maxLod = isFloat ? f : static_cast<GLfloat>(i); return true;
        case GL_TEXTURE_COMPARE_MODE:
            if (v != GL_NONE && v != GL_COMPARE_REF_TO_TEXTURE) break;
            state.compareMode = static_cast<GLenum>(v);
            return true;
        case GL_TEXTURE_COMPARE_FUNC:
            if (!validCompareFunc(v)) break;
            state.compareFunc = static_cast<GLenum>(v);
            return true;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT: {
            if (!context_.features().samplerAnisotropy) {
                setSyntheticError(GL_INVALID_ENUM);
                return false;
            }
            const GLfloat a = isFloat ? f : static_cast<GLfloat>(i);
            if (!(a >= 1.0f)) {
                setSyntheticError(GL_INVALID_VALUE);
                return false;
            }
            state.maxAnisotropy = a;
            return true;
        }
        default:
            setSyntheticError(GL_INVALID_ENUM);
            return false;
    }
    setSyntheticError(GL_INVALID_ENUM);  // a value the name does not take
    return false;
}

void WebGLVkContext::texParameteri(GLenum target, GLenum pname, GLint param) {
    VkTextureResource* tex = textureForTarget(target, kTextureTargets);
    if (!tex) return;
    if (pname == GL_TEXTURE_BASE_LEVEL || pname == GL_TEXTURE_MAX_LEVEL) {
        if (param < 0) {
            setSyntheticError(GL_INVALID_VALUE);
            return;
        }
        (pname == GL_TEXTURE_BASE_LEVEL ? tex->baseLevel : tex->maxLevel) = param;
        return;
    }
    setSamplerParameter(tex->sampler, pname, param, 0.0f, false);
}

void WebGLVkContext::texParameterf(GLenum target, GLenum pname, GLfloat param) {
    VkTextureResource* tex = textureForTarget(target, kTextureTargets);
    if (!tex) return;
    if (pname == GL_TEXTURE_BASE_LEVEL || pname == GL_TEXTURE_MAX_LEVEL) {
        texParameteri(target, pname, static_cast<GLint>(std::lround(param)));
        return;
    }
    setSamplerParameter(tex->sampler, pname, 0, param, true);
}

bool WebGLVkContext::getTexParameter(GLenum target, GLenum pname, TexParameter& out) {
    out = TexParameter{};
    VkTextureResource* tex = textureForTarget(target, kTextureTargets);
    if (!tex) return false;
    const SamplerState& s = tex->sampler;
    using Kind = TexParameter::Kind;
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: out.i = static_cast<GLint>(s.minFilter); return true;
        case GL_TEXTURE_MAG_FILTER: out.i = static_cast<GLint>(s.magFilter); return true;
        case GL_TEXTURE_WRAP_S: out.i = static_cast<GLint>(s.wrapS); return true;
        case GL_TEXTURE_WRAP_T: out.i = static_cast<GLint>(s.wrapT); return true;
        case GL_TEXTURE_WRAP_R: out.i = static_cast<GLint>(s.wrapR); return true;
        case GL_TEXTURE_COMPARE_MODE: out.i = static_cast<GLint>(s.compareMode); return true;
        case GL_TEXTURE_COMPARE_FUNC: out.i = static_cast<GLint>(s.compareFunc); return true;
        case GL_TEXTURE_BASE_LEVEL: out.i = tex->baseLevel; return true;
        case GL_TEXTURE_MAX_LEVEL: out.i = tex->maxLevel; return true;
        case GL_TEXTURE_IMMUTABLE_LEVELS: out.i = static_cast<GLint>(tex->immutableLevels); return true;
        case GL_TEXTURE_IMMUTABLE_FORMAT:
            out.i = tex->immutable ? 1 : 0;
            out.kind = Kind::Bool;
            return true;
        case GL_TEXTURE_MIN_LOD:
        case GL_TEXTURE_MAX_LOD:
            out.f = pname == GL_TEXTURE_MIN_LOD ? s.minLod : s.maxLod;
            out.kind = Kind::Float;
            return true;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            if (!context_.features().samplerAnisotropy) break;
            out.f = s.maxAnisotropy;
            out.kind = Kind::Float;
            return true;
        default: break;
    }
    setSyntheticError(GL_INVALID_ENUM);
    return false;
}

// ---------------------------------------------------------------------------
// Completeness
// ---------------------------------------------------------------------------

// ES 3.0 3.8.13: the base level is defined (every face of a cube map, square
// and the same size), and when the minification filter reads mipmaps every
// level from base to the last one of the chain (or MAX_LEVEL) is defined at
// its size in the same format. Integer textures, and depth textures read
// without comparison, are only complete with NEAREST filters; formats the
// device cannot filter, only without LINEAR. `count` is 1 for a texture the
// filter reads one level of.
bool WebGLVkContext::textureComplete(const VkTextureResource& tex, const SamplerState& state, uint32_t& base,
                                     uint32_t& count) const {
    if (!tex.isValid() || tex.levels.empty()) return false;
    const bool cube = tex.target == GL_TEXTURE_CUBE_MAP;
    const uint8_t allFaces = cube ? 0x3F : 0x01;
    uint32_t maxLevel = static_cast<uint32_t>(std::max(tex.maxLevel, 0));
    base = static_cast<uint32_t>(std::max(tex.baseLevel, 0));
    if (tex.immutable) {
        base = std::min(base, tex.immutableLevels - 1);
        maxLevel = std::clamp(maxLevel, base, tex.immutableLevels - 1);
    }
    if (base > maxLevel || base >= tex.levels.size()) return false;
    const TexLevel& b = tex.levels[base];
    if (b.faces != allFaces || b.width == 0 || b.format.format != tex.format || base >= tex.mipLevels) return false;
    if (cube && b.width != b.height) return false;

    count = 1;
    if (state.mipmapped()) {
        const uint32_t extent = std::max({b.width, b.height, tex.is3D() ? b.depth : 1u});
        const uint32_t last = std::min(base + static_cast<uint32_t>(std::floor(std::log2(extent))), maxLevel);
        for (uint32_t l = base + 1; l <= last; ++l) {
            if (l >= tex.levels.size()) return false;
            const TexLevel& lv = tex.levels[l];
            const uint32_t shift = l - base;
            const uint32_t depth = tex.is3D() ? std::max(1u, b.depth >> shift) : b.depth;
            if (lv.faces != allFaces || lv.width != std::max(1u, b.width >> shift) ||
                lv.height != std::max(1u, b.height >> shift) || lv.depth != depth ||
                lv.format.internalformat != b.format.internalformat || lv.format.format != b.format.format)
                return false;
        }
        count = last - base + 1;
    }

    const bool nearestOnly = state.magFilter == GL_NEAREST &&
                             (state.minFilter == GL_NEAREST || state.minFilter == GL_NEAREST_MIPMAP_NEAREST);
    const TexFormat& tf = b.format;
    if (tf.isInteger() && !nearestOnly) return false;
    if (tf.isDepthOrStencil() && state.compareMode == GL_NONE && !nearestOnly) return false;
    if (!nearestOnly && !tf.isDepthOrStencil() &&
        !formatSupports(tf.format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        return false;
    return true;
}

} // namespace bro::webgl::vk
