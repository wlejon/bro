// Click-through over SDL's window shape (sdl_click_through.h): X11 and macOS.
// Windows has its own file; any other SDL video driver (SDL's own Wayland
// backend, offscreen, dummy) has none, and create() says so.
//
// SDL_SetWindowShape takes an alpha mask the window's size. On X11 it sets
// the XShape *input* region to the mask's opaque pixels (the picture is left
// alone). On macOS SDL sets NSWindow.ignoresMouseEvents from the mask on
// each mouse move the window gets; once it ignores the mouse it gets none,
// so a global event monitor (sdl_click_through_mac.mm) watches the pointer
// come back over the region and lets the window take input again.
#include "platform/sdl/sdl_click_through.h"
#include "util/log.h"

#include <SDL3/SDL.h>

#include <cstring>

namespace bro::platform {

#if !defined(_WIN32)

#if defined(__APPLE__)
// sdl_click_through_mac.mm
void* sdlMacWatchPointer(void (*onMove)(void*), void* ctx);
void sdlMacUnwatchPointer(void* token);
void sdlMacSetIgnoresMouseEvents(SDL_Window* window, bool ignore);
#endif

namespace {

class ShapeClickThrough final : public SdlClickThrough {
public:
    explicit ShapeClickThrough(SDL_Window* window) : window_(window) {}
    ~ShapeClickThrough() override { setEnabled(false); }

    bool wantsRegion() const override { return true; }

    void setEnabled(bool on) override {
        if (on == enabled_) return;
        enabled_ = on;
        if (on) {
            push();
#if defined(__APPLE__)
            monitor_ = sdlMacWatchPointer(&ShapeClickThrough::pointerMoved, this);
#endif
        } else {
#if defined(__APPLE__)
            if (monitor_) sdlMacUnwatchPointer(monitor_);
            monitor_ = nullptr;
#endif
            SDL_SetWindowShape(window_, nullptr);  // the whole window again
        }
    }

    void setRegion(const std::vector<WindowRect>& rects) override {
        rects_ = rects;
        if (enabled_) push();
    }

private:
    void push() {
        int w = 0, h = 0;
        SDL_GetWindowSize(window_, &w, &h);
        if (w <= 0 || h <= 0) return;
        SDL_Surface* mask = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
        if (!mask) return;
        SDL_FillSurfaceRect(mask, nullptr, 0);
        const Uint32 solid = SDL_MapSurfaceRGBA(mask, 255, 255, 255, 255);
        for (const WindowRect& r : rects_) {
            const SDL_Rect sr{r.x, r.y, r.w, r.h};
            SDL_FillSurfaceRect(mask, &sr, solid);
        }
        if (!SDL_SetWindowShape(window_, mask))
            LOG_WARN("click-through: SDL_SetWindowShape failed: %s", SDL_GetError());
        SDL_DestroySurface(mask);
    }

#if defined(__APPLE__)
    // Main thread, from the global monitor: the pointer moved over another
    // application. Back over the region, the window takes input again.
    static void pointerMoved(void* ctx) {
        auto* self = static_cast<ShapeClickThrough*>(ctx);
        float gx = 0, gy = 0;
        SDL_GetGlobalMouseState(&gx, &gy);
        int wx = 0, wy = 0;
        SDL_GetWindowPosition(self->window_, &wx, &wy);
        const float x = gx - wx, y = gy - wy;
        for (const WindowRect& r : self->rects_) {
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                sdlMacSetIgnoresMouseEvents(self->window_, false);
                return;
            }
        }
    }
    void* monitor_ = nullptr;
#endif

    SDL_Window* window_;
    bool enabled_ = false;
    std::vector<WindowRect> rects_;
};

}  // namespace

std::unique_ptr<SdlClickThrough> SdlClickThrough::create(SDL_Window* window) {
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || (std::strcmp(driver, "x11") != 0 && std::strcmp(driver, "cocoa") != 0)) return nullptr;
    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_TRANSPARENT)) {
        LOG_WARN("click-through: the %s window shape needs a transparent window (bro.json \"transparent\": true)",
                 driver);
        return nullptr;
    }
    return std::make_unique<ShapeClickThrough>(window);
}

#endif

} // namespace bro::platform
