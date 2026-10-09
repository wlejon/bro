#pragma once
// The windowing system's event source. A backend's EventLoop reads the OS
// queue and calls the handlers below; the engine installs them. One
// EventLoop serves every window: window-scoped events carry the window's id
// (Window::windowId) first. Keys are in the model of platform/keys.h.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bro::platform {

class EventLoop {
public:
    virtual ~EventLoop() = default;

    // The process was asked to quit: the last window closed, or the OS asked
    // (logout, Ctrl+C on a console app). The backend also calls
    // util::requestInterrupt() and sets shouldQuit().
    std::function<void()> onQuit;
    // The window manager asked for `windowId` to be closed (title-bar close,
    // Alt+F4). Nothing is destroyed; the app decides. When the request is for
    // the LAST remaining window the backend follows it with onQuit — the
    // single-window app-exit path — so a handler must not double up.
    std::function<void(uint32_t windowId)> onCloseRequested;
    std::function<void(uint32_t windowId, uint32_t width, uint32_t height)> onResize;
    // Mouse buttons are numbered 1 left, 2 middle, 3 right, 4 and 5 the side
    // (back / forward) buttons. Positions are window coordinates.
    std::function<void(uint32_t windowId, float x, float y, uint8_t button)> onMouseDown;
    std::function<void(uint32_t windowId, float x, float y, uint8_t button)> onMouseUp;
    std::function<void(uint32_t windowId, float x, float y, float xrel, float yrel)> onMouseMove;
    // keycode is a platform::Keycode, scancode a platform::Scancode, mod the
    // platform::KeyMods held. Key-up events never report `repeat`.
    std::function<void(uint32_t windowId, int32_t keycode, int32_t scancode, uint16_t mod, bool repeat)> onKeyDown;
    std::function<void(uint32_t windowId, int32_t keycode, int32_t scancode, uint16_t mod, bool repeat)> onKeyUp;
    // Committed text (UTF-8) while the window's TextInput is started.
    std::function<void(uint32_t windowId, const std::string& text)> onTextInput;
    // IME composition update: `text` is the current preedit ("" when the
    // composition is cancelled/ended without commit), `start` the
    // composition-cursor position and `length` the selected span within it,
    // both in UTF-8 characters.
    std::function<void(uint32_t windowId, const std::string& text, int32_t start, int32_t length)> onTextEditing;
    // Wheel deltas in detents (+y away from the user); see platform/wheel.h
    // for their meaning in pixels. x/y is the pointer position.
    std::function<void(uint32_t windowId, float x, float y, float dx, float dy)> onWheel;
    // One drop gesture, however many files it carried: the DOM sees a single
    // `drop` event with dataTransfer.files populated, which is what the web
    // platform specifies and what a batch drop target needs to group its work.
    std::function<void(uint32_t windowId, const std::vector<std::string>& paths, float x, float y)> onDropFile;
    std::function<void(uint32_t windowId, const std::string& text, float x, float y)> onDropText;
    // A drag this process handed to the window system (Window::startDrag).
    // Pointer events stop while the window system carries it, so it is
    // reported here instead: moving over one of our windows (window
    // coordinates), leaving it, dropped on it (the page takes the drop
    // itself: onDropFile/onDropText do not fire for it), and over —
    // `action` "copy" / "move" when another client took it, "none" when it
    // was cancelled or refused. onOwnDragEnd fires once per drag, after a
    // drop on our own window as well.
    std::function<void(uint32_t windowId, float x, float y)> onOwnDragMotion;
    std::function<void(uint32_t windowId)> onOwnDragLeave;
    std::function<void(uint32_t windowId, float x, float y)> onOwnDragDrop;
    std::function<void(const std::string& action)> onOwnDragEnd;
    std::function<void(uint32_t windowId)> onFocusLost;
    std::function<void(uint32_t windowId)> onFocusGained;
    // Window state transitions. Restored fires on un-minimize AND
    // un-maximize; query the resulting state via Window::isMinimized() /
    // isMaximized().
    std::function<void(uint32_t windowId)> onMinimized;
    std::function<void(uint32_t windowId)> onMaximized;
    std::function<void(uint32_t windowId)> onRestored;
    // Occlusion: the OS reports the window fully covered / visible again.
    // Used to skip presenting into windows nobody can see.
    std::function<void(uint32_t windowId)> onOccluded;
    std::function<void(uint32_t windowId)> onExposed;
    // OS light/dark theme flipped; query it via SystemInfo::theme().
    std::function<void()> onSystemThemeChanged;
    // The window's display scale or pixel density changed: the user changed
    // the OS scaling factor, or the window moved to a display with a
    // different scale. Re-query via Window::getDisplayScale() /
    // getPixelDensity().
    std::function<void(uint32_t windowId)> onDisplayScaleChanged;
    // Gamepads (device-scoped, no window). `instanceId` names the device for
    // its connected lifetime (see Gamepads); `button` is a
    // platform::GamepadButton, `axis` a platform::GamepadAxis. Axis values are
    // normalized: sticks -1..1, triggers 0..1.
    std::function<void(uint32_t instanceId)> onGamepadAdded;
    std::function<void(uint32_t instanceId)> onGamepadRemoved;
    std::function<void(uint32_t instanceId, int button, bool down)> onGamepadButton;
    std::function<void(uint32_t instanceId, int axis, float value)> onGamepadAxis;
    // Touch contacts. `fingerId` is stable and unique for the lifetime of one
    // contact; x/y are window coordinates (the space mouse events use),
    // pressure is 0..1.
    std::function<void(uint32_t windowId, uint64_t fingerId, float x, float y, float pressure)> onFingerDown;
    std::function<void(uint32_t windowId, uint64_t fingerId, float x, float y, float pressure)> onFingerMove;
    std::function<void(uint32_t windowId, uint64_t fingerId, float x, float y)> onFingerUp;
    std::function<void(uint32_t windowId, uint64_t fingerId, float x, float y)> onFingerCancel;

    /// Dispatch every pending event to the handlers. Call once per frame.
    virtual void pollEvents() = 0;

    /// Called for every window event as the OS delivers it, from inside the
    /// OS's own dispatch — including while a modal loop the OS runs (a Win32
    /// live resize or move) keeps pollEvents from returning. Lets the engine
    /// keep timers running through the modal loop. Empty to remove.
    virtual void setModalWindowEventHook(std::function<void()> hook) = 0;

    /// Runs a blocking loop that polls events each frame until quit.
    /// The perFrame callback is invoked once per iteration.
    void run(std::function<void(float deltaTime)> perFrame);

    bool shouldQuit() const { return m_quit; }
    void requestQuit() { m_quit = true; }

    /// Delta time of the last frame in seconds.
    float getDeltaTime() const { return m_deltaTime; }

protected:
    EventLoop() = default;
    bool m_quit = false;

private:
    float m_deltaTime = 0.0f;
    uint64_t m_lastFrameTime = 0;

    void updateTiming();
};

} // namespace bro::platform
