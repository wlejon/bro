// Input from a device bro's platform layer does not own — a remote viewer
// (bro.remote) — given as such a device reports it (evdev codes, frame
// pixels), and routed as local input is. Under DRM it becomes the libinput
// path's own DrmInputEvent, built by the seat's DrmInputPlatform so the
// modifiers and the pointer are the seat's, and goes through
// dispatchDrmInput: global hotkeys, the shell or a client window. Elsewhere
// (windowed, headless) it goes to the handle* entry points, with a key's
// text derived as the DRM path derives it.
#include "engine/engine.h"
#include "engine/engine_drm.h"
#include "engine/key_mapping.h"
#include "platform/evdev_keymap.h"
#include "util/platform.h"

#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
#include "platform/drm_input.h"
#endif
#if BRO_WITH_COMPOSITOR && BRO_HAVE_WAYLAND_SERVER
#include "compositor/wayland_compositor.h"
#endif

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

namespace bro::engine {

namespace {

// Frame pixels to the CSS px input is handled in.
float toCss(float framePx, int cssExtent, int frameExtent) {
    if (cssExtent <= 0 || frameExtent <= 0) return framePx;
    return framePx * static_cast<float>(cssExtent) / static_cast<float>(frameExtent);
}

}  // namespace

void Engine::injectDeviceInput(const DeviceInput& in) {
    using Kind = DeviceInput::Kind;
    const float cssX = toCss(in.x, viewportWidth_, framePixelWidth());
    const float cssY = toCss(in.y, viewportHeight_, framePixelHeight());

#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    if (displayMode_ == DisplayMode::Drm && drmCtx_ && drmCtx_->input) {
        // DrmInputPlatform's screen is the viewport (engine_init_drm.cpp).
        platform::DrmInputPlatform& seat = *drmCtx_->input;
        switch (in.kind) {
            case Kind::Key: dispatchDrmInput(seat.keyEvent(in.code, in.pressed)); break;
            case Kind::Motion: dispatchDrmInput(seat.pointerMotionAbsolute(cssX, cssY)); break;
            case Kind::Button: dispatchDrmInput(seat.buttonEvent(in.code, in.pressed)); break;
            case Kind::Wheel:
                // libinput reports a wheel in degrees, 15 to a detent.
                dispatchDrmInput(seat.wheelEvent(static_cast<float>(in.wheelX) * 15.0f / 120.0f,
                                                 static_cast<float>(in.wheelY) * 15.0f / 120.0f));
                break;
        }
        return;
    }
#endif

    switch (in.kind) {
        case Kind::Key: {
            const auto scancode = static_cast<SDL_Scancode>(platform::evdevKeyToSdlScancode(in.code));
            if (scancode == SDL_SCANCODE_UNKNOWN) return;
            const SDL_Keycode keycode = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
            auto track = [&](SDL_Keymod bit) {
                if (in.pressed) deviceInputMods_ |= bit;
                else deviceInputMods_ &= ~bit;
            };
            if (scancode == SDL_SCANCODE_LSHIFT || scancode == SDL_SCANCODE_RSHIFT) track(SDL_KMOD_SHIFT);
            else if (scancode == SDL_SCANCODE_LCTRL || scancode == SDL_SCANCODE_RCTRL) track(SDL_KMOD_CTRL);
            else if (scancode == SDL_SCANCODE_LALT || scancode == SDL_SCANCODE_RALT) track(SDL_KMOD_ALT);
            else if (scancode == SDL_SCANCODE_LGUI || scancode == SDL_SCANCODE_RGUI) track(SDL_KMOD_GUI);
            const int code = static_cast<int>(keycode), sc = static_cast<int>(scancode);
            if (!in.pressed) {
                handleKeyUp(code, sc, deviceInputMods_, false);
                break;
            }
            handleKeyDown(code, sc, deviceInputMods_, false);
            if (!util::hasPrimaryMod(deviceInputMods_)) {
                const std::string webKey = sdlKeycodeToWebKey(code, deviceInputMods_);
                if (webKey.size() == 1) handleTextInput(webKey);
            }
            break;
        }
        case Kind::Motion:
            handleMouseMove(cssX, cssY, cssX - lastMouseX_, cssY - lastMouseY_);
            break;
        case Kind::Button: {
            const int button = platform::evdevButtonToMouseButton(in.code);
            if (in.pressed) handleMouseDown(lastMouseX_, lastMouseY_, button);
            else handleMouseUp(lastMouseX_, lastMouseY_, button);
            break;
        }
        case Kind::Wheel:
            // SDL's sense, which handleWheel takes: detents, +y away from the user.
            handleWheel(lastMouseX_, lastMouseY_, static_cast<float>(in.wheelX) / 120.0f,
                        -static_cast<float>(in.wheelY) / 120.0f);
            break;
    }
}

std::string Engine::screenCursorShape() const {
    std::string shape = resolvedCursor_;
#if BRO_WITH_COMPOSITOR && BRO_HAVE_WAYLAND_SERVER
    // The client's cursor while the pointer is on a client (or a window is
    // being dragged); over the shell, the shell's.
    if (displayMode_ == DisplayMode::Drm && drmCtx_ && drmCtx_->compositor &&
        (drmCtx_->pointerOnClient || drmCtx_->compositor->isDragActive() || !isShellApp())) {
        auto c = drmCtx_->compositor->cursor();
        if (c.hidden) shape = "none";
        else if (!c.shape.empty()) shape = c.shape;
    }
#endif
    return shape;
}

}  // namespace bro::engine
