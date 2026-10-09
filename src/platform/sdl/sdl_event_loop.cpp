#include "platform/sdl/sdl_backend.h"

#include "platform/event_loop.h"
#include "platform/gamepads.h"
#include "platform/keys.h"
#include "util/interrupt.h"
#include "util/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace bro::platform {

// bro's key model is SDL's numbering, so SDL key events pass through as they
// are. Pin the equivalence for every family of keys.
static_assert(sc::A == SDL_SCANCODE_A && sc::Digit1 == SDL_SCANCODE_1 &&
              sc::Return == SDL_SCANCODE_RETURN && sc::NonUsHash == SDL_SCANCODE_NONUSHASH &&
              sc::CapsLock == SDL_SCANCODE_CAPSLOCK && sc::F24 == SDL_SCANCODE_F24 &&
              sc::VolumeDown == SDL_SCANCODE_VOLUMEDOWN && sc::Lang9 == SDL_SCANCODE_LANG9 &&
              sc::KpHexadecimal == SDL_SCANCODE_KP_HEXADECIMAL && sc::RGui == SDL_SCANCODE_RGUI &&
              sc::Mode == SDL_SCANCODE_MODE && sc::EndCall == SDL_SCANCODE_ENDCALL &&
              sc::Count == SDL_SCANCODE_COUNT);
static_assert(kScancodeMask == SDLK_SCANCODE_MASK && kExtendedMask == SDLK_EXTENDED_MASK);
static_assert(kc::Return == SDLK_RETURN && kc::Delete == SDLK_DELETE && kc::A == SDLK_A &&
              kc::PlusMinus == SDLK_PLUSMINUS && kc::F1 == SDLK_F1 && kc::KpEnter == SDLK_KP_ENTER &&
              kc::RGui == SDLK_RGUI && kc::MediaPlayPause == SDLK_MEDIA_PLAY_PAUSE &&
              kc::LeftTab == SDLK_LEFT_TAB && kc::RHyper == SDLK_RHYPER);
static_assert(kmod::LShift == SDL_KMOD_LSHIFT && kmod::Level5 == SDL_KMOD_LEVEL5 &&
              kmod::LCtrl == SDL_KMOD_LCTRL && kmod::RAlt == SDL_KMOD_RALT &&
              kmod::RGui == SDL_KMOD_RGUI && kmod::Num == SDL_KMOD_NUM &&
              kmod::Caps == SDL_KMOD_CAPS && kmod::Mode == SDL_KMOD_MODE &&
              kmod::Scroll == SDL_KMOD_SCROLL && kmod::Ctrl == SDL_KMOD_CTRL &&
              kmod::Shift == SDL_KMOD_SHIFT && kmod::Alt == SDL_KMOD_ALT && kmod::Gui == SDL_KMOD_GUI);
static_assert(static_cast<int>(GamepadButton::South) == SDL_GAMEPAD_BUTTON_SOUTH &&
              static_cast<int>(GamepadButton::Back) == SDL_GAMEPAD_BUTTON_BACK &&
              static_cast<int>(GamepadButton::LeftShoulder) == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER &&
              static_cast<int>(GamepadButton::DpadRight) == SDL_GAMEPAD_BUTTON_DPAD_RIGHT &&
              static_cast<int>(GamepadButton::Touchpad) == SDL_GAMEPAD_BUTTON_TOUCHPAD);
static_assert(static_cast<int>(GamepadAxis::LeftX) == SDL_GAMEPAD_AXIS_LEFTX &&
              static_cast<int>(GamepadAxis::RightY) == SDL_GAMEPAD_AXIS_RIGHTY &&
              static_cast<int>(GamepadAxis::RightTrigger) == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

namespace {

// SDL delivers finger coordinates normalized to 0-1 across the window;
// convert to window coordinates so touch flows through the same coordinate
// space as mouse input. SDL3 mouse events report window coordinates (points,
// not pixels), and SDL_GetWindowSize returns the same units, so this stays
// consistent under DPI scaling.
void fingerWindowCoords(const SDL_TouchFingerEvent& tf, float& outX, float& outY) {
    int w = 0, h = 0;
    if (SDL_Window* win = SDL_GetWindowFromID(tf.windowID)) {
        SDL_GetWindowSize(win, &w, &h);
    }
    outX = tf.x * static_cast<float>(w);
    outY = tf.y * static_cast<float>(h);
}

// A gamepad event to the loop's handlers; false for any other event.
bool dispatchGamepadEvent(const SDL_Event& event, EventLoop& loop) {
    switch (event.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            if (loop.onGamepadAdded) loop.onGamepadAdded(event.gdevice.which);
            return true;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (loop.onGamepadRemoved) loop.onGamepadRemoved(event.gdevice.which);
            return true;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (loop.onGamepadButton) {
                loop.onGamepadButton(event.gbutton.which, static_cast<int>(event.gbutton.button),
                                     event.gbutton.down);
            }
            return true;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (loop.onGamepadAxis) {
                // Normalize Sint16 to float: sticks -1..1 (SDL min is
                // -32768, so clamp), triggers land in 0..1 naturally.
                float v = static_cast<float>(event.gaxis.value) / 32767.0f;
                if (v < -1.0f) v = -1.0f;
                loop.onGamepadAxis(event.gaxis.which, static_cast<int>(event.gaxis.axis), v);
            }
            return true;
        default:
            return false;
    }
}

class SdlEventLoop final : public EventLoop {
public:
    ~SdlEventLoop() override { setModalWindowEventHook(nullptr); }

    void pollEvents() override;
    void setModalWindowEventHook(std::function<void()> hook) override;

    bool canWaitEvents() const override { return true; }
    void waitEvents(double timeoutMs) override {
        if (m_wakePending.exchange(false, std::memory_order_acq_rel)) return;
        if (timeoutMs >= 0.0 && timeoutMs < 2.0) {
            // SDL takes its own pump time off a wait and rounds what is left
            // down to the OS's whole ms: a wait this short would not wait at
            // all. A precise sleep instead (input waits a ms at most).
            SDL_DelayNS(static_cast<Uint64>(timeoutMs * 1e6));
        } else {
            // One ms more than asked for, for the same rounding; a wait that
            // still ends early comes back for the rest.
            const Sint32 ms = timeoutMs < 0.0 ? -1 : static_cast<Sint32>(std::min(std::floor(timeoutMs) + 1.0, 2.0e9));
            SDL_WaitEventTimeout(nullptr, ms);
        }
        m_wakePending.store(false, std::memory_order_release);
    }
    // One queued event per pending wake: an SDL_PushEvent from another
    // thread posts the window a message, which ends the wait (Win32:
    // MsgWaitForMultipleObjects). pollEvents drops the event.
    void wake() override {
        if (m_wakePending.exchange(true, std::memory_order_acq_rel)) return;
        static const Uint32 type = [] {
            const Uint32 t = SDL_RegisterEvents(1);
            return t ? t : static_cast<Uint32>(SDL_EVENT_USER);
        }();
        SDL_Event e;
        SDL_zero(e);
        e.type = type;
        SDL_PushEvent(&e);
    }

private:
    std::atomic<bool> m_wakePending{false};

    // In-progress file drop, accumulated between DROP_BEGIN and DROP_COMPLETE.
    // m_dropActive distinguishes "a group is open" from "no files yet", so a
    // DROP_FILE arriving without the bracketing events (defensive: SDL always
    // sends them today) still dispatches on its own rather than being lost.
    bool m_dropActive = false;
    uint32_t m_dropWindowId = 0;
    float m_dropX = -1.0f, m_dropY = -1.0f;
    std::vector<std::string> m_dropPaths;

    std::function<void()> m_modalHook;
    bool m_watching = false;

    void flushDropGroup();
    static bool SDLCALL modalWatch(void* userdata, SDL_Event* event);
};

// Win32 runs its own message loop while the user drags a window edge or the
// title bar, and SDL_PollEvent does not return until it ends; an event watch
// is called from inside that loop.
bool SDLCALL SdlEventLoop::modalWatch(void* userdata, SDL_Event* event) {
    if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST) {
        auto* self = static_cast<SdlEventLoop*>(userdata);
        if (self->m_modalHook) self->m_modalHook();
    }
    return true;
}

void SdlEventLoop::setModalWindowEventHook(std::function<void()> hook) {
    m_modalHook = std::move(hook);
    if (m_modalHook && !m_watching) {
        SDL_AddEventWatch(modalWatch, this);
        m_watching = true;
    } else if (!m_modalHook && m_watching) {
        SDL_RemoveEventWatch(modalWatch, this);
        m_watching = false;
    }
}

void SdlEventLoop::pollEvents() {
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
            case SDL_EVENT_GAMEPAD_REMOVED:
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                dispatchGamepadEvent(event, *this);
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
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (onDisplayScaleChanged) onDisplayScaleChanged(event.window.windowID);
                break;

            default:
                break;
        }
    }
}

// Dispatch whatever the current DROP_BEGIN..DROP_COMPLETE group accumulated.
// A group with no files (drag cancelled over the window) fires nothing.
void SdlEventLoop::flushDropGroup() {
    if (!m_dropActive) return;
    m_dropActive = false;
    if (m_dropPaths.empty()) return;
    if (onDropFile) onDropFile(m_dropWindowId, m_dropPaths, m_dropX, m_dropY);
    m_dropPaths.clear();
}

}  // namespace

std::unique_ptr<EventLoop> createSdlEventLoop() {
    return std::make_unique<SdlEventLoop>();
}

bool sdlStartGamepadsOnly() {
    static int state = 0;  // 0 untried, 1 running, -1 unavailable
    if (state == 0) {
        // The click-through hint is SdlRuntime's business; nothing here
        // touches video, so SDL never opens a window-system connection.
        if (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            state = 1;
        } else {
            LOG_INFO("SDL gamepad subsystem unavailable: %s", SDL_GetError());
            state = -1;
        }
    }
    return state == 1;
}

void sdlPollGamepadEvents(EventLoop& loop) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) dispatchGamepadEvent(event, loop);
}

} // namespace bro::platform
