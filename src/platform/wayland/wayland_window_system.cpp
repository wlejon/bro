// The Wayland window system: bro as a Wayland client of its own, through
// browl (the protocol code: xdg-shell, the seat, the data device, text input,
// presentation time, xdg-activation, fractional scale, decorations). This
// file holds the connection and the services; wayland_window.cpp the window,
// wayland_event_loop.cpp the input. Gamepads, power, URL opening and native
// dialogs still come from SDL used as a library (no SDL video).
#include "platform/backends.h"
#include "platform/clipboard.h"
#include "platform/displays.h"
#include "platform/event_loop.h"
#include "platform/gamepads.h"
#include "platform/keyboard.h"
#include "platform/sdl/sdl_backend.h"
#include "platform/system_info.h"
#include "platform/wayland/wayland_backend.h"
#include "platform/wayland/wayland_window.h"
#include "util/log.h"

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace bro::platform {

namespace wl {

// --- Connection ---

Connection* Connection::get(std::string* why) {
    static std::unique_ptr<Connection> s_conn;
    static bool s_tried = false;
    static std::string s_why;
    if (s_tried) {
        if (!s_conn && why) *why = s_why;
        return s_conn.get();
    }
    s_tried = true;

    std::string err;
    auto display = browl::Display::connect("", &err);
    if (!display) {
        s_why = err;
        if (why) *why = s_why;
        return nullptr;
    }
    if (!display->has_compositor() || !display->has_xdg_shell()) {
        s_why = "the compositor offers no xdg-shell";
        if (why) *why = s_why;
        return nullptr;
    }
    // Without xdg-decoration a window would need client-side decorations,
    // which this backend does not draw (SDL draws them through libdecor).
    // Forcing the backend accepts frameless windows there.
    const char* force = std::getenv("BRO_WINDOW_SYSTEM");
    const bool forced = force && std::strcmp(force, "wayland") == 0;
    if (!display->has_decoration_manager() && !forced) {
        s_why = "the compositor has no xdg-decoration (windows would have no frame); BRO_WINDOW_SYSTEM=wayland forces it";
        if (why) *why = s_why;
        return nullptr;
    }

    s_conn.reset(new Connection());
    s_conn->display_ = std::move(display);
    s_conn->display_->enable_input();
    // Seat capabilities, the keymap and the outputs' details arrive now.
    s_conn->roundtrip();
    s_conn->roundtrip();

    auto& d = *s_conn->display_;
    LOG_INFO("Wayland: connected; viewporter %d, fractional-scale %d, presentation %d, activation %d, "
             "cursor-shape %d, text-input %d, primary-selection %d, pointer-constraints %d, toplevel-icon %d",
             d.has_viewporter(), d.has_fractional_scale(), d.has_presentation(), d.has_activation(),
             d.has_cursor_shape(), d.has_text_input(), d.has_primary_selection(),
             d.has_pointer_constraints(), d.has_toplevel_icon());
    return s_conn.get();
}

browl::Seat* Connection::seat() const {
    auto s = display_->default_seat();
    return s.get();
}

void Connection::pump(int timeoutMs) {
    browl::Display& d = *display_;
    if (lost_) return;
    while (!d.prepare_read()) {
        if (d.dispatch_pending() < 0) {
            lost_ = true;
            return;
        }
    }
    if (d.flush() < 0 && errno != EAGAIN) {
        d.cancel_read();
        lost_ = true;
        return;
    }
    pollfd p{d.fd(), POLLIN, 0};
    const int r = ::poll(&p, 1, timeoutMs);
    if (r > 0) {
        if (d.read_events() < 0) lost_ = true;
    } else {
        d.cancel_read();
    }
    if (d.dispatch_pending() < 0) lost_ = true;
    for (auto& ev : d.events().drain()) {
        route(ev);
        if (!std::holds_alternative<browl::FrameDoneEvent>(ev) &&
            !std::holds_alternative<browl::PresentationFeedbackEvent>(ev))
            ++frameWorthy_;
        backlog_.push_back(std::move(ev));
    }
    if (lost_) LOG_ERROR("Wayland: lost the connection to the compositor");
}

void Connection::roundtrip() {
    display_->roundtrip();
    pump(0);
}

std::deque<browl::ShellEvent> Connection::takeBacklog() {
    std::deque<browl::ShellEvent> out;
    out.swap(backlog_);
    frameWorthyTaken_ = frameWorthy_;
    return out;
}

void Connection::route(const browl::ShellEvent& ev) {
    if (auto* e = std::get_if<browl::WindowConfigureEvent>(&ev)) {
        if (WaylandWindow* w = window(e->surface_id)) w->applyConfigure(e->snapshot);
    } else if (auto* e = std::get_if<browl::WindowScaleEvent>(&ev)) {
        if (WaylandWindow* w = window(e->surface_id)) w->applyScale(e->scale120);
    } else if (auto* e = std::get_if<browl::PresentationFeedbackEvent>(&ev)) {
        if (WaylandWindow* w = window(e->surface_id)) w->addPresentation(*e);
    } else if (auto* e = std::get_if<browl::FrameDoneEvent>(&ev)) {
        if (WaylandWindow* w = window(e->surface_id)) w->frameDone(e->request);
    } else if (auto* e = std::get_if<browl::ActivationTokenEvent>(&ev)) {
        auto it = tokenRequests_.find(e->request);
        if (it == tokenRequests_.end()) return;
        const browl::SurfaceId target = it->second;
        tokenRequests_.erase(it);
        if (target == browl::kNoSurface || e->token.empty()) return;
        if (WaylandWindow* w = window(target)) display_->activate(e->token, w->wl().wl_surface_ptr());
    } else if (auto* e = std::get_if<browl::KeyboardEnterEvent>(&ev)) {
        if (WaylandWindow* w = window(e->surface_id)) w->setFocused(true);
    } else if (auto* e = std::get_if<browl::KeyboardLeaveEvent>(&ev)) {
        if (WaylandWindow* w = window(e->surface_id)) w->setFocused(false);
    }
}

void Connection::addWindow(WaylandWindow* w) { windows_[w->surfaceId()] = w; }

void Connection::removeWindow(WaylandWindow* w) {
    windows_.erase(w->surfaceId());
    for (auto it = tokenRequests_.begin(); it != tokenRequests_.end();) {
        if (it->second == w->surfaceId()) it = tokenRequests_.erase(it);
        else ++it;
    }
}

WaylandWindow* Connection::window(browl::SurfaceId id) const {
    auto it = windows_.find(id);
    return it == windows_.end() ? nullptr : it->second;
}

void Connection::activate(WaylandWindow* w, const std::string& token) {
    if (!display_->has_activation()) return;
    if (!token.empty()) {
        display_->activate(token, w->wl().wl_surface_ptr());
        display_->flush();
        return;
    }
    // A token of our own. When the window has the keyboard, the newest input
    // serial proves the user asked; otherwise none is given, and the
    // compositor's policy decides (helm's honours it).
    browl::Seat* s = w->isFocused() ? seat() : nullptr;
    const browl::RequestId id = display_->request_activation_token(
        "", w->isFocused() ? w->wl().wl_surface_ptr() : nullptr, s, s ? s->last_input_serial() : 0);
    if (id) tokenRequests_[id] = w->surfaceId();
    display_->flush();
}

void Connection::requestAttention(WaylandWindow* w) {
    if (!display_->has_activation()) return;
    const browl::RequestId id = display_->request_activation_token("", w->wl().wl_surface_ptr());
    if (id) tokenRequests_[id] = browl::kNoSurface;
    display_->flush();
}

std::string Connection::takePendingToken() {
    std::string t;
    t.swap(pendingToken_);
    return t;
}

}  // namespace wl

namespace {

using wl::Connection;

class WaylandDisplays final : public Displays {
public:
    explicit WaylandDisplays(Connection& c) : c_(c) {}

    std::vector<DisplayInfo> list(const Window* window) override {
        std::vector<DisplayInfo> result;
        const uint32_t current = window ? window->currentDisplay() : 0;
        bool first = true;
        for (const auto& out : c_.display().outputs()) {
            const browl::OutputSnapshot s = out->snapshot();
            DisplayInfo info;
            info.id = s.id;
            info.name = !s.description.empty() ? s.description : s.name;
            browl::Rect r = s.logical;
            if (r.width <= 0 || r.height <= 0) {
                const int scale = std::max(1, s.scale);
                r = {s.geometry.x, s.geometry.y, s.current_mode.width / scale, s.current_mode.height / scale};
            }
            info.x = r.x;
            info.y = r.y;
            info.width = r.width;
            info.height = r.height;
            // Wayland does not tell clients about panels: the work area is
            // the whole output.
            info.workX = r.x;
            info.workY = r.y;
            info.workWidth = r.width;
            info.workHeight = r.height;
            info.refreshRate = static_cast<float>(s.current_mode.refresh_mhz) / 1000.0f;
            info.contentScale = static_cast<float>(std::max(1, s.scale));
            info.isPrimary = first;
            info.isCurrent = s.id == current;
            first = false;
            result.push_back(std::move(info));
        }
        return result;
    }

    std::vector<DisplayModeInfo> fullscreenModes(uint32_t displayId) override {
        std::vector<DisplayModeInfo> result;
        auto out = c_.display().output_by_id(displayId);
        if (!out) return result;
        for (const auto& m : out->snapshot().modes)
            result.push_back({m.width, m.height, static_cast<float>(m.refresh_mhz) / 1000.0f});
        return result;
    }

private:
    Connection& c_;
};

// The active layout is the seat's keymap; until the compositor has sent one,
// US-QWERTY (as SDL answers before it has a keymap).
class WaylandKeyboard final : public Keyboard {
public:
    explicit WaylandKeyboard(Connection& c) : c_(c) {}

    KeyMods modState() override { return c_.modState(); }

    Keycode eventKeycode(Scancode scancode) override {
        auto km = keymap();
        return wl::layoutKeycode(km.get(), scancode, kmod::None);
    }

    Keycode layoutKeycode(Scancode scancode, KeyMods mods) override {
        auto km = keymap();
        return wl::layoutKeycode(km.get(), scancode, mods);
    }

    Scancode scancodeFromKey(Keycode key, KeyMods* mods) override {
        if (mods) *mods = kmod::None;
        auto km = keymap();
        if (!km || (key & (kScancodeMask | kExtendedMask)) || key == kc::Unknown)
            return defaultScancodeFromKey(key, mods);
        // The key that types `key` unshifted, else shifted.
        for (KeyMods m : {kmod::None, kmod::LShift}) {
            for (Scancode s = sc::A; s < sc::CapsLock; ++s) {
                if (wl::layoutKeycode(km.get(), s, m) == key) {
                    if (mods) *mods = m;
                    return s;
                }
            }
            for (Scancode s : {sc::NonUsBackslash, sc::International1, sc::International3}) {
                if (wl::layoutKeycode(km.get(), s, m) == key) {
                    if (mods) *mods = m;
                    return s;
                }
            }
        }
        return defaultScancodeFromKey(key, mods);
    }

private:
    std::shared_ptr<const browl::Keymap> keymap() {
        browl::Seat* s = c_.seat();
        return s ? s->keymap() : nullptr;
    }
    Connection& c_;
};

class WaylandClipboard final : public Clipboard {
public:
    explicit WaylandClipboard(Connection& c) : c_(c) {}

    bool setText(const std::string& text) override { return set(browl::Selection::Clipboard, text); }
    std::string getText(bool* ok) override {
        auto t = get(browl::Selection::Clipboard, ok);
        return t ? *t : std::string();
    }
    std::optional<std::vector<uint8_t>> getData(const std::string& mimeType) override {
        browl::Seat* s = c_.seat();
        if (!s) return std::nullopt;
        return s->read_selection(browl::Selection::Clipboard, mimeType);
    }
    bool setPrimaryText(const std::string& text) override { return set(browl::Selection::Primary, text); }
    std::optional<std::string> getPrimaryText() override {
        browl::Seat* s = c_.seat();
        if (!s || !s->has_selection_protocol(browl::Selection::Primary)) return std::nullopt;
        bool ok = true;
        auto t = get(browl::Selection::Primary, &ok);
        if (!ok) return std::nullopt;
        return t ? *t : std::string();
    }

private:
    bool set(browl::Selection which, const std::string& text) {
        browl::Seat* s = c_.seat();
        if (!s || !s->has_selection_protocol(which)) return false;
        const bool ok = s->set_selection(which, browl::text_selection(text));
        c_.display().flush();
        return ok;
    }

    // "" with ok for an empty selection; ok = false only for a read that
    // failed (an offer that would not deliver).
    std::optional<std::string> get(browl::Selection which, bool* ok) {
        if (ok) *ok = true;
        browl::Seat* s = c_.seat();
        if (!s || !s->has_selection_protocol(which)) {
            if (ok) *ok = false;
            return std::nullopt;
        }
        // Pick up a selection change the compositor sent since the last poll.
        c_.pump(0);
        const auto types = s->selection_mime_types(which);
        if (types.empty()) return std::string();
        for (const char* mime : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"}) {
            if (std::find(types.begin(), types.end(), mime) == types.end()) continue;
            auto bytes = s->read_selection(which, mime);
            if (!bytes) {
                if (ok) *ok = false;
                return std::nullopt;
            }
            return std::string(bytes->begin(), bytes->end());
        }
        return std::string();  // no text in it (an image): an empty text clipboard
    }

    Connection& c_;
};

// Power and URL opening are SDL's (no video needed); the light/dark
// preference is the desktop portal's, which is where SDL reads it too.
class WaylandSystemInfo final : public SystemInfo {
public:
    PowerInfo power() override { return sdlSystemInfo().power(); }
    SystemTheme theme() override {
        switch (wl::portalColorScheme()) {
            case 1: return SystemTheme::Dark;
            case 2: return SystemTheme::Light;
            default: return SystemTheme::Unknown;
        }
    }
    bool openUrl(const std::string& url, std::string* error) override {
        return sdlSystemInfo().openUrl(url, error);
    }
};

class WaylandWindowSystem final : public WindowSystem {
public:
    explicit WaylandWindowSystem(Connection& c)
        : c_(c), displays_(c), keyboard_(c), clipboard_(c) {}

    const char* name() const override { return "wayland"; }
    std::string driverName() const override { return "wayland"; }

    std::unique_ptr<Window> createWindow(const WindowConfig& config) override {
        return std::make_unique<wl::WaylandWindow>(c_, config);
    }

    std::unique_ptr<EventLoop> createEventLoop() override { return wl::createWaylandEventLoop(c_); }

    void pumpEvents() override { c_.pump(0); }

    std::vector<std::string> vulkanInstanceExtensions() override {
        return {"VK_KHR_surface", "VK_KHR_wayland_surface"};
    }

    void setActivationToken(const std::string& token) override { c_.setPendingToken(token); }

    Displays& displays() override { return displays_; }
    Keyboard& keyboard() override { return keyboard_; }
    Clipboard& clipboard() override { return clipboard_; }
    DialogBackend& dialogs() override { return sdlDialogs(); }
    SystemInfo& systemInfo() override { return systemInfo_; }
    Gamepads& gamepads() override { return sdlGamepads(); }

private:
    Connection& c_;
    WaylandDisplays displays_;
    WaylandKeyboard keyboard_;
    WaylandClipboard clipboard_;
    WaylandSystemInfo systemInfo_;
};

}  // namespace

WindowSystem* waylandWindowSystem(std::string* why) {
    static std::unique_ptr<WaylandWindowSystem> s_system;
    if (s_system) return s_system.get();
    Connection* c = Connection::get(why);
    if (!c) return nullptr;
    s_system = std::make_unique<WaylandWindowSystem>(*c);
    return s_system.get();
}

}  // namespace bro::platform
