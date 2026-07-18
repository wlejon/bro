#include "platform/sdl_window.h"
#include "platform/sdl_runtime.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include "broimage/decode.h"
#include <algorithm>
#include <stdexcept>

namespace bro::platform {

// Process-wide GL attribute set. Both the primary constructor and the
// secondary-window factory request the same attributes before creating their
// SDL_WINDOW_OPENGL surface, so every window's pixel format is compatible
// with the one shared main context (makeGLCurrent on any drawable).
void Window::setGLAttributes() {
    // Request OpenGL 3.3 Core context
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
}

Window::Window(const std::string& title, uint32_t width, uint32_t height,
               bool hidden, bool resizable, bool vsync, bool borderless)
    : m_width(width), m_height(height), m_vsyncPref(vsync)
{
    // SDL library lifetime is refcounted across all windows (SdlRuntime);
    // this primary window holds one reference like any other.
    if (!SdlRuntime::acquire()) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    setGLAttributes();

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL;
    if (hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    } else if (resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (borderless) {
        flags |= SDL_WINDOW_BORDERLESS;
    }
    m_window = SDL_CreateWindow(title.c_str(),
                                static_cast<int>(width),
                                static_cast<int>(height),
                                flags);

    if (!m_window) {
        LOG_ERROR("Failed to create SDL window: %s", SDL_GetError());
        SdlRuntime::release();
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

    // Create OpenGL context. Primary-only: secondary windows (createSecondary)
    // never create a context — the engine points this context at their
    // drawables instead.
    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext) {
        LOG_ERROR("Failed to create GL context: %s", SDL_GetError());
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        SdlRuntime::release();
        throw std::runtime_error(std::string("SDL_GL_CreateContext failed: ") + SDL_GetError());
    }

    // Load OpenGL functions via glad. On the failure paths below, tear down
    // what this constructor built and drop the SDL refcount — the thrown
    // exception means ~Window will never run (headless catches this and falls
    // back to CPU raster; the balanced release keeps SdlRuntime consistent).
    auto failCleanup = [this]() {
        SDL_GL_DestroyContext(m_glContext);
        m_glContext = nullptr;
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        SdlRuntime::release();
    };
    int version = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);
    if (!version) {
        failCleanup();
        throw std::runtime_error("Failed to load OpenGL functions via glad");
    }

    // A driver with no real GL support (headless server, a CI runner with only
    // software GDI GL 1.1) can hand back a context below what we requested:
    // SDL_GL_CreateContext succeeds and glad loads the 1.x entry points fine, so
    // neither check above fires — then the first 3.3-core call (VAOs, etc.) hits
    // a null function pointer and segfaults. Reject it here with a clear message
    // instead. Callers that can fall back to CPU raster (headless) catch this.
    int glMajor = GLAD_VERSION_MAJOR(version);
    int glMinor = GLAD_VERSION_MINOR(version);
    if (glMajor < 3 || (glMajor == 3 && glMinor < 3)) {
        failCleanup();
        throw std::runtime_error(
            "OpenGL 3.3 core required, but this system provides only OpenGL " +
            std::to_string(glMajor) + "." + std::to_string(glMinor) +
            " (update the GPU driver, or run bro-headless with --no-gpu for CPU rendering)");
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
    for (SDL_Cursor*& c : m_cursors) {
        if (c) {
            SDL_DestroyCursor(c);
            c = nullptr;
        }
    }
    if (m_glContext) {
        SDL_GL_DestroyContext(m_glContext);
        m_glContext = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    SdlRuntime::release();
}

std::unique_ptr<Window> Window::createSecondary(const SecondaryConfig& cfg) {
    if (!SdlRuntime::acquire()) return nullptr;

    // Same attribute set as the primary so this SDL_WINDOW_OPENGL surface's
    // pixel format is compatible with the shared main context.
    setGLAttributes();

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL;
    if (cfg.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    } else if (cfg.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (cfg.borderless) flags |= SDL_WINDOW_BORDERLESS;
    if (cfg.alwaysOnTop) flags |= SDL_WINDOW_ALWAYS_ON_TOP;

    SDL_Window* sdlWin = SDL_CreateWindow(cfg.title.c_str(),
                                          static_cast<int>(cfg.width),
                                          static_cast<int>(cfg.height),
                                          flags);
    if (!sdlWin) {
        LOG_ERROR("Failed to create secondary window: %s", SDL_GetError());
        SdlRuntime::release();
        return nullptr;
    }

    // unique_ptr can't reach the private default ctor through make_unique.
    std::unique_ptr<Window> win(new Window());
    win->m_window = sdlWin;
    win->m_width = cfg.width;
    win->m_height = cfg.height;
    win->m_vsyncPref = false;  // secondary swaps run at interval 0 by policy

    // Placement: explicit position wins; else center on the requested
    // display; else leave it to the OS. Skipped for hidden windows — where a
    // hidden window "is" depends on the desktop the process runs on, and
    // headless tests must stay desk-independent (same policy as the primary).
    if (!cfg.hidden) {
        if (cfg.x != SecondaryConfig::kPosUnset &&
            cfg.y != SecondaryConfig::kPosUnset) {
            SDL_SetWindowPosition(sdlWin, cfg.x, cfg.y);
        } else if (cfg.displayId != 0) {
            win->moveToDisplay(cfg.displayId);
        }
    }

    return win;
}

uint32_t Window::windowId() const {
    if (!m_window) return 0;
    return static_cast<uint32_t>(SDL_GetWindowID(m_window));
}

bool Window::makeGLCurrent(SDL_GLContext ctx) {
    if (!m_window) return false;
    if (!SDL_GL_MakeCurrent(m_window, ctx)) {
        LOG_ERROR("SDL_GL_MakeCurrent failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

void Window::applySwapIntervalPreference() {
    if (m_vsyncPref) {
        if (!SDL_GL_SetSwapInterval(-1)) {
            SDL_GL_SetSwapInterval(1);
        }
    } else {
        SDL_GL_SetSwapInterval(0);
    }
}

void Window::getSize(int& w, int& h) const {
    w = h = 0;
    if (!m_window) return;
    SDL_GetWindowSize(m_window, &w, &h);
}

void Window::getSizeInPixels(int& w, int& h) const {
    w = h = 0;
    if (!m_window) return;
    SDL_GetWindowSizeInPixels(m_window, &w, &h);
}

void Window::raise() {
    if (!m_window) return;
    if (!SDL_RaiseWindow(m_window)) {
        LOG_INFO("SDL_RaiseWindow failed: %s", SDL_GetError());
    }
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
    m_vsyncPref = enabled;
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

void Window::setBorderless(bool borderless) {
    if (!m_window) return;
    if (!SDL_SetWindowBordered(m_window, !borderless)) {
        LOG_ERROR("Failed to set borderless: %s", SDL_GetError());
    }
}

bool Window::isBorderless() const {
    if (!m_window) return false;
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_BORDERLESS) != 0;
}

void Window::setAlwaysOnTop(bool onTop) {
    if (!m_window) return;
    if (!SDL_SetWindowAlwaysOnTop(m_window, onTop)) {
        LOG_ERROR("Failed to set always-on-top: %s", SDL_GetError());
    }
}

bool Window::isAlwaysOnTop() const {
    if (!m_window) return false;
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_ALWAYS_ON_TOP) != 0;
}

void Window::setMinimumSize(int w, int h) {
    if (!m_window) return;
    if (!SDL_SetWindowMinimumSize(m_window, std::max(0, w), std::max(0, h))) {
        LOG_ERROR("Failed to set minimum size: %s", SDL_GetError());
    }
}

void Window::getMinimumSize(int& w, int& h) const {
    w = h = 0;
    if (!m_window) return;
    SDL_GetWindowMinimumSize(m_window, &w, &h);
}

void Window::setMaximumSize(int w, int h) {
    if (!m_window) return;
    if (!SDL_SetWindowMaximumSize(m_window, std::max(0, w), std::max(0, h))) {
        LOG_ERROR("Failed to set maximum size: %s", SDL_GetError());
    }
}

void Window::getMaximumSize(int& w, int& h) const {
    w = h = 0;
    if (!m_window) return;
    SDL_GetWindowMaximumSize(m_window, &w, &h);
}

void Window::setPosition(int x, int y) {
    if (!m_window) return;
    if (!SDL_SetWindowPosition(m_window, x, y)) {
        LOG_ERROR("Failed to set window position: %s", SDL_GetError());
    }
}

void Window::getPosition(int& x, int& y) const {
    x = y = 0;
    if (!m_window) return;
    SDL_GetWindowPosition(m_window, &x, &y);
}

void Window::minimize() {
    if (!m_window) return;
    if (!SDL_MinimizeWindow(m_window)) {
        LOG_ERROR("Failed to minimize window: %s", SDL_GetError());
    }
}

void Window::maximize() {
    if (!m_window) return;
    if (!SDL_MaximizeWindow(m_window)) {
        LOG_ERROR("Failed to maximize window: %s", SDL_GetError());
    }
}

void Window::restore() {
    if (!m_window) return;
    if (!SDL_RestoreWindow(m_window)) {
        LOG_ERROR("Failed to restore window: %s", SDL_GetError());
    }
}

bool Window::isMinimized() const {
    if (!m_window) return false;
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0;
}

bool Window::isMaximized() const {
    if (!m_window) return false;
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MAXIMIZED) != 0;
}

bool Window::isFullscreen() const {
    if (!m_window) return false;
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) != 0;
}

std::vector<DisplayInfo> Window::getDisplays() const {
    std::vector<DisplayInfo> result;

    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays) return result;

    SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    SDL_DisplayID current = m_window ? SDL_GetDisplayForWindow(m_window) : 0;

    for (int i = 0; i < count; i++) {
        SDL_DisplayID id = displays[i];
        DisplayInfo info;
        info.id = id;
        if (const char* name = SDL_GetDisplayName(id)) info.name = name;

        SDL_Rect bounds{};
        if (SDL_GetDisplayBounds(id, &bounds)) {
            info.x = bounds.x; info.y = bounds.y;
            info.width = bounds.w; info.height = bounds.h;
        }
        // Usable bounds exclude the taskbar/dock; fall back to full bounds
        // if the query fails (some minimal Wayland compositors).
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(id, &usable)) {
            info.workX = usable.x; info.workY = usable.y;
            info.workWidth = usable.w; info.workHeight = usable.h;
        } else {
            info.workX = info.x; info.workY = info.y;
            info.workWidth = info.width; info.workHeight = info.height;
        }

        if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(id)) {
            info.refreshRate = mode->refresh_rate;
        }
        float scale = SDL_GetDisplayContentScale(id);
        info.contentScale = scale > 0.0f ? scale : 1.0f;

        info.isPrimary = (id == primary);
        info.isCurrent = (id == current);
        result.push_back(std::move(info));
    }
    SDL_free(displays);
    return result;
}

bool Window::moveToDisplay(uint32_t displayId) {
    if (!m_window) return false;

    // Validate the id against the attached displays — SDL_GetDisplayUsableBounds
    // on a stale id would just error, but a clear false return lets JS callers
    // detect a display that was unplugged since getDisplays().
    SDL_Rect usable{};
    if (!SDL_GetDisplayUsableBounds(static_cast<SDL_DisplayID>(displayId), &usable)) {
        LOG_INFO("moveToDisplay(%u): %s", displayId, SDL_GetError());
        return false;
    }

    int w = 0, h = 0;
    SDL_GetWindowSize(m_window, &w, &h);
    int x = usable.x + std::max(0, (usable.w - w) / 2);
    int y = usable.y + std::max(0, (usable.h - h) / 2);
    if (!SDL_SetWindowPosition(m_window, x, y)) {
        LOG_ERROR("Failed to move window to display %u: %s", displayId, SDL_GetError());
        return false;
    }
    return true;
}

void Window::setCursor(CursorShape shape) {
    if (!m_window || shape == m_cursorShape) return;

    if (shape == CursorShape::None) {
        SDL_HideCursor();
        m_cursorShape = shape;
        return;
    }
    if (m_cursorShape == CursorShape::None) {
        SDL_ShowCursor();
    }

    SDL_SystemCursor id = SDL_SYSTEM_CURSOR_DEFAULT;
    switch (shape) {
        case CursorShape::Default:    id = SDL_SYSTEM_CURSOR_DEFAULT;     break;
        case CursorShape::Pointer:    id = SDL_SYSTEM_CURSOR_POINTER;     break;
        case CursorShape::Text:       id = SDL_SYSTEM_CURSOR_TEXT;        break;
        case CursorShape::Move:       id = SDL_SYSTEM_CURSOR_MOVE;        break;
        case CursorShape::Crosshair:  id = SDL_SYSTEM_CURSOR_CROSSHAIR;   break;
        case CursorShape::Wait:       id = SDL_SYSTEM_CURSOR_WAIT;        break;
        case CursorShape::Progress:   id = SDL_SYSTEM_CURSOR_PROGRESS;    break;
        case CursorShape::NotAllowed: id = SDL_SYSTEM_CURSOR_NOT_ALLOWED; break;
        case CursorShape::ResizeEW:   id = SDL_SYSTEM_CURSOR_EW_RESIZE;   break;
        case CursorShape::ResizeNS:   id = SDL_SYSTEM_CURSOR_NS_RESIZE;   break;
        case CursorShape::ResizeNESW: id = SDL_SYSTEM_CURSOR_NESW_RESIZE; break;
        case CursorShape::ResizeNWSE: id = SDL_SYSTEM_CURSOR_NWSE_RESIZE; break;
        case CursorShape::None:
        case CursorShape::Count_:     break;  // handled above / unreachable
    }

    SDL_Cursor*& cached = m_cursors[static_cast<int>(shape)];
    if (!cached) {
        cached = SDL_CreateSystemCursor(id);
        if (!cached) {
            // Driver refused the shape (minimal wayland compositor, etc.) —
            // record the state so we don't hammer SDL, keep whatever cursor
            // the OS currently shows.
            LOG_INFO("SDL_CreateSystemCursor(%d) failed: %s",
                     static_cast<int>(id), SDL_GetError());
            m_cursorShape = shape;
            return;
        }
    }
    SDL_SetCursor(cached);
    m_cursorShape = shape;
}

float Window::getDisplayScale() const {
    if (!m_window) return 1.0f;
    float scale = SDL_GetWindowDisplayScale(m_window);
    if (scale <= 0.0f) {
        SDL_DisplayID disp = SDL_GetDisplayForWindow(m_window);
        if (disp) scale = SDL_GetDisplayContentScale(disp);
    }
    return scale > 0.0f ? scale : 1.0f;
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
