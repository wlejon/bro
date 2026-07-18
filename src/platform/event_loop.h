#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bro::platform {

class EventLoop {
public:
    EventLoop() = default;
    ~EventLoop() = default;

    // Callback types
    std::function<void()> onQuit;
    std::function<void(uint32_t width, uint32_t height)> onResize;
    std::function<void(float x, float y, uint8_t button)> onMouseDown;
    std::function<void(float x, float y, uint8_t button)> onMouseUp;
    std::function<void(float x, float y, float xrel, float yrel)> onMouseMove;
    std::function<void(int32_t keycode, int32_t scancode, uint16_t mod, bool repeat)> onKeyDown;
    std::function<void(int32_t keycode, int32_t scancode, uint16_t mod, bool repeat)> onKeyUp;
    std::function<void(const std::string& text)> onTextInput;
    // IME composition update (SDL_EVENT_TEXT_EDITING): `text` is the current
    // preedit ("" when the composition is cancelled/ended without commit),
    // `start` the composition-cursor position and `length` the selected span
    // within it, both in UTF-8 characters (SDL's units).
    std::function<void(const std::string& text, int32_t start, int32_t length)> onTextEditing;
    std::function<void(float x, float y, float dx, float dy)> onWheel;
    std::function<void(const std::string& path, float x, float y)> onDropFile;
    std::function<void(const std::string& text, float x, float y)> onDropText;
    std::function<void()> onFocusLost;
    std::function<void()> onFocusGained;
    // OS light/dark theme flipped (SDL_EVENT_SYSTEM_THEME_CHANGED). Query the
    // new theme via SDL_GetSystemTheme().
    std::function<void()> onSystemThemeChanged;
    // The window's display scale changed (SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED):
    // the user changed the OS scaling factor, or the window moved to a display
    // with a different scale. Re-query via Window::getDisplayScale().
    std::function<void()> onDisplayScaleChanged;
    // Gamepad events. `instanceId` is the SDL joystick instance id; button is
    // an SDL_GamepadButton, axis an SDL_GamepadAxis. Axis values are already
    // normalized: sticks -1..1, triggers 0..1.
    std::function<void(uint32_t instanceId)> onGamepadAdded;
    std::function<void(uint32_t instanceId)> onGamepadRemoved;
    std::function<void(uint32_t instanceId, int button, bool down)> onGamepadButton;
    std::function<void(uint32_t instanceId, int axis, float value)> onGamepadAxis;
    // Touch (finger) events. `fingerId` is the SDL finger id — stable and
    // unique for the lifetime of one contact. x/y arrive here already
    // converted from SDL's normalized 0-1 range to window coordinates (the
    // same space mouse events use), pressure is 0..1.
    std::function<void(uint64_t fingerId, float x, float y, float pressure)> onFingerDown;
    std::function<void(uint64_t fingerId, float x, float y, float pressure)> onFingerMove;
    std::function<void(uint64_t fingerId, float x, float y)> onFingerUp;
    std::function<void(uint64_t fingerId, float x, float y)> onFingerCancel;

    /// Polls all pending SDL events and dispatches to callbacks.
    /// Call once per frame.
    void pollEvents();

    /// Runs a blocking loop that polls events each frame until quit.
    /// The perFrame callback is invoked once per iteration.
    void run(std::function<void(float deltaTime)> perFrame);

    bool shouldQuit() const { return m_quit; }
    void requestQuit() { m_quit = true; }

    /// Delta time of the last frame in seconds.
    float getDeltaTime() const { return m_deltaTime; }

private:
    bool m_quit = false;
    float m_deltaTime = 0.0f;
    uint64_t m_lastFrameTime = 0;

    void updateTiming();
};

} // namespace bro::platform
