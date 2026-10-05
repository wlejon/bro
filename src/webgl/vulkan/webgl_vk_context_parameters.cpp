// getParameter: state the context tracks, and implementation limits read
// from the Vulkan device rather than assumed, so a page sizing itself by
// them asks for what this device (and this implementation) can do.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_formats.h"

#include <algorithm>
#include <climits>

namespace bro::webgl::vk {

namespace {

GLint clampInt(uint64_t v) { return static_cast<GLint>(std::min<uint64_t>(v, INT_MAX)); }

// The highest count in a VkSampleCountFlags mask.
GLint highestSampleCount(VkSampleCountFlags flags) {
    for (GLint n = 64; n > 1; n /= 2)
        if (flags & static_cast<VkSampleCountFlags>(n)) return n;
    return 0;
}

} // namespace

// Bits of the draw framebuffer's buffers: color from DRAW_BUFFER0's
// attachment (the first drawn one), depth and stencil from theirs.
static FormatBits framebufferBits(VkFormat color, VkFormat depth, VkFormat stencil) {
    FormatBits bits = formatBits(color);
    bits.depth = formatBits(depth).depth;
    bits.stencil = formatBits(stencil).stencil;
    return bits;
}

GLint WebGLVkContext::getParameterInt(GLenum pname) {
    const VkPhysicalDeviceLimits& L = context_.deviceProperties().limits;
    switch (pname) {
        // --- Texture and framebuffer sizes ---
        case GL_MAX_TEXTURE_SIZE: return clampInt(L.maxImageDimension2D);
        case GL_MAX_CUBE_MAP_TEXTURE_SIZE: return clampInt(L.maxImageDimensionCube);
        case 0x8073 /* GL_MAX_3D_TEXTURE_SIZE */: return clampInt(L.maxImageDimension3D);
        case 0x88FF /* GL_MAX_ARRAY_TEXTURE_LAYERS */: return clampInt(L.maxImageArrayLayers);
        case GL_MAX_RENDERBUFFER_SIZE:
            return clampInt(std::min({L.maxImageDimension2D, L.maxFramebufferWidth, L.maxFramebufferHeight}));
        case GL_MAX_DRAW_BUFFERS:
        case GL_MAX_COLOR_ATTACHMENTS:
            return clampInt(std::min({8u, L.maxColorAttachments, L.maxFragmentOutputAttachments}));
        case GL_MAX_SAMPLES:
            return highestSampleCount(L.framebufferColorSampleCounts & L.framebufferDepthSampleCounts &
                                      L.framebufferStencilSampleCounts);
        case GL_MAX_TEXTURE_LOD_BIAS: return static_cast<GLint>(L.maxSamplerLodBias);
        case GL_MIN_PROGRAM_TEXEL_OFFSET: return L.minTexelOffset;
        case GL_MAX_PROGRAM_TEXEL_OFFSET: return clampInt(L.maxTexelOffset);
        case GL_SUBPIXEL_BITS: return clampInt(L.subPixelPrecisionBits);

        // --- Shader resources ---
        // Attributes: the VAO and the generic-attribute block hold 16.
        case GL_MAX_VERTEX_ATTRIBS: return clampInt(std::min(16u, L.maxVertexInputAttributes));
        // The default uniform block is a uniform buffer.
        case GL_MAX_VERTEX_UNIFORM_VECTORS:
        case GL_MAX_FRAGMENT_UNIFORM_VECTORS:
            return clampInt(std::min<uint64_t>(L.maxUniformBufferRange, 65536) / 16);
        case GL_MAX_VERTEX_UNIFORM_COMPONENTS:
        case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:
            return getParameterInt(GL_MAX_FRAGMENT_UNIFORM_VECTORS) * 4;
        case GL_MAX_VARYING_VECTORS:
            return clampInt(std::min(L.maxVertexOutputComponents, L.maxFragmentInputComponents) / 4);
        case GL_MAX_VARYING_COMPONENTS: return getParameterInt(GL_MAX_VARYING_VECTORS) * 4;
        case GL_MAX_VERTEX_OUTPUT_COMPONENTS: return clampInt(L.maxVertexOutputComponents);
        case GL_MAX_FRAGMENT_INPUT_COMPONENTS: return clampInt(L.maxFragmentInputComponents);
        // Samplers: one descriptor set per program, 32 texture units. Each
        // sampler is visible only to the stages that use it, so a stage is
        // bounded by the per-stage limit and the program by the set limit.
        case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:
        case GL_MAX_TEXTURE_IMAGE_UNITS:
            return clampInt(std::min({16u, L.maxPerStageDescriptorSamplers, L.maxPerStageDescriptorSampledImages}));
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
            return clampInt(std::min({32u, L.maxDescriptorSetSamplers, L.maxDescriptorSetSampledImages,
                                      2 * static_cast<uint32_t>(getParameterInt(GL_MAX_TEXTURE_IMAGE_UNITS))}));
        // Uniform blocks: the default block takes one uniform-buffer slot.
        case GL_MAX_UNIFORM_BUFFER_BINDINGS: return static_cast<GLint>(kMaxUniformBufferBindings);
        case GL_MAX_VERTEX_UNIFORM_BLOCKS:
        case GL_MAX_FRAGMENT_UNIFORM_BLOCKS:
        case GL_MAX_COMBINED_UNIFORM_BLOCKS:
            return clampInt(std::min<uint64_t>({kMaxUniformBufferBindings, L.maxPerStageDescriptorUniformBuffers - 1,
                                                L.maxDescriptorSetUniformBuffers - 1}));
        case 0x8A34 /* GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT */: return clampInt(L.minUniformBufferOffsetAlignment);
        case GL_MAX_UNIFORM_BLOCK_SIZE:
        case GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS:
        case GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS:
        case GL_MAX_ELEMENT_INDEX:
        case GL_MAX_SERVER_WAIT_TIMEOUT:
            return clampInt(static_cast<uint64_t>(getParameterInt64(pname)));
        case GL_MAX_ELEMENTS_VERTICES:
        case GL_MAX_ELEMENTS_INDICES: return clampInt(L.maxDrawIndexedIndexValue);
        // Transform feedback is not implemented: its limits are what it can hold.
        case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS:
        case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS:
        case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS: return 0;

        // --- Framebuffer state ---
        case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS:
        case GL_DEPTH_BITS: case GL_STENCIL_BITS: {
            FormatBits bits;
            if (drawFboId_ == 0) {
                bits = framebufferBits(canvasDrawBuffer_ == GL_BACK ? canvas_.colorFormat() : VK_FORMAT_UNDEFINED,
                                       canvas_.depthFormat(), canvas_.depthFormat());
            } else if (auto it = framebuffers_.find(drawFboId_); it != framebuffers_.end()) {
                const VkFramebufferResource& fbo = it->second;
                VkFormat color = VK_FORMAT_UNDEFINED;
                for (GLenum db : fbo.drawBuffers) {
                    if (db == GL_NONE) continue;
                    color = attachmentSurface(fbo.color[db - GL_COLOR_ATTACHMENT0]).format;
                    break;
                }
                bits = framebufferBits(color, attachmentSurface(fbo.depth).format,
                                       attachmentSurface(fbo.stencil).format);
            }
            switch (pname) {
                case GL_RED_BITS: return bits.red;
                case GL_GREEN_BITS: return bits.green;
                case GL_BLUE_BITS: return bits.blue;
                case GL_ALPHA_BITS: return bits.alpha;
                case GL_DEPTH_BITS: return bits.depth;
                default: return bits.stencil;
            }
        }
        case GL_SAMPLES:
        case GL_SAMPLE_BUFFERS: {
            RenderTarget target;
            const GLenum saved = pendingError_;
            const bool complete = drawTarget(target);
            pendingError_ = saved;  // a query raises no error for an incomplete framebuffer
            const GLint samples = complete && target.samples > VK_SAMPLE_COUNT_1_BIT
                                      ? static_cast<GLint>(target.samples) : 0;
            return pname == GL_SAMPLES ? samples : (samples > 0 ? 1 : 0);
        }
        case GL_IMPLEMENTATION_COLOR_READ_FORMAT:
        case GL_IMPLEMENTATION_COLOR_READ_TYPE: {
            Surface s;
            const GLenum saved = pendingError_;
            const bool complete = readColorSurface(s);
            if (!complete || !s) {
                pendingError_ = saved;
                setSyntheticError(GL_INVALID_OPERATION);
                return 0;
            }
            const bool integer = isIntegerFormat(s.format);
            if (pname == GL_IMPLEMENTATION_COLOR_READ_FORMAT) return integer ? GL_RGBA_INTEGER : GL_RGBA;
            if (integer) return isSignedIntegerFormat(s.format) ? GL_INT : GL_UNSIGNED_INT;
            const FormatBits bits = formatBits(s.format);
            return bits.red == 8 || s.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? GL_UNSIGNED_BYTE : GL_FLOAT;
        }
        case GL_READ_BUFFER: return static_cast<GLint>(readBufferState());

        // --- Tracked state ---
        case GL_DEPTH_FUNC: return depthFunc_;
        case GL_BLEND_SRC_RGB: return blendSrcRGB_;
        case GL_BLEND_DST_RGB: return blendDstRGB_;
        case GL_BLEND_SRC_ALPHA: return blendSrcAlpha_;
        case GL_BLEND_DST_ALPHA: return blendDstAlpha_;
        case 0x8009 /* GL_BLEND_EQUATION_RGB */: return blendEqRGB_;
        case 0x883D /* GL_BLEND_EQUATION_ALPHA */: return blendEqAlpha_;
        case 0x0B45 /* GL_CULL_FACE_MODE */: return cullFaceMode_;
        case 0x0B46 /* GL_FRONT_FACE */: return frontFaceMode_;
        case 0x84E0 /* GL_ACTIVE_TEXTURE */: return activeTextureUnit_ + GL_TEXTURE0;
        case GL_UNPACK_ALIGNMENT: return unpackAlignment_;
        case GL_PACK_ALIGNMENT: return packAlignment_;
        case GL_STENCIL_WRITEMASK: return stencilWriteMaskFront_;
        case GL_STENCIL_BACK_WRITEMASK: return stencilWriteMaskBack_;
        case GL_STENCIL_CLEAR_VALUE: return clearStencil_;
        case GL_STENCIL_FUNC: return stencilFuncFront_;
        case GL_STENCIL_VALUE_MASK: return stencilValueMaskFront_;
        case GL_STENCIL_REF: return stencilRefFront_;
        case GL_STENCIL_FAIL: return stencilFailFront_;
        case GL_STENCIL_PASS_DEPTH_FAIL: return stencilPassDepthFailFront_;
        case GL_STENCIL_PASS_DEPTH_PASS: return stencilPassDepthPassFront_;
        case GL_STENCIL_BACK_FUNC: return stencilFuncBack_;
        case GL_STENCIL_BACK_VALUE_MASK: return stencilValueMaskBack_;
        case GL_STENCIL_BACK_REF: return stencilRefBack_;
        case GL_STENCIL_BACK_FAIL: return stencilFailBack_;
        case GL_STENCIL_BACK_PASS_DEPTH_FAIL: return stencilPassDepthFailBack_;
        case GL_STENCIL_BACK_PASS_DEPTH_PASS: return stencilPassDepthPassBack_;
        default: break;
    }
    if (pname >= GL_DRAW_BUFFER0 && pname < GL_DRAW_BUFFER0 + 8)
        return static_cast<GLint>(drawBufferState(pname - GL_DRAW_BUFFER0));
    return 0;
}

bool WebGLVkContext::formatSupports(VkFormat format, VkFormatFeatureFlags features) const {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &props);
    return (props.optimalTilingFeatures & features) == features;
}

// The limits GL types as 64-bit.
int64_t WebGLVkContext::getParameterInt64(GLenum pname) {
    const VkPhysicalDeviceLimits& L = context_.deviceProperties().limits;
    switch (pname) {
        case GL_MAX_UNIFORM_BLOCK_SIZE: return static_cast<int64_t>(L.maxUniformBufferRange);
        case GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS:
        case GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS:
            return static_cast<int64_t>(getParameterInt(GL_MAX_COMBINED_UNIFORM_BLOCKS)) *
                       getParameterInt64(GL_MAX_UNIFORM_BLOCK_SIZE) / 4 +
                   getParameterInt(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS);
        case GL_MAX_ELEMENT_INDEX: return static_cast<int64_t>(L.maxDrawIndexedIndexValue);
        // waitSync has nothing to wait on: GPU work is ordered on one queue.
        case GL_MAX_SERVER_WAIT_TIMEOUT: return 0;
        default: return getParameterInt(pname);
    }
}

GLfloat WebGLVkContext::getParameterFloat(GLenum pname) {
    switch (pname) {
        case 0x0B73 /* GL_DEPTH_CLEAR_VALUE */: return clearDepth_;
        case GL_LINE_WIDTH: return 1.0f;
        case GL_MAX_TEXTURE_LOD_BIAS: return context_.deviceProperties().limits.maxSamplerLodBias;
        case 0x80AA /* GL_SAMPLE_COVERAGE_VALUE */: return 1.0f;
        default: return 0.0f;
    }
}

GLboolean WebGLVkContext::getParameterBool(GLenum pname) {
    if (pname == GL_BLEND) return blendEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == GL_DEPTH_TEST) return depthTestEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == 0x0B72 /* GL_DEPTH_WRITEMASK */) return depthMask_;
    if (pname == GL_CULL_FACE) return cullFaceEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == GL_SCISSOR_TEST) return scissorTest_ ? GL_TRUE : GL_FALSE;
    if (pname == GL_STENCIL_TEST) return stencilTestEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == 0x0BD0 /* GL_DITHER */) return ditherEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == 0x8037 /* GL_POLYGON_OFFSET_FILL */) return polygonOffsetFillEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == 0x8C89 /* GL_RASTERIZER_DISCARD */) return rasterizerDiscardEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == 0x809E /* GL_SAMPLE_ALPHA_TO_COVERAGE */) return sampleAlphaToCoverageEnabled_ ? GL_TRUE : GL_FALSE;
    if (pname == 0x80A0 /* GL_SAMPLE_COVERAGE */) return sampleCoverageEnabled_ ? GL_TRUE : GL_FALSE;
    return GL_FALSE;
}

void WebGLVkContext::getParameterInt2(GLenum pname, GLint* out) {
    out[0] = 0;
    out[1] = 0;
    if (pname == GL_MAX_VIEWPORT_DIMS) {
        const VkPhysicalDeviceLimits& L = context_.deviceProperties().limits;
        out[0] = clampInt(std::min(L.maxViewportDimensions[0], L.maxFramebufferWidth));
        out[1] = clampInt(std::min(L.maxViewportDimensions[1], L.maxFramebufferHeight));
    }
}

void WebGLVkContext::getParameterInt4(GLenum pname, GLint* out) {
    if (pname == GL_VIEWPORT) {
        out[0] = static_cast<GLint>(viewport_.x);
        out[1] = static_cast<GLint>(viewport_.y);
        out[2] = static_cast<GLint>(viewport_.width);
        out[3] = static_cast<GLint>(viewport_.height);
    } else if (pname == GL_SCISSOR_BOX) {
        out[0] = scissor_.offset.x;
        out[1] = scissor_.offset.y;
        out[2] = static_cast<GLint>(scissor_.extent.width);
        out[3] = static_cast<GLint>(scissor_.extent.height);
    } else {
        out[0] = out[1] = out[2] = out[3] = 0;
    }
}

void WebGLVkContext::getParameterFloat2(GLenum pname, GLfloat* out) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    if (pname == 0x846D /* GL_ALIASED_POINT_SIZE_RANGE */ || pname == 0x0B12 /* GL_POINT_SIZE_RANGE */) {
        // gl_PointSize beyond 1 needs the largePoints feature.
        const VkPhysicalDeviceLimits& L = context_.deviceProperties().limits;
        out[0] = 1.0f;
        out[1] = context_.largePoints() ? L.pointSizeRange[1] : 1.0f;
    } else if (pname == GL_ALIASED_LINE_WIDTH_RANGE) {
        out[0] = 1.0f;  // lines are always one pixel wide
        out[1] = 1.0f;
    } else if (pname == 0x0B70 /* GL_DEPTH_RANGE */) {
        out[1] = 1.0f;
    }
}

void WebGLVkContext::getParameterFloat4(GLenum pname, GLfloat* out) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (pname == 0x0C22 /* GL_COLOR_CLEAR_VALUE */) {
        for (int i = 0; i < 4; ++i) out[i] = clearColor_[i];
    }
}

void WebGLVkContext::getParameterBool4(GLenum pname, GLboolean* out) {
    if (pname == 0x0C23 /* GL_COLOR_WRITEMASK */) {
        for (int i = 0; i < 4; ++i) out[i] = colorMask_[i];
    } else {
        out[0] = out[1] = out[2] = out[3] = GL_TRUE;
    }
}

} // namespace bro::webgl::vk
