#include "platform/event_loop.h"
#include "util/interrupt.h"
#include "util/log.h"

#include <SDL3/SDL.h>

namespace bro::platform {

// SDL delivers finger coordinates normalized to 0-1 across the window;
// convert to window coordinates so touch flows through the same coordinate
// space as mouse input. SDL3 mouse events report window coordinates (points,
// not pixels), and SDL_GetWindowSize returns the same units, so this stays
// consistent under DPI scaling.
static void fingerWindowCoords(const SDL_TouchFingerEvent& tf,
                               float& outX, float& outY) {
    int w = 0, h = 0;
    if (SDL_Window* win = SDL_GetWindowFromID(tf.windowID)) {
        SDL_GetWindowSize(win, &w, &h);
    }
    outX = tf.x * static_cast<float>(w);
    outY = tf.y * static_cast<float>(h);
}

void EventLoop::pollEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                // Treat the window close button like Ctrl+C: tell JS to bail
                // out immediately. A second close request hard-exits via the
                // same path Ctrl+C uses.
                ::bro::util::requestInterrupt();
                m_quit = true;
                if (onQuit) onQuit();
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (onCloseRequested) {
                    onCloseRequested(event.window.windowID);
                }
                break;

            case SDL_EVENT_WINDOW_RESIZED:
                if (onResize) {
                    onResize(event.window.windowID,
                             static_cast<uint32_t>(event.window.data1),
                             static_cast<uint32_t>(event.window.data2));
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (onMouseDown) {
                    onMouseDown(event.button.windowID,
                                event.button.x, event.button.y, event.button.button);
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (onMouseUp) {
                    onMouseUp(event.button.windowID,
                              event.button.x, event.button.y, event.button.button);
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                if (onMouseMove) {
                    onMouseMove(event.motion.windowID,
                                event.motion.x, event.motion.y,
                                event.motion.xrel, event.motion.yrel);
                }
                break;

            case SDL_EVENT_KEY_DOWN:
                if (onKeyDown) {
                    onKeyDown(event.key.windowID,
                              static_cast<int32_t>(event.key.key),
                              static_cast<int32_t>(event.key.scancode),
                              event.key.mod,
                              event.key.repeat);
                }
                break;

            case SDL_EVENT_KEY_UP:
                if (onKeyUp) {
                    onKeyUp(event.key.windowID,
                            static_cast<int32_t>(event.key.key),
                            static_cast<int32_t>(event.key.scancode),
                            event.key.mod,
                            false);
                }
                break;

            case SDL_EVENT_TEXT_INPUT:
                if (onTextInput) {
                    onTextInput(event.text.windowID, event.text.text);
                }
                break;

            case SDL_EVENT_TEXT_EDITING:
                if (onTextEditing) {
                    onTextEditing(event.edit.windowID,
                                  event.edit.text ? event.edit.text : "",
                                  event.edit.start, event.edit.length);
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                if (onWheel) {
                    onWheel(event.wheel.windowID,
                            event.wheel.mouse_x, event.wheel.mouse_y,
                            event.wheel.x, event.wheel.y);
                }
                break;

            // A multi-file drop arrives as DROP_BEGIN, N × DROP_FILE,
            // DROP_COMPLETE. Accumulate the group and dispatch it once, so the
            // DOM gets a single `drop` carrying every path.
            case SDL_EVENT_DROP_BEGIN:
                flushDropGroup();          // defensive: unterminated prior group
                m_dropActive = true;
                m_dropWindowId = event.drop.windowID;
                m_dropX = event.drop.x;
                m_dropY = event.drop.y;
                m_dropPaths.clear();
                break;

            case SDL_EVENT_DROP_FILE:
                if (event.drop.data) {
                    if (m_dropActive) {
                        // DROP_BEGIN's position is (0,0) on some backends; the
                        // per-file events carry the real cursor position.
                        m_dropWindowId = event.drop.windowID;
                        m_dropX = event.drop.x;
                        m_dropY = event.drop.y;
                        m_dropPaths.emplace_back(event.drop.data);
                    } else if (onDropFile) {
                        onDropFile(event.drop.windowID, { std::string(event.drop.data) },
                                   event.drop.x, event.drop.y);
                    }
                }
                break;

            case SDL_EVENT_DROP_COMPLETE:
                flushDropGroup();
                break;

            case SDL_EVENT_DROP_TEXT:
                if (onDropText && event.drop.data) {
                    onDropText(event.drop.windowID,
                               event.drop.data, event.drop.x, event.drop.y);
                }
                break;

            case SDL_EVENT_GAMEPAD_ADDED:
                if (onGamepadAdded) onGamepadAdded(event.gdevice.which);
                break;

            case SDL_EVENT_GAMEPAD_REMOVED:
                if (onGamepadRemoved) onGamepadRemoved(event.gdevice.which);
                break;

            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (onGamepadButton) {
                    onGamepadButton(event.gbutton.which,
                                    static_cast<int>(event.gbutton.button),
                                    event.gbutton.down);
                }
                break;

            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (onGamepadAxis) {
                    // Normalize Sint16 to float: sticks -1..1 (SDL min is
                    // -32768, so clamp), triggers land in 0..1 naturally.
                    float v = static_cast<float>(event.gaxis.value) / 32767.0f;
                    if (v < -1.0f) v = -1.0f;
                    onGamepadAxis(event.gaxis.which,
                                  static_cast<int>(event.gaxis.axis), v);
                }
                break;

            case SDL_EVENT_FINGER_DOWN:
                if (onFingerDown) {
                    float x, y;
                    fingerWindowCoords(event.tfinger, x, y);
                    onFingerDown(event.tfinger.windowID,
                                 static_cast<uint64_t>(event.tfinger.fingerID),
                                 x, y, event.tfinger.pressure);
                }
                break;

            case SDL_EVENT_FINGER_MOTION:
                if (onFingerMove) {
                    float x, y;
                    fingerWindowCoords(event.tfinger, x, y);
                    onFingerMove(event.tfinger.windowID,
                                 static_cast<uint64_t>(event.tfinger.fingerID),
                                 x, y, event.tfinger.pressure);
                }
                break;

            case SDL_EVENT_FINGER_UP:
                if (onFingerUp) {
                    float x, y;
                    fingerWindowCoords(event.tfinger, x, y);
                    onFingerUp(event.tfinger.windowID,
                               static_cast<uint64_t>(event.tfinger.fingerID), x, y);
                }
                break;

            case SDL_EVENT_FINGER_CANCELED:
                if (onFingerCancel) {
                    float x, y;
                    fingerWindowCoords(event.tfinger, x, y);
                    onFingerCancel(event.tfinger.windowID,
                                   static_cast<uint64_t>(event.tfinger.fingerID), x, y);
                }
                break;

            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (onFocusLost) onFocusLost(event.window.windowID);
                break;

            case SDL_EVENT_WINDOW_MINIMIZED:
                if (onMinimized) onMinimized(event.window.windowID);
                break;

            case SDL_EVENT_WINDOW_MAXIMIZED:
                if (onMaximized) onMaximized(event.window.windowID);
                break;

            case SDL_EVENT_WINDOW_RESTORED:
                if (onRestored) onRestored(event.window.windowID);
                break;

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                if (onFocusGained) onFocusGained(event.window.windowID);
                break;

            case SDL_EVENT_WINDOW_OCCLUDED:
                if (onOccluded) onOccluded(event.window.windowID);
                break;

            case SDL_EVENT_WINDOW_EXPOSED:
                if (onExposed) onExposed(event.window.windowID);
                break;

            case SDL_EVENT_SYSTEM_THEME_CHANGED:
                if (onSystemThemeChanged) onSystemThemeChanged();
                break;

            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                if (onDisplayScaleChanged) onDisplayScaleChanged(event.window.windowID);
                break;

            default:
                break;
        }
    }
}

// Dispatch whatever the current DROP_BEGIN..DROP_COMPLETE group accumulated.
// A group with no files (drag cancelled over the window) fires nothing.
void EventLoop::flushDropGroup() {
    if (!m_dropActive) return;
    m_dropActive = false;
    if (m_dropPaths.empty()) return;
    if (onDropFile) onDropFile(m_dropWindowId, m_dropPaths, m_dropX, m_dropY);
    m_dropPaths.clear();
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
