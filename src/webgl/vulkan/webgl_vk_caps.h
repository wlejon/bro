#pragma once

#include "webgl/webgl_types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::render { class VulkanContext; }

namespace bro::webgl::vk {

/// What a WebGL2 context on this device can do, taken once when the context
/// is made and never changed: the getParameter constants (limits, ranges,
/// strings), the extensions, the compressed formats each backs, the sample
/// counts of each renderable format, shader precisions, and what transform
/// feedback and timer queries need. Plain data, so the thread that answers
/// the page (WebGL2RenderingContext) and the thread that runs the backend
/// (WebGLVkContext) read the same snapshot without asking each other.
struct WebGLCaps {
    /// getParameter's integer limits by pname, exact (the 64-bit ones too),
    /// sorted by pname.
    std::vector<std::pair<GLenum, int64_t>> limits;
    /// getParameter's float limits by pname (MAX_TEXTURE_LOD_BIAS, and
    /// MAX_TEXTURE_MAX_ANISOTROPY_EXT where the device filters
    /// anisotropically), sorted by pname.
    std::vector<std::pair<GLenum, float>> floatLimits;
    GLint maxViewportDims[2]{};
    GLfloat aliasedPointSizeRange[2]{1.0f, 1.0f};
    GLfloat aliasedLineWidthRange[2]{1.0f, 1.0f};

    std::string version = "WebGL 2.0";
    std::string shadingLanguageVersion = "WebGL GLSL ES 3.00";
    std::string vendor = "Bro";
    std::string renderer = "Bro Vulkan";
    /// WEBGL_debug_renderer_info's UNMASKED_VENDOR_WEBGL /
    /// UNMASKED_RENDERER_WEBGL: the Vulkan device's vendor and name.
    std::string unmaskedVendor;
    std::string unmaskedRenderer;

    /// getSupportedExtensions, in the order it answers them.
    std::vector<std::string> extensions;
    /// The compressed-texture extensions the device samples natively, each
    /// with its formats (COMPRESSED_TEXTURE_FORMATS once enabled).
    struct CompressedExtension {
        std::string name;
        std::vector<GLint> formats;
    };
    std::vector<CompressedExtension> compressedExtensions;

    /// getInternalformatParameter(RENDERBUFFER, f, SAMPLES): the counts
    /// renderbufferStorageMultisample accepts for each renderable format f,
    /// highest first (none for integer formats), sorted by format.
    std::vector<std::pair<GLenum, std::vector<GLint>>> sampleCounts;

    /// getShaderPrecisionFormat: every precision is highp (shaders compile
    /// with precision qualifiers dropped: 32-bit floats and ints throughout).
    struct Precision {
        GLint rangeMin = 0, rangeMax = 0, precision = 0;
    };
    Precision floatPrecision{127, 127, 23};
    Precision intPrecision{31, 30, 0};

    bool transformFeedback = false;     // the vertex stage can store (createTransformFeedback)
    bool anisotropicFiltering = false;  // EXT_texture_filter_anisotropic
    uint32_t timestampBits = 0;         // 0: the graphics queue cannot time
    float timestampPeriodNs = 0.0f;
    bool timerQueries() const { return timestampBits > 0 && timestampPeriodNs > 0.0f; }

    /// A limit getParameter answers for `pname`; false when it is not one.
    bool limit(GLenum pname, int64_t& out) const;
    bool floatLimit(GLenum pname, float& out) const;
    /// The sample counts of `internalformat` (empty for any other format).
    const std::vector<GLint>& samples(GLenum internalformat) const;
    bool supports(const std::string& extension) const;
    const CompressedExtension* compressedExtension(const std::string& name) const;
};

/// The snapshot of `device`. Reads only the physical device's properties,
/// features and format support, so any thread may take it.
std::shared_ptr<const WebGLCaps> buildWebGLCaps(render::VulkanContext& device);

} // namespace bro::webgl::vk
