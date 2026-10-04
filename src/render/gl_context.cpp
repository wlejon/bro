#include "render/gl_context.h"
#include "platform/sdl_window.h"
#include "util/log.h"

#include <atomic>
#include <cstring>

namespace bro::render {

// --- GL capabilities ---------------------------------------------------------

namespace {
std::atomic<int> gCombinedTextureUnits{32};
}

int GLCaps::combinedTextureImageUnits() {
    return gCombinedTextureUnits.load(std::memory_order_relaxed);
}

void GLCaps::latch() {
    gCombinedTextureUnits.store(32, std::memory_order_relaxed);
}

GLContext::GLContext(platform::Window& window) : window_(window) {
    GLCaps::latch();
    createPipelines();
    LOG_INFO("GLContext initialized (legacy GL purged, Vulkan native)");
}

GLContext::~GLContext() = default;

GLuint GLContext::compileShader(GLenum /*type*/, const char* /*source*/) {
    return 0;
}

GLuint GLContext::linkProgram(GLuint /*vs*/, GLuint /*fs*/) {
    return 0;
}

void GLContext::createPipelines() {
    colorViewportLoc_ = 0;
    textureViewportLoc_ = 0;
    textureSamplerLoc_ = 0;
}

GLuint GLContext::createTexture2D(uint32_t /*w*/, uint32_t /*h*/,
                                  GLenum /*internalFormat*/, GLenum /*format*/, GLenum /*type*/) {
    return 0;
}

void GLContext::uploadTexture2D(GLuint /*tex*/, const void* /*pixels*/,
                                 uint32_t /*w*/, uint32_t /*h*/,
                                 GLenum /*format*/, GLenum /*type*/) {
}

void GLContext::deleteTexture(GLuint /*tex*/) {
}

GLuint GLContext::createBuffer(uint32_t /*sizeBytes*/, GLenum /*usage*/) {
    return 0;
}

void GLContext::uploadBuffer(GLuint /*buf*/, const void* /*data*/, uint32_t /*sizeBytes*/) {
}

void GLContext::deleteBuffer(GLuint /*buf*/) {
}

void GLContext::swapBuffers() {
    window_.swapWindow();
}

} // namespace bro::render
