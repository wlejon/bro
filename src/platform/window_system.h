#pragma once
// A windowing system: the backend that makes windows and supplies the
// services that come with a desktop session. bro selects one per process
// (selectWindowSystem) before it makes its first window; everything else
// reaches the backend through the accessors here and in the service headers.
// See src/platform/README.md.

#include <memory>
#include <string>
#include <vector>

namespace bro::platform {

class Clipboard;
class DialogBackend;
class Displays;
class EventLoop;
class Gamepads;
class Keyboard;
class SystemInfo;
class Window;
struct WindowConfig;

class WindowSystem {
public:
    virtual ~WindowSystem() = default;

    /// "sdl", "drm", ...
    virtual const char* name() const = 0;

    /// What the backend is driving: "windows", "cocoa", "x11", "wayland";
    /// "offscreen" / "dummy" when there is no display behind the windows; ""
    /// before the first window.
    virtual std::string driverName() const = 0;

    /// Make a window. Throws std::runtime_error when the backend cannot.
    virtual std::unique_ptr<Window> createWindow(const WindowConfig& config) = 0;

    /// The event source for this backend's windows; null for a backend that
    /// delivers input another way (DRM reads the seat's devices itself).
    virtual std::unique_ptr<EventLoop> createEventLoop() = 0;

    /// Let the OS process pending window messages without dispatching them
    /// to anyone: keeps windows responsive (and the compositor from calling
    /// them hung) through a stretch with no EventLoop polling.
    virtual void pumpEvents() = 0;

    /// The Vulkan instance extensions this backend's surfaces need. Valid
    /// once the backend has made a window.
    virtual std::vector<std::string> vulkanInstanceExtensions() = 0;

    /// An activation token another process handed over (xdg-activation: the
    /// launch that found this app already running). The next Window::raise
    /// spends it; a backend without activation tokens ignores it.
    virtual void setActivationToken(const std::string& token) { (void)token; }

    virtual Displays& displays() = 0;
    virtual Keyboard& keyboard() = 0;
    virtual Clipboard& clipboard() = 0;
    virtual DialogBackend& dialogs() = 0;
    virtual SystemInfo& systemInfo() = 0;
    virtual Gamepads& gamepads() = 0;
};

enum class WindowSystemKind {
    Sdl,      // SDL3: desktop windows on Windows, macOS, X11 and Wayland (the default)
    Drm,      // bro owns the screen through KMS and the input devices through libinput
    Wayland,  // a Wayland client of its own (browl), Linux only
};

/// Choose the process's windowing system. Call before the first window or
/// service use; the default is Sdl. Selecting Wayland connects to the
/// compositor; when that fails (or the build has no Wayland backend) SDL is
/// selected instead and false is returned.
bool selectWindowSystem(WindowSystemKind kind);

/// Choose the window system for desktop windows: on Linux the Wayland
/// backend when $WAYLAND_DISPLAY is set and the compositor offers what it
/// needs, else SDL. $BRO_WINDOW_SYSTEM=wayland|sdl forces one.
void selectDesktopWindowSystem();

/// The activation token this process was launched with ($XDG_ACTIVATION_TOKEN;
/// read once and removed from the environment, so children do not inherit
/// it); "" when there was none. A single-instance hand-off forwards it to the
/// running instance; otherwise the first window spends it as it maps.
const std::string& launchActivationToken();

/// The active windowing system.
WindowSystem& windowSystem();

/// windowSystem().pumpEvents().
void pumpEvents();

} // namespace bro::platform
