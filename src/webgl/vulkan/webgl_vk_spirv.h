#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::webgl::vk::spirv {

/// Edits glslang's SPIR-V for a WebGL program after code generation, where
/// what GL wants is not something the GLSL source can say.

/// Drop every RelaxedPrecision decoration. GLSL ES precision qualifiers are
/// hints desktop GL ignored, and bro's WebGL ran on desktop GL: a mediump
/// float stays 32-bit rather than becoming whatever a Vulkan driver (Apple's,
/// through MoltenVK, in particular) makes of relaxed precision.
void stripRelaxedPrecision(std::vector<uint32_t>& words);

/// Set the Location decoration of the Input variables named in `locations`
/// (attributes placed by bindAttribLocation or the linker rather than by the
/// source). Returns false if a named input is not found.
bool setInputLocations(std::vector<uint32_t>& words,
                       const std::unordered_map<std::string, uint32_t>& locations);

} // namespace bro::webgl::vk::spirv
