#include "render/gl_context.h"
#include "platform/sdl_window.h"
#include "util/log.h"

#include <atomic>
#include <stdexcept>
#include <cstring>

namespace bro::render {

// ---------------------------------------------------------------------------
// GLSL shader sources (equivalent to the old HLSL color/texture shaders)
// ---------------------------------------------------------------------------

static const char* colorVertSrc = R"(
#version 330 core
uniform vec2 viewportSize;
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
out vec4 vColor;
void main() {
    vec2 ndc = (aPos / viewportSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vColor = aColor;
}
)";

static const char* colorFragSrc = R"(
#version 330 core
in vec4 vColor;
out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)";

static const char* textureVertSrc = R"(
#version 330 core
uniform vec2 viewportSize;
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    vec2 ndc = (aPos / viewportSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aUV;
}
)";

static const char* textureFragSrc = R"(
#version 330 core
uniform sampler2D uTexture;
in vec2 vUV;
out vec4 fragColor;
void main() {
    fragColor = texture(uTexture, vUV);
}
)";

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

// --- GL capabilities ---------------------------------------------------------

namespace {
// Relaxed atomic: written once from the GL thread during context construction,
// read from the JS thread when a node decides whether a sampler slot is
// available. Any reader that races the latch sees the GL 3.3 floor, which is
// the conservative answer and the one the code used unconditionally before.
std::atomic<int> gCombinedTextureUnits{16};
}

int GLCaps::combinedTextureImageUnits() {
    return gCombinedTextureUnits.load(std::memory_order_relaxed);
}

void GLCaps::latch() {
    GLint units = 0;
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
    // Clamp UP to the GL 3.3 guarantee. A driver reporting less than the spec
    // minimum is either lying or broken, and trusting it would shrink a budget
    // that already worked.
    if (units < 16) units = 16;
    gCombinedTextureUnits.store(units, std::memory_order_relaxed);
}

GLContext::GLContext(platform::Window& window) : window_(window) {
    GLCaps::latch();
    createPipelines();
    LOG_INFO("GL pipelines created (color + texture), %d combined texture units",
             GLCaps::combinedTextureImageUnits());
}

GLContext::~GLContext() {
    if (colorVAO_) glDeleteVertexArrays(1, &colorVAO_);
    if (textureVAO_) glDeleteVertexArrays(1, &textureVAO_);
    if (colorProgram_) glDeleteProgram(colorProgram_);
    if (textureProgram_) glDeleteProgram(textureProgram_);
}

// ---------------------------------------------------------------------------
// Shader compilation
// ---------------------------------------------------------------------------

GLuint GLContext::compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOG_ERROR("Shader compilation failed: %s", log);
        glDeleteShader(shader);
        throw std::runtime_error(std::string("Shader compilation failed: ") + log);
    }
    return shader;
}

GLuint GLContext::linkProgram(GLuint vs, GLuint fs) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        LOG_ERROR("Program link failed: %s", log);
        glDeleteProgram(prog);
        throw std::runtime_error(std::string("Program link failed: ") + log);
    }
    return prog;
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

void GLContext::createPipelines() {
    // --- Color pipeline ---
    {
        GLuint vs = compileShader(GL_VERTEX_SHADER, colorVertSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, colorFragSrc);
        colorProgram_ = linkProgram(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);

        colorViewportLoc_ = glGetUniformLocation(colorProgram_, "viewportSize");

        // Create VAO with color vertex layout
        glGenVertexArrays(1, &colorVAO_);
        glBindVertexArray(colorVAO_);
        // Attributes are configured when a buffer is bound before drawing
        glBindVertexArray(0);
    }

    // --- Texture pipeline ---
    {
        GLuint vs = compileShader(GL_VERTEX_SHADER, textureVertSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, textureFragSrc);
        textureProgram_ = linkProgram(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);

        textureViewportLoc_ = glGetUniformLocation(textureProgram_, "viewportSize");
        textureSamplerLoc_ = glGetUniformLocation(textureProgram_, "uTexture");

        // Create VAO with texture vertex layout
        glGenVertexArrays(1, &textureVAO_);
        glBindVertexArray(textureVAO_);
        glBindVertexArray(0);
    }
}

// ---------------------------------------------------------------------------
// Texture helpers
// ---------------------------------------------------------------------------

GLuint GLContext::createTexture2D(uint32_t w, uint32_t h,
                                  GLenum internalFormat, GLenum format, GLenum type) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, w, h, 0, format, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

void GLContext::uploadTexture2D(GLuint tex, const void* pixels,
                                 uint32_t w, uint32_t h,
                                 GLenum format, GLenum type) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, type, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GLContext::deleteTexture(GLuint tex) {
    if (tex) glDeleteTextures(1, &tex);
}

// ---------------------------------------------------------------------------
// Buffer helpers
// ---------------------------------------------------------------------------

GLuint GLContext::createBuffer(uint32_t sizeBytes, GLenum usage) {
    GLuint buf = 0;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferData(GL_ARRAY_BUFFER, sizeBytes, nullptr, usage);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return buf;
}

void GLContext::uploadBuffer(GLuint buf, const void* data, uint32_t sizeBytes) {
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeBytes, data);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GLContext::deleteBuffer(GLuint buf) {
    if (buf) glDeleteBuffers(1, &buf);
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

void GLContext::swapBuffers() {
    window_.swapWindow();
}

} // namespace bro::render
