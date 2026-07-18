#pragma once

#include <string>
#include <cstdint>
#include <vector>

struct SDL_Window;
struct SDL_Cursor;
typedef struct SDL_GLContextState* SDL_GLContext;

namespace bro::platform {

/// Display mode description (resolution + refresh rate).
struct DisplayModeInfo {
    int width = 0;
    int height = 0;
    float refreshRate = 0.0f;
};

/// One attached display. `x/y/width/height` are the full bounds in desktop
/// coordinates; the `work*` fields exclude the taskbar/dock (SDL usable
/// bounds). `refreshRate` is the desktop mode's rate, `contentScale` the OS
/// scaling factor (2.0 on a 200% HiDPI desktop).
struct DisplayInfo {
    uint32_t id = 0;   // SDL display id — stable while the display is attached
    std::string name;
    int x = 0, y = 0, width = 0, height = 0;
    int workX = 0, workY = 0, workWidth = 0, workHeight = 0;
    float refreshRate = 0.0f;
    float contentScale = 1.0f;
    bool isPrimary = false;
    bool isCurrent = false;  // the display this window currently sits on
};

/// System cursor shapes the engine can request (the CSS `cursor` keywords
/// collapse onto these — see cursorShapeFromCss in input_handling.cpp).
/// None hides the OS cursor (CSS `cursor: none`).
enum class CursorShape {
    Default,
    Pointer,
    Text,
    Move,
    Crosshair,
    Wait,
    Progress,
    NotAllowed,
    ResizeEW,
    ResizeNS,
    ResizeNESW,
    ResizeNWSE,
    None,
    Count_  // sentinel — cache array size, not a real shape
};

class Window {
public:
    Window(const std::string& title, uint32_t width, uint32_t height,
           bool hidden = false, bool resizable = true, bool vsync = true,
           bool borderless = false);
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

    /// Create a second GL context that shares resources with the main
    /// context. Must be called on the main thread (macOS SDL_GL_CreateContext
    /// calls AppKit and will deadlock if invoked from a worker while the
    /// main thread is blocked). The returned context is current on the
    /// calling thread when this returns; the main context is restored
    /// before the function exits. The caller owns the returned context
    /// and must destroy it with SDL_GL_DestroyContext.
    SDL_GLContext createSharedContext();

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

    // --- Window management ---
    // All of these are safe on the hidden headless window: SDL applies the
    // style/limit flags immediately (so the matching getters round-trip in
    // tests) and the OS simply never shows the result.

    /// Remove (true) or restore (false) the window border + title bar.
    void setBorderless(bool borderless);
    bool isBorderless() const;

    /// Keep the window above all normal windows.
    void setAlwaysOnTop(bool onTop);
    bool isAlwaysOnTop() const;

    /// Minimum client-area size the user can resize down to. (0,0) clears.
    void setMinimumSize(int w, int h);
    void getMinimumSize(int& w, int& h) const;

    /// Maximum client-area size the user can resize up to. (0,0) clears.
    void setMaximumSize(int w, int h);
    void getMaximumSize(int& w, int& h) const;

    /// Window position in desktop coordinates (top-left of the client area).
    void setPosition(int x, int y);
    void getPosition(int& x, int& y) const;

    /// Programmatic minimize / maximize / restore. State changes arrive back
    /// through SDL window events (EventLoop::onMinimized/onMaximized/
    /// onRestored); query the current state with the predicates below.
    void minimize();
    void maximize();
    void restore();
    bool isMinimized() const;
    bool isMaximized() const;
    bool isFullscreen() const;

    /// Enumerate all attached displays. `isCurrent` marks the display this
    /// window sits on. Never empty on a machine with a working video driver.
    std::vector<DisplayInfo> getDisplays() const;

    /// Center the window on the given display (id from getDisplays), inside
    /// its usable (work-area) bounds. Returns false for an unknown id.
    bool moveToDisplay(uint32_t displayId);

    /// Current display scale for this window (2.0 on a 200% HiDPI desktop).
    /// SDL_GetWindowDisplayScale first (per-window, tracks the display the
    /// window actually sits on), falling back to the display's content scale;
    /// 1.0 when neither is available. Re-query after
    /// SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED (EventLoop::onDisplayScaleChanged).
    float getDisplayScale() const;

    /// Set the window icon from a PNG file (taskbar / Alt-Tab / title bar).
    /// Silently no-ops if the file is missing or malformed — a missing icon
    /// should never stop the app from starting.
    void setIcon(const std::string& pngPath);

    /// Apply a system cursor shape to the OS cursor. SDL cursor objects are
    /// created lazily, cached for the window's lifetime, and destroyed at
    /// shutdown; a repeated call with the current shape is a no-op (safe to
    /// drive from per-mouse-move code). CursorShape::None hides the OS
    /// cursor; switching to any other shape shows it again.
    void setCursor(CursorShape shape);

private:
    SDL_Window* m_window = nullptr;
    SDL_GLContext m_glContext = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    SDL_Cursor* m_cursors[static_cast<int>(CursorShape::Count_)] = {};
    CursorShape m_cursorShape = CursorShape::Default;
};

} // namespace bro::platform
