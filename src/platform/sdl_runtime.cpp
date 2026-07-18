#include "platform/sdl_runtime.h"
#include "util/log.h"

#include <SDL3/SDL.h>

namespace bro::platform {

// Main-thread only — see header.
static int s_refCount = 0;

bool SdlRuntime::acquire() {
    if (s_refCount == 0) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            LOG_ERROR("Failed to initialize SDL: %s", SDL_GetError());
            return false;
        }

        // Gamepad support is best-effort: a headless box or stripped-down
        // driver stack may have no controller backend, and apps must still run
        // (they just see zero gamepads). So init it as a subsystem and only
        // log on failure.
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            LOG_INFO("SDL gamepad subsystem unavailable: %s", SDL_GetError());
        }

        // Deliver the click that activates an unfocused window. SDL defaults
        // this off on every platform (Windows/X11/Wayland all gate on the same
        // hint), so without it the first click after alt-tabbing back in is
        // swallowed to raise the window and the user has to click a second
        // time to actually hit anything. Browsers and native apps pass that
        // click through; so do we.
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    }
    ++s_refCount;
    return true;
}

void SdlRuntime::release() {
    if (s_refCount <= 0) {
        LOG_ERROR("SdlRuntime::release() without matching acquire()");
        return;
    }
    if (--s_refCount == 0) {
        SDL_Quit();
    }
}

} // namespace bro::platform
