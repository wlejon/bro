// getParameter: state the context tracks, and the implementation limits of
// the capabilities snapshot (webgl_vk_caps.cpp).

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_formats.h"

#include <algorithm>
#include <climits>

namespace bro::webgl::vk {

// Bits of the draw framebuffer's buffers: color from DRAW_BUFFER0's
// attachment (the first drawn one), depth and stencil from theirs.
static FormatBits framebufferBits(VkFormat color, VkFormat depth, VkFormat stencil) {
    FormatBits bits = formatBits(color);
    bits.depth = formatBits(depth).depth;
    bits.stencil = formatBits(stencil).stencil;
    return bits;
}

GLint WebGLVkContext::getParameterInt(GLenum pname) {
    // The device's limits are the snapshot's (webgl_vk_caps.cpp).
    if (int64_t limit = 0; caps_->limit(pname, limit))
        return static_cast<GLint>(std::clamp<int64_t>(limit, INT_MIN, INT_MAX));
    switch (pname) {
        case GL_MAX_TEXTURE_LOD_BIAS: {
            float bias = 0.0f;
            caps_->floatLimit(pname, bias);
            return static_cast<GLint>(bias);
        }

        // --- Framebuffer state ---
        case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS:
        case GL_DEPTH_BITS: case GL_STENCIL_BITS: {
            FormatBits bits;
            if (drawFboId_ == 0) {
                bits = framebufferBits(canvasDrawBuffer_ == GL_BACK ? canvas_.colorFormat() : VK_FORMAT_UNDEFINED,
                                       canvas_.depthFormat(), canvas_.depthFormat());
                if (canvas_.opaque()) bits.alpha = 0;
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
        case GL_GENERATE_MIPMAP_HINT: return static_cast<GLint>(generateMipmapHint_);
        case GL_FRAGMENT_SHADER_DERIVATIVE_HINT: return static_cast<GLint>(derivativeHint_);
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
            GLenum format = 0, type = 0;
            implementationReadFormat(s, format, type);
            return static_cast<GLint>(pname == GL_IMPLEMENTATION_COLOR_READ_FORMAT ? format : type);
        }
        case GL_READ_BUFFER: return static_cast<GLint>(readBufferState());
        case GL_MAX_SHADER_COMPILER_THREADS_KHR:
            if (!parallelCompileEnabled_) break;
            return static_cast<GLint>(maxShaderCompilerThreads_);

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
        case GL_UNPACK_ALIGNMENT: case GL_PACK_ALIGNMENT: case GL_UNPACK_ROW_LENGTH: case GL_UNPACK_IMAGE_HEIGHT:
        case GL_UNPACK_SKIP_PIXELS: case GL_UNPACK_SKIP_ROWS: case GL_UNPACK_SKIP_IMAGES: case GL_PACK_ROW_LENGTH:
        case GL_PACK_SKIP_PIXELS: case GL_PACK_SKIP_ROWS: case GL_UNPACK_COLORSPACE_CONVERSION_WEBGL:
            return getPixelStorei(pname);
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
    setSyntheticError(GL_INVALID_ENUM);   // not a parameter WebGL 2 has
    return 0;
}

bool WebGLVkContext::formatSupports(VkFormat format, VkFormatFeatureFlags features) const {
    const auto idx = static_cast<uint32_t>(format);
    if (idx < formatFeaturesLow_.size()) {
        if (!formatFeaturesValid_[idx]) {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &props);
            formatFeaturesLow_[idx] = props.optimalTilingFeatures;
            formatFeaturesValid_[idx] = 1;
        }
        return (formatFeaturesLow_[idx] & features) == features;
    }
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &props);
    return (props.optimalTilingFeatures & features) == features;
}

bool WebGLVkContext::enableExtension(const std::string& name) {
    // A compressed-texture extension's formats are accepted once enabled.
    if (enableCompressedExtension(name)) return true;
    // TIME_ELAPSED_EXT, TIMESTAMP_EXT and the rest are enums from now on.
    if (name == "EXT_disjoint_timer_query_webgl2" && timerQueriesSupported()) {
        timerQueriesEnabled_ = true;
        return true;
    }
    if (!caps_->supports(name)) return false;
    // COMPLETION_STATUS_KHR and the UNMASKED_* strings are enums from now on.
    if (name == "KHR_parallel_shader_compile") parallelCompileEnabled_ = true;
    if (name == "WEBGL_debug_renderer_info") rendererInfoEnabled_ = true;
    return true;
}

bool WebGLVkContext::unmaskedRendererInfo(GLenum pname, std::string& out) {
    if (!rendererInfoEnabled_ || (pname != GL_UNMASKED_VENDOR_WEBGL && pname != GL_UNMASKED_RENDERER_WEBGL)) {
        setSyntheticError(GL_INVALID_ENUM);
        return false;
    }
    out = pname == GL_UNMASKED_RENDERER_WEBGL ? caps_->unmaskedRenderer : caps_->unmaskedVendor;
    return true;
}

// The limits GL types as 64-bit.
int64_t WebGLVkContext::getParameterInt64(GLenum pname) {
    if (int64_t limit = 0; caps_->limit(pname, limit)) return limit;
    return getParameterInt(pname);
}

GLfloat WebGLVkContext::getParameterFloat(GLenum pname) {
    if (float limit = 0.0f; caps_->floatLimit(pname, limit)) return limit;
    switch (pname) {
        case 0x0B73 /* GL_DEPTH_CLEAR_VALUE */: return clearDepth_;
        case GL_LINE_WIDTH: return lineWidth_;
        case GL_POLYGON_OFFSET_FACTOR: return polygonOffsetFactor_;
        case GL_POLYGON_OFFSET_UNITS: return polygonOffsetUnits_;
        case GL_SAMPLE_COVERAGE_VALUE: return sampleCoverageValue_;
        case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT:  // the device does not filter anisotropically
            setSyntheticError(GL_INVALID_ENUM);
            return 0.0f;
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
    if (pname == GL_SAMPLE_COVERAGE_INVERT) return sampleCoverageInvert_ ? GL_TRUE : GL_FALSE;
    return GL_FALSE;
}

void WebGLVkContext::getParameterInt2(GLenum pname, GLint* out) {
    out[0] = 0;
    out[1] = 0;
    if (pname == GL_MAX_VIEWPORT_DIMS) {
        out[0] = caps_->maxViewportDims[0];
        out[1] = caps_->maxViewportDims[1];
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
        out[0] = caps_->aliasedPointSizeRange[0];
        out[1] = caps_->aliasedPointSizeRange[1];
    } else if (pname == GL_ALIASED_LINE_WIDTH_RANGE) {
        out[0] = caps_->aliasedLineWidthRange[0];
        out[1] = caps_->aliasedLineWidthRange[1];
    } else if (pname == GL_DEPTH_RANGE) {
        out[0] = depthNear_;
        out[1] = depthFar_;
    }
}

void WebGLVkContext::getParameterFloat4(GLenum pname, GLfloat* out) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (pname == 0x0C22 /* GL_COLOR_CLEAR_VALUE */) {
        for (int i = 0; i < 4; ++i) out[i] = clearColor_[i];
    } else if (pname == GL_BLEND_COLOR) {
        for (int i = 0; i < 4; ++i) out[i] = blendColor_[i];
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
