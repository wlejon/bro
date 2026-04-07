#pragma once

#include <string>
#include <cstdint>

struct SDL_Window;
typedef struct SDL_GLContextState* SDL_GLContext;

namespace bro::platform {

class Window {
public:
    Window(const std::string& title, uint32_t width, uint32_t height, bool hidden = false);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    SDL_Window* getSDLWindow() const { return m_window; }
    SDL_GLContext getGLContext() const { return m_glContext; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

    void setSize(uint32_t width, uint32_t height) { m_width = width; m_height = height; }
    void setTitle(const std::string& title);
    void swapWindow();

    /// Create a second GL context that shares textures with the main context.
    /// The caller owns the returned context and must destroy it with SDL_GL_DestroyContext.
    SDL_GLContext createSharedContext();

private:
    SDL_Window* m_window = nullptr;
    SDL_GLContext m_glContext = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace bro::platform
