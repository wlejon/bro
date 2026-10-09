#include "platform/sdl/sdl_window.h"
#include "platform/sdl/sdl_runtime.h"
#include "platform/desktop_platform.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "broimage/decode.h"
#include <algorithm>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#endif

namespace bro::platform {

namespace {

SDL_WindowFlags baseWindowFlags(GraphicsBackend backend) {
    SDL_WindowFlags flags = backend == GraphicsBackend::Vulkan ? SDL_WINDOW_VULKAN : 0;
#ifdef __APPLE__
    flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    return flags;
}

// The app's identity for the desktop, before its first window maps: the
// Wayland app_id / X11 WM_CLASS through SDL's hint, the taskbar grouping id
// on Windows. Set once; a process is one app.
void applyAppId(const std::string& appId) {
    static bool applied = false;
    if (appId.empty() || applied) return;
    applied = true;
    SDL_SetHint(SDL_HINT_APP_ID, appId.c_str());
#if defined(_WIN32)
    const int n = MultiByteToWideChar(CP_UTF8, 0, appId.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, appId.c_str(), -1, wide.data(), n);
    SetCurrentProcessExplicitAppUserModelID(wide.c_str());
#endif
}

} // namespace

SdlWindow::SdlWindow(const WindowConfig& cfg)
    : m_title(cfg.title), m_width(cfg.width), m_height(cfg.height), m_vsyncPref(cfg.vsync),
      m_alwaysOnTop(cfg.alwaysOnTop), m_borderless(cfg.borderless), m_backend(cfg.backend)
{
    applyAppId(cfg.appId);

    // SDL library lifetime is refcounted across all windows (SdlRuntime).
    if (!SdlRuntime::acquire()) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    SDL_WindowFlags flags = baseWindowFlags(m_backend);
    if (cfg.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    } else if (cfg.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (cfg.borderless) {
        SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "0");
        SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "0");
        flags |= SDL_WINDOW_BORDERLESS;
    }
    if (cfg.alwaysOnTop) flags |= SDL_WINDOW_ALWAYS_ON_TOP;

    m_window = SDL_CreateWindow(cfg.title.c_str(), static_cast<int>(cfg.width),
                                static_cast<int>(cfg.height), flags);
    if (!m_window) {
        const std::string err = SDL_GetError();
        LOG_ERROR("Failed to create SDL window: %s", err.c_str());
        SdlRuntime::release();
        throw std::runtime_error("SDL_CreateWindow failed: " + err);
    }

#if defined(_WIN32)
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(m_window),
                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (hwnd && cfg.borderless) {
        LONG style = GetWindowLongW(hwnd, GWL_STYLE);
        style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        style |= WS_POPUP;
        SetWindowLongW(hwnd, GWL_STYLE, style);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
#endif

    // Placement only for a visible window: where a hidden window "is" depends
    // on the desktop the process runs on, and headless tests must stay
    // desk-independent.
    if (!cfg.hidden) {
        // Clamp to the display's usable area so the whole window — title bar
        // and borders included — fits on screen, then center it.
        if (cfg.fitToWorkArea && !cfg.borderless) {
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
                SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            }
        }
        if (cfg.x != WindowConfig::kPosUnset && cfg.y != WindowConfig::kPosUnset) {
            SDL_SetWindowPosition(m_window, cfg.x, cfg.y);
        } else if (cfg.displayId != 0) {
            moveToDisplay(cfg.displayId);
        }
    }

    LOG_INFO("Created window \"%s\" (%ux%u) with %s",
             cfg.title.c_str(), cfg.width, cfg.height,
             m_backend == GraphicsBackend::Vulkan ? "Vulkan" : "software presentation");
}

SdlWindow::~SdlWindow() {
    for (SDL_Cursor*& c : m_cursors) {
        if (c) {
            SDL_DestroyCursor(c);
            c = nullptr;
        }
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    SdlRuntime::release();
}

SDL_Window* sdlWindowOf(const Window* window) {
    auto* w = dynamic_cast<const SdlWindow*>(window);
    return w ? w->sdlWindow() : nullptr;
}

uint32_t SdlWindow::windowId() const {
    return static_cast<uint32_t>(SDL_GetWindowID(m_window));
}

bool SdlWindow::createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) {
    if (!SDL_Vulkan_CreateSurface(m_window, instance, nullptr, surface)) {
        LOG_ERROR("SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

void SdlWindow::getSize(int& w, int& h) const {
    w = h = 0;
    SDL_GetWindowSize(m_window, &w, &h);
}

void SdlWindow::getSizeInPixels(int& w, int& h) const {
    w = h = 0;
    SDL_GetWindowSizeInPixels(m_window, &w, &h);
}

float SdlWindow::getPixelDensity() const {
    float density = SDL_GetWindowPixelDensity(m_window);
    return density > 0.0f ? density : 1.0f;
}

float SdlWindow::getDisplayScale() const {
    float scale = SDL_GetWindowDisplayScale(m_window);
    if (scale <= 0.0f) {
        SDL_DisplayID disp = SDL_GetDisplayForWindow(m_window);
        if (disp) scale = SDL_GetDisplayContentScale(disp);
    }
    return scale > 0.0f ? scale : 1.0f;
}

void SdlWindow::raise() {
    if (!SDL_RaiseWindow(m_window)) {
        LOG_INFO("SDL_RaiseWindow failed: %s", SDL_GetError());
    }
}

bool SdlWindow::presentPixels(const void* pixels, int width, int height, int stride, bool bgra) {
    if (m_backend != GraphicsBackend::Software || !pixels) return false;
    SDL_Surface* target = SDL_GetWindowSurface(m_window);
    if (!target) {
        LOG_ERROR("SDL_GetWindowSurface failed: %s", SDL_GetError());
        return false;
    }
    SDL_Surface* src = SDL_CreateSurfaceFrom(width, height,
                                             bgra ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888,
                                             const_cast<void*>(pixels), stride);
    if (!src) {
        LOG_ERROR("SDL_CreateSurfaceFrom failed: %s", SDL_GetError());
        return false;
    }
    // The frame is premultiplied UI over nothing: copy, don't blend.
    SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
    SDL_FillSurfaceRect(target, nullptr, SDL_MapSurfaceRGB(target, 0, 0, 0));
    const bool ok = SDL_BlitSurface(src, nullptr, target, nullptr) && SDL_UpdateWindowSurface(m_window);
    if (!ok) LOG_ERROR("Software present failed: %s", SDL_GetError());
    SDL_DestroySurface(src);
    return ok;
}

void SdlWindow::setTitle(const std::string& title) {
    m_title = title;
    SDL_SetWindowTitle(m_window, title.c_str());
}

std::string SdlWindow::getTitle() const {
    if (const char* t = SDL_GetWindowTitle(m_window)) return std::string(t);
    return m_title;
}

// The requested opacity is the window's opacity: a video driver without
// per-window opacity (the dummy driver headless Linux runs on) ignores the
// request, and reading SDL back would report 1.0 for a value never refused.
float SdlWindow::getOpacity() const {
    return m_opacity;
}

void SdlWindow::setOpacity(float opacity) {
    m_opacity = std::max(0.0f, std::min(1.0f, opacity));
    SDL_SetWindowOpacity(m_window, m_opacity);
}

bool SdlWindow::isFocused() const {
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool SdlWindow::flash(bool on) {
    m_flashing = on;
    if (desktop::isHeadless()) return true;
    return SDL_FlashWindow(m_window, on ? SDL_FLASH_UNTIL_FOCUSED : SDL_FLASH_CANCEL);
}

void SdlWindow::setFullscreen(bool fullscreen) {
    m_fullscreen = fullscreen;
    if (!SDL_SetWindowFullscreen(m_window, fullscreen)) {
        LOG_ERROR("Failed to set fullscreen: %s", SDL_GetError());
    }
}

void SdlWindow::setResizable(bool resizable) {
    if (!SDL_SetWindowResizable(m_window, resizable)) {
        LOG_ERROR("Failed to set resizable: %s", SDL_GetError());
    }
}

void SdlWindow::setWindowSize(uint32_t width, uint32_t height) {
    if (!SDL_SetWindowSize(m_window, static_cast<int>(width), static_cast<int>(height))) {
        LOG_ERROR("Failed to set window size: %s", SDL_GetError());
    }
    m_width = width;
    m_height = height;
}

void SdlWindow::setBorderless(bool borderless) {
    m_borderless = borderless;
    if (!SDL_SetWindowBordered(m_window, !borderless)) {
        LOG_ERROR("Failed to set borderless: %s", SDL_GetError());
    }
}

bool SdlWindow::isBorderless() const {
    return m_borderless;
}

void SdlWindow::setAlwaysOnTop(bool onTop) {
    m_alwaysOnTop = onTop;
    if (!SDL_SetWindowAlwaysOnTop(m_window, onTop)) {
        LOG_ERROR("Failed to set always-on-top: %s", SDL_GetError());
    }
}

bool SdlWindow::isAlwaysOnTop() const {
    return m_alwaysOnTop;
}

void SdlWindow::setMinimumSize(int w, int h) {
    if (!SDL_SetWindowMinimumSize(m_window, std::max(0, w), std::max(0, h))) {
        LOG_ERROR("Failed to set minimum size: %s", SDL_GetError());
    }
}

void SdlWindow::getMinimumSize(int& w, int& h) const {
    w = h = 0;
    SDL_GetWindowMinimumSize(m_window, &w, &h);
}

void SdlWindow::setMaximumSize(int w, int h) {
    if (!SDL_SetWindowMaximumSize(m_window, std::max(0, w), std::max(0, h))) {
        LOG_ERROR("Failed to set maximum size: %s", SDL_GetError());
    }
}

void SdlWindow::getMaximumSize(int& w, int& h) const {
    w = h = 0;
    SDL_GetWindowMaximumSize(m_window, &w, &h);
}

void SdlWindow::setPosition(int x, int y) {
    if (!SDL_SetWindowPosition(m_window, x, y)) {
        LOG_ERROR("Failed to set window position: %s", SDL_GetError());
    }
}

void SdlWindow::getPosition(int& x, int& y) const {
    x = y = 0;
    SDL_GetWindowPosition(m_window, &x, &y);
}

void SdlWindow::minimize() {
    if (!SDL_MinimizeWindow(m_window)) {
        LOG_ERROR("Failed to minimize window: %s", SDL_GetError());
    }
}

void SdlWindow::maximize() {
    if (!SDL_MaximizeWindow(m_window)) {
        LOG_ERROR("Failed to maximize window: %s", SDL_GetError());
    }
}

void SdlWindow::restore() {
    if (!SDL_RestoreWindow(m_window)) {
        LOG_ERROR("Failed to restore window: %s", SDL_GetError());
    }
}

bool SdlWindow::isMinimized() const {
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0;
}

bool SdlWindow::isMaximized() const {
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MAXIMIZED) != 0;
}

bool SdlWindow::isHidden() const {
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_HIDDEN) != 0;
}

void SdlWindow::show() { SDL_ShowWindow(m_window); }
void SdlWindow::hide() { SDL_HideWindow(m_window); }
void SdlWindow::sync() { SDL_SyncWindow(m_window); }

bool SdlWindow::isFullscreen() const {
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) != 0 || m_fullscreen;
}

uint32_t SdlWindow::currentDisplay() const {
    return static_cast<uint32_t>(SDL_GetDisplayForWindow(m_window));
}

bool SdlWindow::moveToDisplay(uint32_t displayId) {
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

void SdlWindow::setIcon(const std::string& pngPath) {
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

NativeHandle SdlWindow::nativeHandle() const {
    NativeHandle h;
    const SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
#if defined(_WIN32)
    h.hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (h.hwnd) h.kind = NativeHandle::Kind::Win32;
#elif defined(__APPLE__)
    h.cocoaWindow = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (h.cocoaWindow) h.kind = NativeHandle::Kind::Cocoa;
#else
    h.waylandSurface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    if (h.waylandSurface) {
        h.kind = NativeHandle::Kind::Wayland;
        h.waylandDisplay = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    } else {
        h.x11Display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        h.x11Window = static_cast<uint64_t>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        if (h.x11Display) h.kind = NativeHandle::Kind::X11;
    }
#endif
    return h;
}

// --- TextInput ---

void SdlWindow::start() { SDL_StartTextInput(m_window); }
void SdlWindow::stop() { SDL_StopTextInput(m_window); }

void SdlWindow::setArea(int x, int y, int w, int h, int cursor) {
    SDL_Rect rect{x, y, w, h};
    SDL_SetTextInputArea(m_window, &rect, cursor);
}

// --- Cursor ---

void SdlWindow::setShape(CursorShape shape) {
    if (shape == m_cursorShape) return;

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

void SdlWindow::setRelativeMode(bool enabled) {
    SDL_SetWindowRelativeMouseMode(m_window, enabled);
}

void SdlWindow::warp(float x, float y) {
    SDL_WarpMouseInWindow(m_window, x, y);
}

} // namespace bro::platform
