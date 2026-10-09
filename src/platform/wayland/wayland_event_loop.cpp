// The Wayland backend's EventLoop: browl's events in bro's terms.
//
// What the compositor leaves to clients is done here, the way SDL's Wayland
// backend does it, so apps see the same events on either backend: key repeat
// (from wl_keyboard.repeat_info), text from key presses while text input is
// on, the left/right modifier bits, wheel detents, quitting when the last
// window is asked to close, and "minimized until activated".
#include "platform/evdev_keymap.h"
#include "platform/event_loop.h"
#include "platform/sdl/sdl_backend.h"
#include "platform/wayland/wayland_backend.h"
#include "platform/wayland/wayland_window.h"
#include "util/interrupt.h"
#include "util/log.h"
#include "util/time.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <type_traits>

namespace bro::platform::wl {

namespace {

constexpr double kWheelAxisUnit = 10.0;  // continuous scroll px per detent (SDL's WAYLAND_WHEEL_AXIS_UNIT)

// Utf-8 byte offset -> character count, for IME cursor positions.
int32_t utf8Chars(const std::string& s, int32_t bytes) {
    if (bytes <= 0) return 0;
    int32_t n = 0;
    for (int32_t i = 0; i < bytes && i < static_cast<int32_t>(s.size()); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++n;
    return n;
}

bool printable(const std::string& utf8) {
    if (utf8.empty()) return false;
    const auto c = static_cast<unsigned char>(utf8[0]);
    return c >= 0x20 && c != 0x7f;
}

KeyMods modifierKeyBit(Scancode s) {
    switch (s) {
        case sc::LShift: return kmod::LShift;
        case sc::RShift: return kmod::RShift;
        case sc::LCtrl: return kmod::LCtrl;
        case sc::RCtrl: return kmod::RCtrl;
        case sc::LAlt: return kmod::LAlt;
        case sc::RAlt: return kmod::RAlt;
        case sc::LGui: return kmod::LGui;
        case sc::RGui: return kmod::RGui;
        default: return 0;
    }
}

std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// text/uri-list: one URI per line, '#' comments; file:// URIs become paths.
std::vector<std::string> pathsFromUriList(const std::string& list) {
    std::vector<std::string> paths;
    size_t pos = 0;
    while (pos < list.size()) {
        size_t end = list.find('\n', pos);
        if (end == std::string::npos) end = list.size();
        std::string line = list.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("file://", 0) == 0) {
            std::string rest = line.substr(7);
            const size_t slash = rest.find('/');  // skip a host part
            if (slash != std::string::npos) rest = rest.substr(slash);
            paths.push_back(percentDecode(rest));
        }
    }
    return paths;
}

class WaylandEventLoop final : public EventLoop {
public:
    explicit WaylandEventLoop(Connection& c) : c_(c) { gamepads_ = sdlStartGamepadsOnly(); }

    void pollEvents() override;
    // Wayland has no modal OS loop: nothing ever keeps pollEvents from returning.
    void setModalWindowEventHook(std::function<void()>) override {}

private:
    void handle(const browl::ShellEvent& ev);
    void reportWindows();
    void keyRepeat();
    void quit();
    void updateMods();
    uint32_t winId(browl::SurfaceId id) const { return static_cast<uint32_t>(id); }
    WaylandWindow* win(browl::SurfaceId id) const { return c_.window(id); }

    Connection& c_;
    bool gamepads_ = false;

    // Modifiers: left/right from the keys held, locks from the keymap state.
    KeyMods heldMods_ = 0;
    KeyMods lockMods_ = 0;

    // Key repeat.
    struct Repeat {
        bool active = false;
        browl::SurfaceId surface = browl::kNoSurface;
        uint32_t key = 0;
        Scancode scancode = 0;
        std::string text;
        double nextMs = 0.0;
    } repeat_;

    // Touch points, for the surface and position of up/cancel.
    struct Touch {
        browl::SurfaceId surface;
        float x, y;
    };
    std::map<int32_t, Touch> touches_;

    float lastX_ = 0.0f, lastY_ = 0.0f;
    bool preediting_ = false;
};

void WaylandEventLoop::quit() {
    ::bro::util::requestInterrupt();
    m_quit = true;
    if (onQuit) onQuit();
}

void WaylandEventLoop::updateMods() { c_.setModState(static_cast<KeyMods>(heldMods_ | lockMods_)); }

void WaylandEventLoop::pollEvents() {
    c_.pump(0);
    for (auto& ev : c_.takeBacklog()) handle(ev);
    reportWindows();
    keyRepeat();
    if (gamepads_) sdlPollGamepadEvents(*this);
    if (portalColorSchemeChanged() && onSystemThemeChanged) onSystemThemeChanged();
    if (c_.lost() && !m_quit) quit();
}

void WaylandEventLoop::reportWindows() {
    // Indexed copy: a handler may close a window.
    std::vector<WaylandWindow*> windows;
    for (auto& [id, w] : c_.windows()) windows.push_back(w);
    for (WaylandWindow* w : windows) {
        if (!c_.window(w->surfaceId())) continue;
        const uint32_t id = w->windowId();
        if (w->takeMinimizedNow() && onMinimized) onMinimized(id);
        if (w->takeScaleChanged() && onDisplayScaleChanged) onDisplayScaleChanged(id);
        if (w->takeResized() && onResize)
            onResize(id, static_cast<uint32_t>(w->logicalWidth()), static_cast<uint32_t>(w->logicalHeight()));
    }
}

void WaylandEventLoop::keyRepeat() {
    if (!repeat_.active) return;
    browl::Seat* seat = c_.seat();
    const int rate = seat ? seat->repeat_rate() : 0;
    if (rate <= 0) {
        repeat_.active = false;
        return;
    }
    const double period = 1000.0 / rate;
    const double now = util::currentTimeMs();
    // A stalled frame does not burst out a run of repeats.
    if (now - repeat_.nextMs > period * 4) repeat_.nextMs = now - period;
    while (now >= repeat_.nextMs && repeat_.active) {
        repeat_.nextMs += period;
        WaylandWindow* w = win(repeat_.surface);
        if (!w) {
            repeat_.active = false;
            break;
        }
        auto km = seat->keymap();
        const KeyMods mods = c_.modState();
        if (onKeyDown)
            onKeyDown(w->windowId(), layoutKeycode(km.get(), repeat_.scancode, kmod::None), repeat_.scancode, mods,
                      true);
        if (w->textInputActive() && printable(repeat_.text) && onTextInput) onTextInput(w->windowId(), repeat_.text);
    }
}

void WaylandEventLoop::handle(const browl::ShellEvent& ev) {
    std::visit([this](auto&& e) {
        using T = std::decay_t<decltype(e)>;
        browl::Seat* seat = c_.seat();

        if constexpr (std::is_same_v<T, browl::WindowConfigureEvent>) {
            WaylandWindow* w = win(e.surface_id);
            if (!w) return;
            const uint32_t id = w->windowId();
            const uint32_t now = e.snapshot.states, was = w->reportedStates;
            const bool wasMinimized = w->isMinimized();
            w->reportedStates = now;
            using namespace browl::window_state;
            if ((now & Maximized) && !(was & Maximized) && onMaximized) onMaximized(id);
            if (!(now & Maximized) && (was & Maximized) && onRestored) onRestored(id);
            if ((now & Suspended) && !(was & Suspended) && onOccluded) onOccluded(id);
            if (!(now & Suspended) && (was & Suspended) && onExposed) onExposed(id);
            // Suspended reads as minimized (WaylandWindow::isMinimized): the
            // page is hidden while the compositor shows nothing of it.
            if (w->isMinimized() != wasMinimized) {
                if (w->isMinimized() && onMinimized) onMinimized(id);
                else if (!w->isMinimized() && onRestored) onRestored(id);
            }
            if ((now & Activated) && w->isMinimized()) {
                w->setMinimizedFlag(false);
                if (onRestored) onRestored(id);
            }
        } else if constexpr (std::is_same_v<T, browl::WindowCloseEvent>) {
            WaylandWindow* w = win(e.surface_id);
            if (!w) return;
            const bool last = c_.windowCount() == 1;
            if (onCloseRequested) onCloseRequested(w->windowId());
            if (last) quit();
        } else if constexpr (std::is_same_v<T, browl::PointerEnterEvent>) {
            WaylandWindow* w = win(e.surface_id);
            c_.pointerSurface = e.surface_id;
            if (!w) return;
            if (seat && !w->relativeMode()) seat->set_cursor(toBrowlCursor(w->cursorShape()));
            lastX_ = static_cast<float>(e.x);
            lastY_ = static_cast<float>(e.y);
            if (onMouseMove) onMouseMove(w->windowId(), lastX_, lastY_, 0.0f, 0.0f);
        } else if constexpr (std::is_same_v<T, browl::PointerLeaveEvent>) {
            if (c_.pointerSurface == e.surface_id) c_.pointerSurface = browl::kNoSurface;
        } else if constexpr (std::is_same_v<T, browl::PointerMotionEvent>) {
            WaylandWindow* w = win(e.surface_id);
            if (!w) return;
            const float x = static_cast<float>(e.x), y = static_cast<float>(e.y);
            const float dx = x - lastX_, dy = y - lastY_;
            lastX_ = x;
            lastY_ = y;
            if (!w->relativeMode() && onMouseMove) onMouseMove(w->windowId(), x, y, dx, dy);
        } else if constexpr (std::is_same_v<T, browl::PointerRelativeMotionEvent>) {
            WaylandWindow* w = win(c_.pointerSurface);
            if (!w || !w->relativeMode()) return;
            if (onMouseMove)
                onMouseMove(w->windowId(), lastX_, lastY_, static_cast<float>(e.dx_unaccel),
                            static_cast<float>(e.dy_unaccel));
        } else if constexpr (std::is_same_v<T, browl::PointerButtonEvent>) {
            WaylandWindow* w = win(e.surface_id);
            if (!w) return;
            if (e.button < kEvdevBtnLeft || e.button > kEvdevBtnExtra) return;  // as SDL: five buttons
            const auto b = static_cast<uint8_t>(evdevButtonToMouseButton(e.button));
            if (e.pressed) {
                if (onMouseDown) onMouseDown(w->windowId(), lastX_, lastY_, b);
            } else {
                if (onMouseUp) onMouseUp(w->windowId(), lastX_, lastY_, b);
            }
        } else if constexpr (std::is_same_v<T, browl::PointerAxisEvent>) {
            WaylandWindow* w = win(e.surface_id);
            if (!w || !onWheel) return;
            // Detents, +y away from the user (Wayland's +y is down).
            float dx = 0.0f, dy = 0.0f;
            if (e.v120x || e.v120y) {
                dx = static_cast<float>(e.v120x) / 120.0f;
                dy = -static_cast<float>(e.v120y) / 120.0f;
            } else {
                dx = static_cast<float>(e.dx / kWheelAxisUnit);
                dy = -static_cast<float>(e.dy / kWheelAxisUnit);
            }
            if (dx != 0.0f || dy != 0.0f) onWheel(w->windowId(), lastX_, lastY_, dx, dy);
        } else if constexpr (std::is_same_v<T, browl::KeyboardEnterEvent>) {
            WaylandWindow* w = win(e.surface_id);
            if (!w) return;
            heldMods_ = 0;
            updateMods();
            if (onFocusGained) onFocusGained(w->windowId());
            if (w->isMinimized()) {
                w->setMinimizedFlag(false);
                if (onRestored) onRestored(w->windowId());
            }
        } else if constexpr (std::is_same_v<T, browl::KeyboardLeaveEvent>) {
            repeat_.active = false;
            heldMods_ = 0;
            updateMods();
            if (WaylandWindow* w = win(e.surface_id); w && onFocusLost) onFocusLost(w->windowId());
        } else if constexpr (std::is_same_v<T, browl::ModifiersEvent>) {
            KeyMods locks = 0;
            if (e.modifiers & browl::modifier::CapsLock) locks |= kmod::Caps;
            if (e.modifiers & browl::modifier::NumLock) locks |= kmod::Num;
            if (e.modifiers & browl::modifier::AltGr) locks |= kmod::Mode;
            if (e.modifiers & browl::modifier::Level5) locks |= kmod::Level5;
            lockMods_ = locks;
            updateMods();
        } else if constexpr (std::is_same_v<T, browl::KeyEvent>) {
            WaylandWindow* w = win(e.surface_id);
            if (!w) return;
            const Scancode s = evdevKeyToScancode(e.key);
            if (const KeyMods bit = modifierKeyBit(s)) {
                if (e.pressed) heldMods_ |= bit;
                else heldMods_ &= static_cast<KeyMods>(~bit);
                updateMods();
            }
            auto km = seat ? seat->keymap() : nullptr;
            const Keycode key = layoutKeycode(km.get(), s, kmod::None);
            const KeyMods mods = c_.modState();
            if (e.pressed) {
                if (onKeyDown) onKeyDown(w->windowId(), key, s, mods, false);
                if (w->textInputActive() && printable(e.utf8) && onTextInput) onTextInput(w->windowId(), e.utf8);
                const int rate = seat ? seat->repeat_rate() : 0;
                if (rate > 0 && km && km->repeats(e.key)) {
                    repeat_.active = true;
                    repeat_.surface = e.surface_id;
                    repeat_.key = e.key;
                    repeat_.scancode = s;
                    repeat_.text = e.utf8;
                    repeat_.nextMs = util::currentTimeMs() + (seat ? seat->repeat_delay_ms() : 600);
                } else if (repeat_.active && repeat_.key != e.key && modifierKeyBit(s) == 0) {
                    repeat_.active = false;
                }
            } else {
                if (repeat_.active && repeat_.key == e.key) repeat_.active = false;
                if (onKeyUp) onKeyUp(w->windowId(), key, s, mods, false);
            }
        } else if constexpr (std::is_same_v<T, browl::TouchDownEvent>) {
            WaylandWindow* w = win(e.surface_id);
            if (!w) return;
            touches_[e.id] = {e.surface_id, static_cast<float>(e.x), static_cast<float>(e.y)};
            if (onFingerDown)
                onFingerDown(w->windowId(), static_cast<uint64_t>(e.id), static_cast<float>(e.x),
                             static_cast<float>(e.y), 1.0f);
        } else if constexpr (std::is_same_v<T, browl::TouchMotionEvent>) {
            auto it = touches_.find(e.id);
            if (it == touches_.end()) return;
            it->second.x = static_cast<float>(e.x);
            it->second.y = static_cast<float>(e.y);
            if (WaylandWindow* w = win(it->second.surface); w && onFingerMove)
                onFingerMove(w->windowId(), static_cast<uint64_t>(e.id), it->second.x, it->second.y, 1.0f);
        } else if constexpr (std::is_same_v<T, browl::TouchUpEvent>) {
            auto it = touches_.find(e.id);
            if (it == touches_.end()) return;
            const Touch t = it->second;
            touches_.erase(it);
            if (WaylandWindow* w = win(t.surface); w && onFingerUp)
                onFingerUp(w->windowId(), static_cast<uint64_t>(e.id), t.x, t.y);
        } else if constexpr (std::is_same_v<T, browl::TouchCancelEvent>) {
            auto all = std::move(touches_);
            touches_.clear();
            for (auto& [id, t] : all)
                if (WaylandWindow* w = win(t.surface); w && onFingerCancel)
                    onFingerCancel(w->windowId(), static_cast<uint64_t>(id), t.x, t.y);
        } else if constexpr (std::is_same_v<T, browl::TextInputFocusEvent>) {
            WaylandWindow* w = win(e.surface_id);
            if (!w || !seat) return;
            if (e.entered && w->textInputActive() && !seat->text_input_enabled()) seat->enable_text_input();
        } else if constexpr (std::is_same_v<T, browl::TextInputEvent>) {
            WaylandWindow* w = win(e.surface_id);
            if (!w) return;
            const uint32_t id = w->windowId();
            if (!e.commit.empty() && onTextInput) onTextInput(id, e.commit);
            if (!e.preedit.empty()) {
                preediting_ = true;
                if (onTextEditing) {
                    const int32_t begin = e.preedit_cursor_begin < 0 ? utf8Chars(e.preedit, static_cast<int32_t>(e.preedit.size()))
                                                                     : utf8Chars(e.preedit, e.preedit_cursor_begin);
                    const int32_t end = e.preedit_cursor_end < 0 ? begin : utf8Chars(e.preedit, e.preedit_cursor_end);
                    onTextEditing(id, e.preedit, begin, std::max(0, end - begin));
                }
            } else if (preediting_) {
                preediting_ = false;
                if (onTextEditing) onTextEditing(id, "", 0, 0);
            }
        } else if constexpr (std::is_same_v<T, browl::DragEnterEvent> || std::is_same_v<T, browl::DragMotionEvent>) {
            // Our own drag over our window: the page goes on dragging, as
            // pointer events stopped when the compositor took the drag.
            WaylandWindow* w = win(e.surface_id);
            if (w && c_.ownDrag && onOwnDragMotion)
                onOwnDragMotion(w->windowId(), static_cast<float>(e.x), static_cast<float>(e.y));
        } else if constexpr (std::is_same_v<T, browl::DragLeaveEvent>) {
            WaylandWindow* w = win(e.surface_id);
            if (w && c_.ownDrag && onOwnDragLeave) onOwnDragLeave(w->windowId());
        } else if constexpr (std::is_same_v<T, browl::DragSourceEvent>) {
            using Kind = browl::DragSourceEvent::Kind;
            if (e.kind != Kind::Finished && e.kind != Kind::Cancelled) return;
            if (!c_.ownDrag) return;
            c_.ownDrag = false;
            const char* action = "none";
            if (e.kind == Kind::Finished)
                action = (e.action & browl::dnd_action::Move) ? "move" : "copy";
            if (onOwnDragEnd) onOwnDragEnd(action);
        } else if constexpr (std::is_same_v<T, browl::DragDropEvent>) {
            WaylandWindow* w = win(e.surface_id);
            if (!w || !seat) return;
            const float x = static_cast<float>(e.x), y = static_cast<float>(e.y);
            if (c_.ownDrag) {
                // Our own drag dropped on our window: the page drops it
                // (with the data it set), nothing to read.
                if (onOwnDragDrop) onOwnDragDrop(w->windowId(), x, y);
                seat->finish_drop();
                return;
            }
            auto has = [&](const char* m) {
                return std::find(e.mime_types.begin(), e.mime_types.end(), m) != e.mime_types.end();
            };
            // Files when the URI list names local ones; anything else (a
            // link dragged out of a browser) arrives as its text.
            bool dropped = false;
            if (has("text/uri-list")) {
                if (auto bytes = seat->read_drop("text/uri-list")) {
                    auto paths = pathsFromUriList(std::string(bytes->begin(), bytes->end()));
                    if (!paths.empty() && onDropFile) {
                        onDropFile(w->windowId(), paths, x, y);
                        dropped = true;
                    }
                }
            }
            if (!dropped) {
                for (const char* m : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "text/uri-list"}) {
                    if (!has(m)) continue;
                    if (auto bytes = seat->read_drop(m); bytes && onDropText)
                        onDropText(w->windowId(), std::string(bytes->begin(), bytes->end()), x, y);
                    break;
                }
            }
            seat->finish_drop();
        }
    }, ev);
}

}  // namespace

std::unique_ptr<EventLoop> createWaylandEventLoop(Connection& conn) {
    return std::make_unique<WaylandEventLoop>(conn);
}

}  // namespace bro::platform::wl
