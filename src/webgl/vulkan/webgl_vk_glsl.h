#pragma once

#include "webgl/vulkan/webgl_vk_types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::webgl::vk::glsl {

/// WebGL's shading languages (GLSL ES 1.00 and 3.00) on Vulkan, through
/// glslang. A shader is compiled as desktop GLSL 450 with a preamble that
/// supplies what ES has and 450 lacks — the ES 1.00 keywords and texture
/// functions, gl_FragColor / gl_FragData, GL_ES — and, in the fragment stage,
/// GL's window space (gl_FragCoord.y, dFdy, gl_PointCoord through the
/// FragmentPush constant). The vertex stage's main is wrapped to map GL's
/// clip-space depth onto Vulkan's and give points a size. glslang links both
/// stages (matching varyings by name), maps every resource to a binding of
/// one descriptor set, and its reflection is what the program reports.

/// The device limits linking checks against.
struct Limits {
    uint32_t maxVertexAttribs = 16;
    uint32_t maxDrawBuffers = 8;
};

/// Check one shader's source; false with glslang's diagnostics in `log`.
bool compile(const std::string& source, GLenum type, std::string& log);

struct LinkResult {
    bool ok = false;
    std::string log;
    std::vector<uint32_t> vertSpirv;
    std::vector<uint32_t> fragSpirv;
    ProgramInterface iface;
};

/// Link a vertex and a fragment shader. `boundAttribs` are the program's
/// bindAttribLocation calls; a location given in the source takes precedence.
/// With feedback varyings, the vertex stage also stores them (see
/// kFeedbackBinding), and linking fails if one is not an output of it or the
/// set exceeds the WebGL 2 limits.
LinkResult link(const std::string& vertexSource, const std::string& fragmentSource,
                const std::unordered_map<std::string, GLuint>& boundAttribs, const Limits& limits,
                const FeedbackRequest& feedback = {});

/// What a GLSL type enum (GL_FLOAT_VEC3, GL_FLOAT_MAT2x4, GL_SAMPLER_2D...) is
/// made of: component kind, and columns x rows (a vector is 1 x n).
struct TypeInfo {
    enum class Kind : uint8_t { Float, Int, Uint, Bool, Sampler, Unknown } kind = Kind::Unknown;
    uint8_t columns = 1;
    uint8_t rows = 1;
    bool isMatrix() const { return columns > 1; }
    uint32_t components() const { return static_cast<uint32_t>(columns) * rows; }
};
TypeInfo typeInfo(GLenum type);

} // namespace bro::webgl::vk::glsl
