#pragma once

#include <string>
#include <glad/gl.h>

namespace bro::webgl {

/// Translate GLSL ES 3.00 (WebGL2) shader source to GLSL 3.30 (desktop GL).
///
/// Differences handled:
/// - #version 300 es → #version 330 core
/// - Strip precision qualifiers (precision highp float, etc.)
/// - Strip precision from variable declarations (highp vec3 → vec3)
std::string translateGLSL(const std::string& source, GLenum shaderType);

} // namespace bro::webgl
