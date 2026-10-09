#pragma once
// One top-level OS window, whatever windowing system draws it.
//
// Coordinates: every position and size here is in window coordinates, the
// units input events arrive in (logical points; device pixels only where the
// OS has no scaling). getSizeInPixels / getPixelDensity give the drawable.
// A window is created by the active WindowSystem (createWindow below); see
// src/platform/README.md for the backend model.

#include <vulkan/vulkan_core.h>

#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::platform {

/// Display mode description (resolution + refresh rate).
struct DisplayModeInfo {
    int width = 0;
    int height = 0;
    float refreshRate = 0.0f;
};

/// One attached display. `x/y/width/height` are the full bounds in desktop
/// coordinates; the `work*` fields exclude the taskbar/dock/panels.
/// `refreshRate` is the desktop mode's rate, `contentScale` the OS scaling
/// factor (2.0 on a 200% HiDPI desktop).
struct DisplayInfo {
    uint32_t id = 0;   // stable while the display is attached; 0 is never a display
    std::string name;
    int x = 0, y = 0, width = 0, height = 0;
    int workX = 0, workY = 0, workWidth = 0, workHeight = 0;
    float refreshRate = 0.0f;
    float contentScale = 1.0f;
    bool isPrimary = false;
    bool isCurrent = false;  // the display the asking window currently sits on
};

/// System cursor shapes the engine can request (the CSS `cursor` keywords
/// collapse onto these — see cursorShapeFromCss in the engine). None hides
/// the cursor (CSS `cursor: none`).
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

/// How a window's frames reach the screen.
enum class GraphicsBackend {
    Vulkan,    // a VulkanSwapchain presents to the window's VkSurfaceKHR
    Software,  // no GPU: CPU frames go through Window::presentPixels
};

/// What createWindow builds.
struct WindowConfig {
    static constexpr int kPosUnset = INT_MIN;
    std::string title = "bro";
    uint32_t width = 800;
    uint32_t height = 600;
    /// Never shown (headless runs one of these as the primary window). Style
    /// and limit setters still apply and read back.
    bool hidden = false;
    bool resizable = true;
    bool borderless = false;
    bool alwaysOnTop = false;
    /// Initial vsync preference (Window::vsyncPreference).
    bool vsync = true;
    /// Fit a visible, bordered window into its display's work area — frame
    /// included — and center it there. The primary window's policy.
    bool fitToWorkArea = false;
    /// Explicit placement for a visible window: a position wins, else
    /// centering on `displayId`, else the OS decides.
    int x = kPosUnset;
    int y = kPosUnset;
    uint32_t displayId = 0;
    GraphicsBackend backend = GraphicsBackend::Vulkan;
    /// The application's identity for the desktop: Wayland app_id, X11
    /// WM_CLASS, Windows AppUserModelID. Empty leaves the backend's default.
    std::string appId;
};

/// The window's native handles, for desktop integration code (taskbar
/// progress, tray, notifications, global hotkeys) that talks to the OS
/// directly. Only the fields of `kind` are set.
struct NativeHandle {
    enum class Kind { None, Win32, X11, Wayland, Cocoa } kind = Kind::None;
    void* hwnd = nullptr;            // Win32 HWND
    void* x11Display = nullptr;      // X11 Display*
    uint64_t x11Window = 0;          // X11 Window
    void* waylandDisplay = nullptr;  // wl_display*
    void* waylandSurface = nullptr;  // wl_surface*
    void* cocoaWindow = nullptr;     // NSWindow*
};

/// Text entry for one window: turns key presses into committed text
/// (EventLoop::onTextInput) and IME composition (onTextEditing). Off until
/// started; the engine starts it while an editable element has focus.
class TextInput {
public:
    virtual ~TextInput() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    /// Where the text being edited is (window coordinates), so an IME places
    /// its candidate window beside it. `cursor` is the caret's x offset from
    /// the rectangle's left edge.
    virtual void setArea(int x, int y, int w, int h, int cursor) = 0;
};

/// The pointer as one window shows and steers it.
class Cursor {
public:
    virtual ~Cursor() = default;
    /// Show a system cursor shape; CursorShape::None hides the cursor. A
    /// repeated call with the current shape costs nothing (safe per mouse move).
    virtual void setShape(CursorShape shape) = 0;
    /// Relative mode (pointer lock): the cursor is hidden and confined, and
    /// EventLoop::onMouseMove keeps reporting relative motion.
    virtual void setRelativeMode(bool enabled) = 0;
    /// Move the cursor to a window-coordinate position.
    virtual void warp(float x, float y) = 0;
};

class Window {
public:
    virtual ~Window() = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// The id input events carry for this window (EventLoop's windowId).
    virtual uint32_t windowId() const = 0;
    virtual GraphicsBackend backend() const = 0;

    // --- Presentation ---

    /// Create a Vulkan surface for this window on `instance` (which must have
    /// WindowSystem::vulkanInstanceExtensions enabled). False on failure. The
    /// caller owns the surface and destroys it with vkDestroySurfaceKHR,
    /// before the window goes.
    virtual bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) = 0;
    /// Software backend: copy a frame of 32-bit pixels to the window and show
    /// it, clipped to the window. False on failure or for a Vulkan window.
    virtual bool presentPixels(const void* pixels, int width, int height, int stride, bool bgra) = 0;
    /// Record the vsync preference; the engine applies it to the swapchain.
    virtual void setVSync(bool enabled) = 0;
    virtual bool vsyncPreference() const = 0;

    // --- Geometry ---

    /// The last size set through this interface (creation, setWindowSize,
    /// setSize) — not a live query; see getSize.
    virtual uint32_t getWidth() const = 0;
    virtual uint32_t getHeight() const = 0;
    virtual void setSize(uint32_t width, uint32_t height) = 0;
    /// Current client-area size in window coordinates, queried live.
    virtual void getSize(int& w, int& h) const = 0;
    /// Current drawable size in physical pixels.
    virtual void getSizeInPixels(int& w, int& h) const = 0;
    /// Drawable pixels per window coordinate: 2.0 for a Retina window on
    /// macOS, 1.0 where window coordinates already are pixels.
    virtual float getPixelDensity() const = 0;
    /// The OS scaling factor for the display the window is on (2.0 on a 200%
    /// desktop); re-query after EventLoop::onDisplayScaleChanged.
    virtual float getDisplayScale() const = 0;
    /// What window.devicePixelRatio reports for content in this window. On
    /// Apple it is the pixel density (CSS px = points, rendered at the backing
    /// scale). Elsewhere it stays getDisplayScale(), although rendering there
    /// is 1 device px per CSS px — a known inconsistency left for a Windows /
    /// X11 follow-up that would render at the display scale too.
    float getDevicePixelRatio() const;
    /// Resize the window (windowed mode only).
    virtual void setWindowSize(uint32_t width, uint32_t height) = 0;
    /// Window position in desktop coordinates (top-left of the client area).
    virtual void setPosition(int x, int y) = 0;
    virtual void getPosition(int& x, int& y) const = 0;
    /// Minimum / maximum client-area size the user can resize to; (0,0) clears.
    virtual void setMinimumSize(int w, int h) = 0;
    virtual void getMinimumSize(int& w, int& h) const = 0;
    virtual void setMaximumSize(int w, int h) = 0;
    virtual void getMaximumSize(int& w, int& h) const = 0;

    // --- Style and state ---
    // All of these are safe on a hidden window: the style/limit flags apply
    // at once (so the getters round-trip in tests) and the OS never shows
    // the result.

    virtual void setTitle(const std::string& title) = 0;
    virtual std::string getTitle() const = 0;
    /// Opacity in [0.0, 1.0]. Reads back what was requested.
    virtual float getOpacity() const = 0;
    virtual void setOpacity(float opacity) = 0;
    virtual void setResizable(bool resizable) = 0;
    /// Remove (true) or restore (false) the border and title bar.
    virtual void setBorderless(bool borderless) = 0;
    virtual bool isBorderless() const = 0;
    /// Keep the window above all normal windows.
    virtual void setAlwaysOnTop(bool onTop) = 0;
    virtual bool isAlwaysOnTop() const = 0;
    virtual void setFullscreen(bool fullscreen) = 0;
    virtual bool isFullscreen() const = 0;
    /// Programmatic minimize / maximize / restore. The resulting transitions
    /// arrive through EventLoop::onMinimized / onMaximized / onRestored.
    virtual void minimize() = 0;
    virtual void maximize() = 0;
    virtual void restore() = 0;
    virtual bool isMinimized() const = 0;
    virtual bool isMaximized() const = 0;
    /// Not shown at all (created hidden, or hidden since).
    virtual bool isHidden() const = 0;
    virtual void show() = 0;
    virtual void hide() = 0;
    /// Block until the windowing system has applied the requests made so far
    /// (size, position, visibility), where it applies them asynchronously.
    virtual void sync() = 0;
    /// Raise the window above its siblings and request input focus.
    virtual void raise() = 0;
    virtual bool isFocused() const = 0;
    /// Flash the window / taskbar button to request attention (on) or stop.
    virtual bool flash(bool on = true) = 0;
    /// Set the window icon from a PNG file (taskbar / Alt-Tab / title bar).
    /// A missing or malformed file is logged and ignored.
    virtual void setIcon(const std::string& pngPath) = 0;
    /// Set the window icon from straight-alpha RGBA8 pixels (stride width*4),
    /// for an icon decoded or rasterized above the platform layer (an app's
    /// SVG icon). Returns false when the window system refused it.
    virtual bool setIconPixels(int width, int height, const uint8_t* rgba) = 0;

    // --- Displays ---

    /// The display the window currently sits on (0 when unknown).
    virtual uint32_t currentDisplay() const = 0;
    /// Center the window in the work area of `displayId`. False for an
    /// unknown (e.g. unplugged) display.
    virtual bool moveToDisplay(uint32_t displayId) = 0;
    /// Displays().list(this): every attached display, this one's marked current.
    std::vector<DisplayInfo> getDisplays() const;
    /// The fullscreen modes of the display the window is on.
    std::vector<DisplayModeInfo> getDisplayModes() const;

    // --- Input surfaces ---

    virtual TextInput& textInput() = 0;
    virtual Cursor& cursor() = 0;

    /// The OS handles behind the window, for desktop integration code.
    virtual NativeHandle nativeHandle() const = 0;

protected:
    Window() = default;
};

/// Create a window with the active WindowSystem. Throws std::runtime_error
/// when the windowing system cannot make one.
std::unique_ptr<Window> createWindow(const WindowConfig& config);

} // namespace bro::platform
