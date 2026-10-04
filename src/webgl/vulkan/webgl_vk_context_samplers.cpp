#include "webgl/vulkan/webgl_vk_context.h"

#include <algorithm>

namespace bro::webgl::vk {

void WebGLVkContext::updateTextureSampler(VkTextureResource& tex) {
    if (tex.sampler != VK_NULL_HANDLE && !tex.samplerDirty) return;
    VkDevice dev = context_.device();
    releaseSampler(tex.sampler);
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = (tex.magFilter == GL_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = (tex.minFilter == GL_NEAREST || tex.minFilter == GL_NEAREST_MIPMAP_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.mipmapMode = (tex.minFilter == GL_LINEAR_MIPMAP_LINEAR || tex.minFilter == GL_NEAREST_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    auto toAddressMode = [](GLenum wrap) {
        if (wrap == GL_REPEAT) return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    };
    info.addressModeU = toAddressMode(tex.wrapS);
    info.addressModeV = toAddressMode(tex.wrapT);
    info.addressModeW = toAddressMode(tex.wrapS);
    info.minLod = 0.0f;
    bool hasMipmaps = (tex.minFilter != GL_NEAREST && tex.minFilter != GL_LINEAR);
    info.maxLod = (hasMipmaps && tex.mipLevels > 1) ? static_cast<float>(tex.mipLevels - 1) : 0.0f;
    vkCreateSampler(dev, &info, nullptr, &tex.sampler);
    tex.samplerDirty = false;
}
void WebGLVkContext::updateSamplerObject(VkSamplerResource& smp) {
    if (smp.sampler != VK_NULL_HANDLE && !smp.samplerDirty) return;
    VkDevice dev = context_.device();
    releaseSampler(smp.sampler);
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = (smp.magFilter == GL_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = (smp.minFilter == GL_NEAREST || smp.minFilter == GL_NEAREST_MIPMAP_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.mipmapMode = (smp.minFilter == GL_LINEAR_MIPMAP_LINEAR || smp.minFilter == GL_NEAREST_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    auto toAddressMode = [](GLenum wrap) {
        if (wrap == GL_REPEAT) return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (wrap == GL_MIRRORED_REPEAT) return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    };
    info.addressModeU = toAddressMode(smp.wrapS);
    info.addressModeV = toAddressMode(smp.wrapT);
    info.addressModeW = toAddressMode(smp.wrapR);
    bool hasMipmaps = (smp.minFilter != GL_NEAREST && smp.minFilter != GL_LINEAR);
    info.minLod = std::max(0.0f, smp.minLod);
    info.maxLod = hasMipmaps ? std::max(0.0f, smp.maxLod) : 0.0f;
    if (smp.compareMode == GL_COMPARE_REF_TO_TEXTURE) {
        info.compareEnable = VK_TRUE;
        info.compareOp = glCompareOpToVk(smp.compareFunc);
    }
    vkCreateSampler(dev, &info, nullptr, &smp.sampler);
    smp.samplerDirty = false;
}

WebGLSampler WebGLVkContext::createSampler() {
    GLuint id = nextSamplerId_++;
    samplers_[id] = VkSamplerResource{};
    return {id};
}

void WebGLVkContext::deleteSampler(WebGLSampler s) {
    auto it = samplers_.find(s.id);
    if (it != samplers_.end()) {
        releaseSampler(it->second.sampler);
        samplers_.erase(it);
    }
    for (size_t i = 0; i < boundSamplers_.size(); ++i) {
        if (boundSamplers_[i] == s.id) {
            boundSamplers_[i] = 0;
        }
    }
}

void WebGLVkContext::bindSampler(GLuint unit, WebGLSampler s) {
    if (unit < boundSamplers_.size()) {
        boundSamplers_[unit] = s.id;
    }
}

void WebGLVkContext::samplerParameteri(WebGLSampler s, GLenum pname, GLint param) {
    auto it = samplers_.find(s.id);
    if (it == samplers_.end()) return;
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: it->second.minFilter = param; break;
        case GL_TEXTURE_MAG_FILTER: it->second.magFilter = param; break;
        case GL_TEXTURE_WRAP_S: it->second.wrapS = param; break;
        case GL_TEXTURE_WRAP_T: it->second.wrapT = param; break;
        case GL_TEXTURE_WRAP_R: it->second.wrapR = param; break;
        case GL_TEXTURE_COMPARE_MODE: it->second.compareMode = param; break;
        case GL_TEXTURE_COMPARE_FUNC: it->second.compareFunc = param; break;
        default: break;
    }
    it->second.samplerDirty = true;
}

void WebGLVkContext::samplerParameterf(WebGLSampler s, GLenum pname, GLfloat param) {
    auto it = samplers_.find(s.id);
    if (it == samplers_.end()) return;
    switch (pname) {
        case GL_TEXTURE_MIN_LOD: it->second.minLod = param; break;
        case GL_TEXTURE_MAX_LOD: it->second.maxLod = param; break;
        default: samplerParameteri(s, pname, static_cast<GLint>(param)); return;
    }
    it->second.samplerDirty = true;
}

GLint WebGLVkContext::getSamplerParameteri(WebGLSampler s, GLenum pname) {
    auto it = samplers_.find(s.id);
    if (it == samplers_.end()) return 0;
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: return it->second.minFilter;
        case GL_TEXTURE_MAG_FILTER: return it->second.magFilter;
        case GL_TEXTURE_WRAP_S: return it->second.wrapS;
        case GL_TEXTURE_WRAP_T: return it->second.wrapT;
        case GL_TEXTURE_WRAP_R: return it->second.wrapR;
        case GL_TEXTURE_COMPARE_MODE: return it->second.compareMode;
        case GL_TEXTURE_COMPARE_FUNC: return it->second.compareFunc;
        default: return 0;
    }
}

GLfloat WebGLVkContext::getSamplerParameterf(WebGLSampler s, GLenum pname) {
    auto it = samplers_.find(s.id);
    if (it == samplers_.end()) return 0.0f;
    switch (pname) {
        case GL_TEXTURE_MIN_LOD: return it->second.minLod;
        case GL_TEXTURE_MAX_LOD: return it->second.maxLod;
        default: return static_cast<GLfloat>(getSamplerParameteri(s, pname));
    }
}

GLboolean WebGLVkContext::isSampler(WebGLSampler s) {
    return (s.id != 0 && samplers_.find(s.id) != samplers_.end()) ? GL_TRUE : GL_FALSE;
}
} // namespace bro::webgl::vk
