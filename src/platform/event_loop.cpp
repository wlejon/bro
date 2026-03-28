#include "platform/event_loop.h"
#include "util/log.h"

#include <SDL3/SDL.h>

namespace bro::platform {

void EventLoop::pollEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                m_quit = true;
                if (onQuit) onQuit();
                break;

            case SDL_EVENT_WINDOW_RESIZED:
                if (onResize) {
                    onResize(static_cast<uint32_t>(event.window.data1),
                             static_cast<uint32_t>(event.window.data2));
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (onMouseDown) {
                    onMouseDown(event.button.x, event.button.y, event.button.button);
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (onMouseUp) {
                    onMouseUp(event.button.x, event.button.y, event.button.button);
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                if (onMouseMove) {
                    onMouseMove(event.motion.x, event.motion.y);
                }
                break;

            case SDL_EVENT_KEY_DOWN:
                if (onKeyDown) {
                    onKeyDown(static_cast<int32_t>(event.key.key),
                              static_cast<int32_t>(event.key.scancode),
                              event.key.mod);
                }
                break;

            case SDL_EVENT_KEY_UP:
                if (onKeyUp) {
                    onKeyUp(static_cast<int32_t>(event.key.key),
                            static_cast<int32_t>(event.key.scancode),
                            event.key.mod);
                }
                break;

            case SDL_EVENT_TEXT_INPUT:
                if (onTextInput) {
                    onTextInput(event.text.text);
                }
                break;

            default:
                break;
        }
    }
}

void EventLoop::updateTiming() {
    uint64_t now = SDL_GetPerformanceCounter();
    if (m_lastFrameTime == 0) {
        m_lastFrameTime = now;
        m_deltaTime = 0.0f;
        return;
    }

    uint64_t frequency = SDL_GetPerformanceFrequency();
    m_deltaTime = static_cast<float>(now - m_lastFrameTime) / static_cast<float>(frequency);
    m_lastFrameTime = now;
}

void EventLoop::run(std::function<void(float deltaTime)> perFrame) {
    m_quit = false;
    m_lastFrameTime = 0;

    LOG_INFO("Event loop started");

    while (!m_quit) {
        updateTiming();
        pollEvents();

        if (!m_quit && perFrame) {
            perFrame(m_deltaTime);
        }
    }

    LOG_INFO("Event loop ended");
}

} // namespace bro::platform
