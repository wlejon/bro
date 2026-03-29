#pragma once

#include <glad/gl.h>
#include <string>
#include <cstdint>

namespace bro::webgl {

// Each WebGL object wraps a GLuint handle. These are lightweight value types
// used as opaque handles in the JS bindings. The ID 0 is reserved/invalid
// for most types (matching WebGL spec where null represents "no object").

struct WebGLBuffer       { GLuint id = 0; };
struct WebGLTexture      { GLuint id = 0; };
struct WebGLProgram      { GLuint id = 0; };
struct WebGLShader       { GLuint id = 0; GLenum type = 0; };
struct WebGLFramebuffer  { GLuint id = 0; };
struct WebGLRenderbuffer { GLuint id = 0; };
struct WebGLVertexArrayObject { GLuint id = 0; };
struct WebGLSampler      { GLuint id = 0; };
struct WebGLQuery        { GLuint id = 0; };
struct WebGLSync         { GLsync sync = nullptr; };
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

struct WebGLShaderPrecisionFormat {
    GLint rangeMin = 0;
    GLint rangeMax = 0;
    GLint precision = 0;
};

} // namespace bro::webgl
