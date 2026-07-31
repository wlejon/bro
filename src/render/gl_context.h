#pragma once

#include <glad/gl.h>
#include <cstdint>
#include <cstddef>

namespace bro::platform { class Window; }

namespace bro::render {

// --- GL capabilities, latched once from the GL thread ------------------------
//
// Queried limits rather than the spec's guaranteed minimums, for the one case
// where the difference is the difference between a feature existing and not:
// combined texture image units. GL 3.3 guarantees 16 and every desktop driver
// this runs on reports far more (32 to 192 is the usual range), so hardcoding
// the guarantee spends a budget the hardware does not actually impose.
//
// This lives in the render layer because that is where a current context is,
// and scene/ may include render/ but not the other way round. It is latched in
// GLContext's constructor and read from anywhere afterwards.
//
// Both accessors are safe before the latch: they return the GL 3.3 guaranteed
// minimum, which is what the code assumed unconditionally before. A caller that
// runs early therefore gets the old, conservative answer rather than a wrong
// one — the failure mode is "fewer slots than the hardware has", never "more".
struct GLCaps {
    /// GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, never reported below the GL 3.3
    /// guaranteed minimum of 16 even if a driver claims less.
    static int combinedTextureImageUnits();

    /// Called once, from the GL thread, with a context current.
    static void latch();
};

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
