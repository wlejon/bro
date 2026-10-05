#pragma once

#include "webgl/webgl_types.h"
#include <string>
#include <cstdint>

namespace bro::webgl {

// Each WebGL object is a name the backend issues (one sequence for every
// kind). These are lightweight value types the JS binding wraps; 0 is no
// object, which WebGL spells null.

struct WebGLBuffer       { GLuint id = 0; };
struct WebGLTexture      { GLuint id = 0; };
struct WebGLProgram      { GLuint id = 0; };
struct WebGLShader       { GLuint id = 0; GLenum type = 0; };
struct WebGLFramebuffer  { GLuint id = 0; };
struct WebGLRenderbuffer { GLuint id = 0; };
struct WebGLVertexArrayObject { GLuint id = 0; };
struct WebGLSampler      { GLuint id = 0; };
struct WebGLQuery        { GLuint id = 0; };
struct WebGLSync         { GLuint id = 0; };
struct WebGLTransformFeedback { GLuint id = 0; };

struct WebGLUniformLocation {
    GLint location = -1;
    GLuint program = 0;
};

struct WebGLActiveInfo {
    std::string name;
    GLenum type = 0;
    GLint size = 0;
};

} // namespace bro::webgl
