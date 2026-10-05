// Sampler objects, and what a draw binds for a sampler uniform: a view of
// the texture's sampled level range (swizzled to its GL format) and a
// VkSampler from the context's cache, one per distinct sampling state.

#include "webgl/vulkan/webgl_vk_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>

namespace bro::webgl::vk {

namespace {

VkSamplerAddressMode addressMode(GLenum wrap) {
    switch (wrap) {
        case GL_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case GL_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

VkImageViewType viewType(GLenum target) {
    switch (target) {
        case GL_TEXTURE_CUBE_MAP: return VK_IMAGE_VIEW_TYPE_CUBE;
        case GL_TEXTURE_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case GL_TEXTURE_3D: return VK_IMAGE_VIEW_TYPE_3D;
        default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

} // namespace

// A sampler for `state` reading a texture of `tf`. GL's NEAREST/LINEAR
// minification reads only the base level: the view then holds one level and
// the LOD is clamped to [0, 0.25], which keeps Vulkan's choice between the
// magnification and minification filters (made on the unclamped side of 0)
// while every LOD rounds to that level. Filters the format cannot do, the
// device does not do: an incomplete texture never gets here, so that only
// happens to depth formats, filtered nearest instead.
VkSampler WebGLVkContext::samplerFor(const SamplerState& state, const TexFormat& tf, bool mipmapped) {
    const bool linearOk = !tf.isInteger() && formatSupports(tf.format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = linearOk && state.magFilter == GL_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = linearOk && (state.minFilter == GL_LINEAR || state.minFilter == GL_LINEAR_MIPMAP_NEAREST ||
                                  state.minFilter == GL_LINEAR_MIPMAP_LINEAR)
                         ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = linearOk && mipmapped && (state.minFilter == GL_NEAREST_MIPMAP_LINEAR ||
                                                state.minFilter == GL_LINEAR_MIPMAP_LINEAR)
                          ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = addressMode(state.wrapS);
    info.addressModeV = addressMode(state.wrapT);
    info.addressModeW = addressMode(state.wrapR);
    if (mipmapped) {
        info.minLod = state.minLod;
        info.maxLod = std::max(state.maxLod, state.minLod);
    } else {
        // GL's own LOD clamp still decides between the two filters.
        info.minLod = std::clamp(state.minLod, 0.0f, 0.25f);
        info.maxLod = std::clamp(state.maxLod, info.minLod, 0.25f);
    }
    if (tf.isDepthOrStencil() && state.compareMode == GL_COMPARE_REF_TO_TEXTURE) {
        info.compareEnable = VK_TRUE;
        info.compareOp = glCompareOpToVk(state.compareFunc);
    }
    const float maxAnisotropy = context_.deviceProperties().limits.maxSamplerAnisotropy;
    if (context_.features().samplerAnisotropy && state.maxAnisotropy > 1.0f && linearOk) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::min(state.maxAnisotropy, maxAnisotropy);
    }

    SamplerKey key;
    key.bits = static_cast<uint32_t>(info.magFilter) | static_cast<uint32_t>(info.minFilter) << 1 |
               static_cast<uint32_t>(info.mipmapMode) << 2 | static_cast<uint32_t>(info.addressModeU) << 3 |
               static_cast<uint32_t>(info.addressModeV) << 6 | static_cast<uint32_t>(info.addressModeW) << 9 |
               info.compareEnable << 12 | static_cast<uint32_t>(info.compareOp) << 13 | info.anisotropyEnable << 16;
    key.minLod = info.minLod;
    key.maxLod = info.maxLod;
    key.anisotropy = info.anisotropyEnable ? info.maxAnisotropy : 0.0f;
    if (auto it = samplerCache_.find(key); it != samplerCache_.end()) return it->second;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(context_.device(), &info, nullptr, &sampler) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkContext: failed to create a sampler");
        return VK_NULL_HANDLE;
    }
    samplerCache_.emplace(key, sampler);
    return sampler;
}

bool WebGLVkContext::anisotropicFiltering() const { return context_.features().samplerAnisotropy == VK_TRUE; }

void WebGLVkContext::destroySamplerCache() {
    for (const auto& [key, sampler] : samplerCache_) vkDestroySampler(context_.device(), sampler, nullptr);
    samplerCache_.clear();
}

// A view of levels [base, base + count) of `tex`, every layer, through the
// swizzle that makes its storage read as its GL format. Depth reads as
// (D, 0, 0, 1), as ES 3.0 has it.
VkImageView WebGLVkContext::sampledView(VkTextureResource& tex, uint32_t base, uint32_t count) {
    const uint32_t key = base << 8 | count;
    for (const auto& [k, view] : tex.sampledViews)
        if (k == key) return view;
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = tex.image;
    info.viewType = viewType(tex.target);
    info.format = tex.format;
    info.components = tex.tf.swizzle;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (tex.tf.isDepthOrStencil()) {
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        info.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                           VK_COMPONENT_SWIZZLE_ONE};
    }
    info.subresourceRange = {aspect, base, count, 0, tex.arrayLayers};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(context_.device(), &info, nullptr, &view) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkContext: failed to create a texture view");
        return VK_NULL_HANDLE;
    }
    tex.sampledViews.emplace_back(key, view);
    return view;
}

// ---------------------------------------------------------------------------
// Sampler objects
// ---------------------------------------------------------------------------

WebGLSampler WebGLVkContext::createSampler() {
    GLuint id = nextObjectId_++;
    samplers_[id] = VkSamplerResource{};
    return {id};
}

void WebGLVkContext::deleteSampler(WebGLSampler s) {
    if (samplers_.erase(s.id) == 0) return;
    for (GLuint& unit : boundSamplers_)
        if (unit == s.id) unit = 0;
}

void WebGLVkContext::bindSampler(GLuint unit, WebGLSampler s) {
    if (unit >= static_cast<GLuint>(getParameterInt(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS))) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (s.id != 0 && samplers_.find(s.id) == samplers_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    boundSamplers_[unit] = s.id;
}

void WebGLVkContext::samplerParameteri(WebGLSampler s, GLenum pname, GLint param) {
    auto it = samplers_.find(s.id);
    if (it == samplers_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    setSamplerParameter(it->second.state, pname, param, 0.0f, false);
}

void WebGLVkContext::samplerParameterf(WebGLSampler s, GLenum pname, GLfloat param) {
    auto it = samplers_.find(s.id);
    if (it == samplers_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    setSamplerParameter(it->second.state, pname, 0, param, true);
}

GLint WebGLVkContext::getSamplerParameteri(WebGLSampler s, GLenum pname) {
    auto it = samplers_.find(s.id);
    if (it == samplers_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return 0;
    }
    const SamplerState& st = it->second.state;
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: return static_cast<GLint>(st.minFilter);
        case GL_TEXTURE_MAG_FILTER: return static_cast<GLint>(st.magFilter);
        case GL_TEXTURE_WRAP_S: return static_cast<GLint>(st.wrapS);
        case GL_TEXTURE_WRAP_T: return static_cast<GLint>(st.wrapT);
        case GL_TEXTURE_WRAP_R: return static_cast<GLint>(st.wrapR);
        case GL_TEXTURE_COMPARE_MODE: return static_cast<GLint>(st.compareMode);
        case GL_TEXTURE_COMPARE_FUNC: return static_cast<GLint>(st.compareFunc);
        default: setSyntheticError(GL_INVALID_ENUM); return 0;
    }
}

GLfloat WebGLVkContext::getSamplerParameterf(WebGLSampler s, GLenum pname) {
    auto it = samplers_.find(s.id);
    if (it == samplers_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return 0.0f;
    }
    switch (pname) {
        case GL_TEXTURE_MIN_LOD: return it->second.state.minLod;
        case GL_TEXTURE_MAX_LOD: return it->second.state.maxLod;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            if (context_.features().samplerAnisotropy) return it->second.state.maxAnisotropy;
            setSyntheticError(GL_INVALID_ENUM);
            return 0.0f;
        default: return static_cast<GLfloat>(getSamplerParameteri(s, pname));
    }
}

GLboolean WebGLVkContext::isSampler(WebGLSampler s) {
    return (s.id != 0 && samplers_.find(s.id) != samplers_.end()) ? GL_TRUE : GL_FALSE;
}

} // namespace bro::webgl::vk
