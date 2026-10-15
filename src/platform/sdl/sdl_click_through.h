#pragma once
// Click-through for an SDL window (Window::setClickThrough): pointer input
// over the window where the page takes none goes to whatever is beneath it,
// other applications' windows included. Internal to src/platform.
//
// Each OS routes input past a window its own way:
//   Windows  WS_EX_LAYERED | WS_EX_TRANSPARENT, toggled to follow the
//            pointer: a low-level mouse hook on a thread of its own watches
//            the pointer, and the main thread asks the page about the point
//            under it and flips the style (sdl_click_through_win.cpp).
//   X11      the XShape input region, set from the page's rects
//            (SDL_SetWindowShape; sdl_click_through.cpp).
//   macOS    NSWindow.ignoresMouseEvents from the same mask (SDL's window
//            shape), plus a global monitor to take input again
//            (sdl_click_through.cpp, sdl_click_through_mac.mm).
// Elsewhere create() returns null and the window reports no click-through.

#include "platform/window.h"

#include <functional>
#include <memory>
#include <vector>

struct SDL_Window;

namespace bro::platform {

class SdlClickThrough {
public:
    /// Null where this window system has no click-through.
    static std::unique_ptr<SdlClickThrough> create(SDL_Window* window);
    virtual ~SdlClickThrough() = default;

    virtual void setEnabled(bool on) = 0;
    /// Window::wantsInputRegion: rects (setRegion) or a point test (setTest).
    virtual bool wantsRegion() const = 0;
    /// The rects take input, in window coordinates.
    virtual void setRegion(const std::vector<WindowRect>& rects) { (void)rects; }
    virtual void setTest(std::function<bool(float x, float y)> takesInput) { (void)takesInput; }
    /// Main thread: ask the test again where the pointer is.
    virtual void refresh() {}
    /// Main thread: act on what the pointer did off it. Cheap when nothing.
    virtual void apply() {}
    /// The window's styles were rewritten behind the tracker's back (SDL's
    /// opacity toggles WS_EX_LAYERED): put its own back.
    virtual void restyle() {}
};

} // namespace bro::platform
