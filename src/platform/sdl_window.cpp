#include "platform/sdl_window.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <stdexcept>

namespace bro::platform {

Window::Window(const std::string& title, uint32_t width, uint32_t height)
    : m_width(width), m_height(height)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR("Failed to initialize SDL: %s", SDL_GetError());
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    // Request OpenGL 3.3 Core context
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL;
    m_window = SDL_CreateWindow(title.c_str(),
                                static_cast<int>(width),
                                static_cast<int>(height),
                                flags);

    if (!m_window) {
        LOG_ERROR("Failed to create SDL window: %s", SDL_GetError());
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }

    // Create OpenGL context
    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext) {
        LOG_ERROR("Failed to create GL context: %s", SDL_GetError());
        throw std::runtime_error(std::string("SDL_GL_CreateContext failed: ") + SDL_GetError());
    }

    // Load OpenGL functions via glad
    int version = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);
    if (!version) {
        throw std::runtime_error("Failed to load OpenGL functions via glad");
    }

    LOG_INFO("Created window \"%s\" (%ux%u) with OpenGL %d.%d",
             title.c_str(), width, height,
             GLAD_VERSION_MAJOR(version), GLAD_VERSION_MINOR(version));

    // Enable vsync (0 = off, 1 = on, -1 = adaptive)
    SDL_GL_SetSwapInterval(1);
}

Window::~Window() {
    if (m_glContext) {
        SDL_GL_DestroyContext(m_glContext);
        m_glContext = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    SDL_Quit();
}

void Window::swapWindow() {
    SDL_GL_SwapWindow(m_window);
}

} // namespace bro::platform
