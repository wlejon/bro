#pragma once

#include <string>
#include <glad/gl.h>

namespace bro::webgl {

/// Translate WebGL shader source to GLSL 3.30 core (desktop GL).
///
/// Two source dialects arrive here, told apart by the #version line:
/// - GLSL ES 3.00 (`#version 300 es`): the line becomes `#version 330 core`.
/// - Version-less source, which WebGL defines to be GLSL ES 1.00. That
///   dialect (attribute/varying/gl_FragColor/texture2D — including sources
///   that reach it through their own `#ifdef GL_ES` compatibility blocks,
///   as pixi's shader templates do) is lifted to 3.30 core by a prelude of
///   defines; see the comment in the .cpp for how, and why GL_ES is defined.
///
/// Both paths strip precision qualifiers (statements and inline).
std::string translateGLSL(const std::string& source, GLenum shaderType);

} // namespace bro::webgl
