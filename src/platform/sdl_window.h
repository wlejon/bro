#pragma once

#include <string>
#include <cstdint>
#include <vector>

struct SDL_Window;
typedef struct SDL_GLContextState* SDL_GLContext;

namespace bro::platform {

/// Display mode description (resolution + refresh rate).
struct DisplayModeInfo {
    int width = 0;
    int height = 0;
    float refreshRate = 0.0f;
};

class Window {
public:
    Window(const std::string& title, uint32_t width, uint32_t height,
           bool hidden = false, bool resizable = true, bool vsync = true);
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

    /// Release the main GL context from the calling (main) thread so another
    /// thread can temporarily make it current to create a context that shares
    /// resources with it. Call reclaimGLContext() once the other thread has
    /// finished creating + switching to its own context.
    void releaseGLContext();

    /// Re-bind the main GL context on the calling (main) thread. Counterpart
    /// to releaseGLContext().
    void reclaimGLContext();

    // --- Runtime settings ---

    /// Toggle fullscreen mode.
    void setFullscreen(bool fullscreen);

    /// Change vsync mode. Requires GL context to be current.
    void setVSync(bool enabled);

    /// Change whether the window is resizable.
    void setResizable(bool resizable);

    /// Resize the window (windowed mode only).
    void setWindowSize(uint32_t width, uint32_t height);

    /// Enumerate available fullscreen display modes.
    std::vector<DisplayModeInfo> getDisplayModes() const;

private:
    SDL_Window* m_window = nullptr;
    SDL_GLContext m_glContext = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace bro::platform
