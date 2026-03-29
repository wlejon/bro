#pragma once

#include <glad/gl.h>
#include <cstdint>
#include <cstddef>

namespace bro::platform { class Window; }

namespace bro::render {

struct ColorVertex {
    float x, y;
    float r, g, b, a;
};

struct TextureVertex {
    float x, y;
    float u, v;
};

class GLContext {
public:
    explicit GLContext(platform::Window& window);
    ~GLContext();

    GLContext(const GLContext&) = delete;
    GLContext& operator=(const GLContext&) = delete;

    // --- Pipeline access (shader programs) ---
    GLuint colorProgram() const { return colorProgram_; }
    GLuint textureProgram() const { return textureProgram_; }

    // --- Uniform locations ---
    GLint colorViewportLoc() const { return colorViewportLoc_; }
    GLint textureViewportLoc() const { return textureViewportLoc_; }
    GLint textureSamplerLoc() const { return textureSamplerLoc_; }

    // --- Texture helpers ---
    GLuint createTexture2D(uint32_t w, uint32_t h, GLenum internalFormat, GLenum format, GLenum type);
    void uploadTexture2D(GLuint tex, const void* pixels, uint32_t w, uint32_t h, GLenum format, GLenum type);
    void deleteTexture(GLuint tex);

    // --- Buffer helpers ---
    GLuint createBuffer(uint32_t sizeBytes, GLenum usage = GL_DYNAMIC_DRAW);
    void uploadBuffer(GLuint buf, const void* data, uint32_t sizeBytes);
    void deleteBuffer(GLuint buf);

    // --- VAO for color pipeline ---
    GLuint colorVAO() const { return colorVAO_; }
    // --- VAO for texture pipeline ---
    GLuint textureVAO() const { return textureVAO_; }

    // --- Frame lifecycle ---
    void swapBuffers();

    platform::Window& window() { return window_; }

private:
    GLuint compileShader(GLenum type, const char* source);
    GLuint linkProgram(GLuint vs, GLuint fs);
    void createPipelines();

    platform::Window& window_;

    GLuint colorProgram_ = 0;
    GLuint textureProgram_ = 0;

    GLint colorViewportLoc_ = -1;
    GLint textureViewportLoc_ = -1;
    GLint textureSamplerLoc_ = -1;

    // VAOs with vertex format pre-configured
    GLuint colorVAO_ = 0;
    GLuint textureVAO_ = 0;
};

} // namespace bro::render
