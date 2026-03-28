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
    std::function<void(float x, float y)> onMouseMove;
    std::function<void(int32_t keycode, int32_t scancode, uint16_t mod, bool repeat)> onKeyDown;
    std::function<void(int32_t keycode, int32_t scancode, uint16_t mod, bool repeat)> onKeyUp;
    std::function<void(const std::string& text)> onTextInput;

    /// Polls all pending SDL events and dispatches to callbacks.
    /// Call once per frame.
    void pollEvents();

    /// Runs a blocking loop that polls events each frame until quit.
    /// The perFrame callback is invoked once per iteration.
    void run(std::function<void(float deltaTime)> perFrame);

    bool shouldQuit() const { return m_quit; }

    /// Delta time of the last frame in seconds.
    float getDeltaTime() const { return m_deltaTime; }

private:
    bool m_quit = false;
    float m_deltaTime = 0.0f;
    uint64_t m_lastFrameTime = 0;

    void updateTiming();
};

} // namespace bro::platform
