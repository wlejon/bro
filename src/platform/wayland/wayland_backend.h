#pragma once
// The Wayland window system's shared state: one browl::Display for the
// process, the windows on it, and the events read from it but not yet
// handed to the EventLoop. Internal to src/platform/wayland.
//
// Threading: everything here runs on the main thread (the one that makes
// windows and polls the EventLoop), except Window::beforePresent, which the
// engine calls from the thread that presents, and the clipboard reads, which
// browl allows from any thread.

#include "platform/keys.h"
#include "platform/window.h"

#include <browl/browl.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::platform {
class EventLoop;
}

namespace bro::platform::wl {

class WaylandWindow;

class Connection {
public:
    /// The process's connection, made on the first call. Null (with `why`)
    /// when there is no compositor, or it lacks xdg-shell.
    static Connection* get(std::string* why = nullptr);

    browl::Display& display() { return *display_; }
    /// The seat input comes from (the first); null until the compositor
    /// announces one.
    browl::Seat* seat() const;

    /// Read whatever the compositor sent and dispatch it, waiting up to
    /// `timeoutMs` for something to arrive (0: just what is there). Window
    /// state (configures, scale, presentation) is applied to the windows at
    /// once; every event is also queued for the EventLoop.
    void pump(int timeoutMs = 0);
    /// Round-trip to the compositor and pump what came back.
    void roundtrip();
    std::deque<browl::ShellEvent> takeBacklog();
    /// The connection broke (the compositor went away or killed us).
    bool lost() const { return lost_; }

    void addWindow(WaylandWindow* w);
    void removeWindow(WaylandWindow* w);
    WaylandWindow* window(browl::SurfaceId id) const;
    size_t windowCount() const { return windows_.size(); }
    const std::unordered_map<browl::SurfaceId, WaylandWindow*>& windows() const { return windows_; }

    /// Activate `w` with `token`, or with a token asked for now when empty.
    void activate(WaylandWindow* w, const std::string& token);
    /// Ask for a token without spending it: a compositor marks the window
    /// urgent (flash).
    void requestAttention(WaylandWindow* w);
    void setPendingToken(const std::string& token) { pendingToken_ = token; }
    std::string takePendingToken();

    /// The modifier state as the EventLoop tracks it (left/right from the
    /// keys held, locks from the keymap state).
    KeyMods modState() const { return modState_; }
    void setModState(KeyMods m) { modState_ = m; }

    /// The last pointer position, per window (for wheel events).
    float pointerX = 0.0f, pointerY = 0.0f;
    browl::SurfaceId pointerSurface = browl::kNoSurface;

private:
    Connection() = default;
    void route(const browl::ShellEvent& ev);

    std::unique_ptr<browl::Display> display_;
    std::deque<browl::ShellEvent> backlog_;
    std::unordered_map<browl::SurfaceId, WaylandWindow*> windows_;
    // Activation tokens asked for, and the window each is for (0 = flash only).
    std::unordered_map<browl::RequestId, browl::SurfaceId> tokenRequests_;
    std::string pendingToken_;
    KeyMods modState_ = 0;
    bool lost_ = false;
};

/// The Keycode for a key event: what `scancode` types unshifted in the
/// active layout (`keymap` null: US-QWERTY), or its scancode keycode for a
/// key that types nothing.
Keycode layoutKeycode(const browl::Keymap* keymap, Scancode scancode, KeyMods mods);
/// The evdev key code for a scancode; 0 for none.
uint32_t evdevKeyFromScancode(Scancode scancode);
/// bro modifier bits as browl::modifier bits, and back (locks and AltGr only;
/// left/right comes from the keys held).
uint32_t browlModifiers(KeyMods mods);

std::unique_ptr<EventLoop> createWaylandEventLoop(Connection& conn);

/// bro's cursor shape as browl's (CSS-named) one.
browl::CursorShape toBrowlCursor(CursorShape shape);

/// The OS light/dark preference from the XDG desktop portal
/// (org.freedesktop.appearance color-scheme): 1 dark, 2 light, 0 none / no
/// portal. Read on a background thread; `changed` is set when it flips.
int portalColorScheme();
bool portalColorSchemeChanged();

}  // namespace bro::platform::wl
