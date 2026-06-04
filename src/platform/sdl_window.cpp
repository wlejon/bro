#include "platform/sdl_window.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include "broimage/decode.h"
#include <algorithm>
#include <stdexcept>

namespace bro::platform {

Window::Window(const std::string& title, uint32_t width, uint32_t height,
               bool hidden, bool resizable, bool vsync)
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

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL;
    if (hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    } else if (resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    m_window = SDL_CreateWindow(title.c_str(),
                                static_cast<int>(width),
                                static_cast<int>(height),
                                flags);

    if (!m_window) {
        LOG_ERROR("Failed to create SDL window: %s", SDL_GetError());
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }

    // Clamp a visible window to the display's usable area so the whole window —
    // title bar and borders included — fits on screen. The requested size is a
    // client-area size in physical pixels (Windows/X11 deal in device pixels),
    // so a default like 1920x1080 plus chrome overshoots smaller or DPI-scaled
    // desktops and lands partly off-screen. SDL_GetDisplayUsableBounds already
    // excludes the taskbar; subtracting the border sizes accounts for chrome.
    if (!hidden) {
        SDL_DisplayID disp = SDL_GetDisplayForWindow(m_window);
        SDL_Rect usable{};
        if (disp && SDL_GetDisplayUsableBounds(disp, &usable)) {
            int top = 0, left = 0, bottom = 0, right = 0;
            SDL_GetWindowBordersSize(m_window, &top, &left, &bottom, &right);
            int maxW = usable.w - left - right;
            int maxH = usable.h - top - bottom;
            int clampedW = std::min<int>(static_cast<int>(m_width), std::max(1, maxW));
            int clampedH = std::min<int>(static_cast<int>(m_height), std::max(1, maxH));
            if (clampedW != static_cast<int>(m_width) ||
                clampedH != static_cast<int>(m_height)) {
                LOG_INFO("Clamped window from %ux%u to %dx%d to fit display %dx%d",
                         m_width, m_height, clampedW, clampedH, usable.w, usable.h);
                SDL_SetWindowSize(m_window, clampedW, clampedH);
                m_width = static_cast<uint32_t>(clampedW);
                m_height = static_cast<uint32_t>(clampedH);
            }
            SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED);
        }
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

    // VSync: adaptive (-1) preferred, standard (1) fallback, or disabled (0).
    if (!hidden) {
        if (vsync) {
            if (!SDL_GL_SetSwapInterval(-1)) {
                SDL_GL_SetSwapInterval(1);
            }
        } else {
            SDL_GL_SetSwapInterval(0);
        }
    }
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

void Window::setTitle(const std::string& title) {
    if (m_window) {
        SDL_SetWindowTitle(m_window, title.c_str());
    }
}

void Window::setFullscreen(bool fullscreen) {
    if (!m_window) return;
    if (!SDL_SetWindowFullscreen(m_window, fullscreen)) {
        LOG_ERROR("Failed to set fullscreen: %s", SDL_GetError());
    }
}

void Window::setVSync(bool enabled) {
    if (enabled) {
        if (!SDL_GL_SetSwapInterval(-1)) {
            SDL_GL_SetSwapInterval(1);
        }
    } else {
        SDL_GL_SetSwapInterval(0);
    }
}

void Window::setResizable(bool resizable) {
    if (!m_window) return;
    if (!SDL_SetWindowResizable(m_window, resizable)) {
        LOG_ERROR("Failed to set resizable: %s", SDL_GetError());
    }
}

void Window::setWindowSize(uint32_t width, uint32_t height) {
    if (!m_window) return;
    if (!SDL_SetWindowSize(m_window, static_cast<int>(width), static_cast<int>(height))) {
        LOG_ERROR("Failed to set window size: %s", SDL_GetError());
    }
    m_width = width;
    m_height = height;
}

std::vector<DisplayModeInfo> Window::getDisplayModes() const {
    std::vector<DisplayModeInfo> result;
    if (!m_window) return result;

    SDL_DisplayID displayID = SDL_GetDisplayForWindow(m_window);
    if (!displayID) return result;

    int count = 0;
    const SDL_DisplayMode* const* modes = SDL_GetFullscreenDisplayModes(displayID, &count);
    if (!modes) return result;

    for (int i = 0; i < count; i++) {
        if (modes[i]) {
            result.push_back({modes[i]->w, modes[i]->h, modes[i]->refresh_rate});
        }
    }
    return result;
}

void Window::setIcon(const std::string& pngPath) {
    if (!m_window) return;
    broimage::Image img;
    std::string err;
    if (!broimage::decode_file(pngPath, img, &err)) {
        LOG_INFO("Window icon: could not load '%s' (%s)", pngPath.c_str(), err.c_str());
        return;
    }
    SDL_Surface* surf = SDL_CreateSurfaceFrom(img.width, img.height,
        SDL_PIXELFORMAT_RGBA32, img.pixels.data(), img.width * 4);
    if (!surf) {
        LOG_ERROR("Window icon: SDL_CreateSurfaceFrom failed: %s", SDL_GetError());
        return;
    }
    if (!SDL_SetWindowIcon(m_window, surf)) {
        LOG_ERROR("Window icon: SDL_SetWindowIcon failed: %s", SDL_GetError());
    }
    SDL_DestroySurface(surf);
}

SDL_GLContext Window::createSharedContext() {
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    SDL_GLContext shared = SDL_GL_CreateContext(m_window);
    if (!shared) {
        LOG_ERROR("Failed to create shared GL context: %s", SDL_GetError());
        return nullptr;
    }
    // SDL_GL_CreateContext makes the new context current on this thread;
    // restore the main context so callers see unchanged current-context state.
    SDL_GL_MakeCurrent(m_window, m_glContext);
    return shared;
}

} // namespace bro::platform
