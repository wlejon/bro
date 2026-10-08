// DRM input routing: every libinput event decides between the shell document
// (the bro app) and the Wayland client windows the compositor hosts. bro
// supplies the mechanism and the shell supplies the policy:
//
//   keyboard  Global chords the shell registered (bro.window.registerGlobalHotkey)
//             are matched first; a matched chord's key reaches nobody else
//             and its callback runs before the next event. Otherwise keys go
//             to the focused client window unless the shell claims the
//             keyboard (Engine::shellClaimsKeyboard: data-shell-keyboard or a
//             focused editable), no client is focused, or a hotkey grab is
//             active. A key's repeats and release follow its press; a release
//             also reaches the shell while it holds the keyboard, so a
//             switcher sees the modifier release that ends its chord.
//
//   pointer   Points the shell claims (Engine::shellClaimsPointerAt: z-index
//             >= 1000) go to the document; elsewhere the window manager's
//             interaction policy (bro.compositor.setInteraction: title band,
//             resize border, drag modifiers) decides whether a press moves or
//             resizes the window under it before the client gets it.
#include "engine/engine.h"
#include "engine/engine_drm.h"
#include "engine/key_mapping.h"
#include "platform/desktop_hotkeys.h"
#include "util/platform.h"
#include "util/time.h"

#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
#include "platform/drm_input.h"
#include <SDL3/SDL_keycode.h>
#include <linux/input-event-codes.h>
#endif
#if BRO_WITH_COMPOSITOR
#include "compositor/wayland_compositor.h"
#endif

#include "dom/document.h"
#include "dom/element.h"

namespace bro::engine {

double Engine::activityClockMs() const {
    return displayMode_ == DisplayMode::Headless ? virtualTime_ : util::currentTimeMs();
}

bool Engine::hostIdleInhibited() const {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF && BRO_WITH_COMPOSITOR && BRO_HAVE_WAYLAND_SERVER
    if (drmCtx_ && drmCtx_->compositor && drmCtx_->compositor->backend())
        return drmCtx_->compositor->backend()->idle_inhibited();
#endif
    return false;
}

void Engine::blurShellFocus() {
    if (!document_) return;
    dom::Element* active = document_->activeElement();
    if (!active || active == document_->body() || active == document_->documentElement()) return;
    dispatchFocusEvents(active, nullptr);
    document_->setActiveElement(nullptr);
}

bool Engine::shellOwnsDrmPointerAt(float x, float y) {
    // A plain app run under DRM is the whole screen.
    if (!isShellApp()) return true;
    return shellClaimsPointerAt(x, y);
}

#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF

namespace {

using EvType = platform::DrmInputEvent::Type;

#if BRO_WITH_COMPOSITOR && BRO_HAVE_WAYLAND_SERVER
uint32_t waylandButton(const platform::DrmInputEvent& ev) {
    if (ev.rawButton) return ev.rawButton;
    if (ev.button == 3) return BTN_RIGHT;
    if (ev.button == 2) return BTN_MIDDLE;
    return BTN_LEFT;
}

uint32_t wmModifiers(int32_t sdlMods) {
    uint32_t m = 0;
    if (sdlMods & SDL_KMOD_SHIFT) m |= brocompositor::modifier::Shift;
    if (sdlMods & SDL_KMOD_CTRL) m |= brocompositor::modifier::Ctrl;
    if (sdlMods & SDL_KMOD_ALT) m |= brocompositor::modifier::Alt;
    if (sdlMods & SDL_KMOD_GUI) m |= brocompositor::modifier::Super;
    return m;
}

brocompositor::PressButton pressButton(uint32_t wlButton) {
    switch (wlButton) {
        case BTN_LEFT: return brocompositor::PressButton::Left;
        case BTN_RIGHT: return brocompositor::PressButton::Right;
        case BTN_MIDDLE: return brocompositor::PressButton::Middle;
        default: return brocompositor::PressButton::Other;
    }
}
#endif

}  // namespace

void Engine::dispatchDrmInput(const platform::DrmInputEvent& ev) {
    noteUserActivity();
    const bool pointer = ev.type == EvType::MouseMove || ev.type == EvType::MouseDown ||
                         ev.type == EvType::MouseUp;
    if (pointer) {
        cursorVisible_ = true;
        uiDirty_ = true;
        lastMouseX_ = ev.x;
        lastMouseY_ = ev.y;
    }
    if (ev.type == EvType::MouseDown || ev.type == EvType::MouseWheel ||
        ev.type == EvType::TouchDown)
        platform::desktop::cancelHotkeyTap();

    if (ev.type == EvType::KeyDown || ev.type == EvType::KeyUp) {
        if (routeDrmKey(ev)) return;
    } else if (routeDrmPointer(ev)) {
        return;
    }
    deliverDrmInputToShell(ev);
}

// True when the event was consumed by a hotkey or went to a client only.
bool Engine::routeDrmKey(const platform::DrmInputEvent& ev) {
    const bool down = ev.type == EvType::KeyDown;
    auto hk = platform::desktop::hotkeyKeyFromSdl(ev.keycode, ev.scancode, ev.modifiers, down, ev.repeat);
    auto routed = platform::desktop::routeHotkeyKey(hk);
    if (routed.consumed) return true;

    if (!drmCtx_) return false;
    const uint32_t code = hk.code;
    bool clientFocused = false;
#if BRO_WITH_COMPOSITOR && BRO_HAVE_WAYLAND_SERVER
    auto* comp = drmCtx_->compositor.get();
    clientFocused = comp && comp->isRunning() && comp->focusedWindow() != 0;
#endif
    const bool shellOwns = !isShellApp() || routed.grabbed || !clientFocused || shellClaimsKeyboard();

    bool toClient = false, toShell = false;
    if (down && !ev.repeat) {
        (shellOwns ? drmCtx_->keysToShell : drmCtx_->keysToClient).insert(code);
        (shellOwns ? toShell : toClient) = true;
    } else if (down) {
        toClient = drmCtx_->keysToClient.count(code) != 0;
        toShell = drmCtx_->keysToShell.count(code) != 0;
    } else {
        toClient = drmCtx_->keysToClient.erase(code) != 0;
        toShell = drmCtx_->keysToShell.erase(code) != 0 || shellOwns;
    }
    if (!toClient && !toShell) (shellOwns ? toShell : toClient) = true;

#if BRO_WITH_COMPOSITOR && BRO_HAVE_WAYLAND_SERVER
    if (toClient && comp && comp->isRunning()) {
        uint32_t k = ev.rawKeycode ? ev.rawKeycode : static_cast<uint32_t>(ev.scancode);
        comp->injectKey(k, down);
    }
#else
    toShell = true;
#endif
    return !toShell;
}

// True when a client window took the event and the shell must not see it.
bool Engine::routeDrmPointer(const platform::DrmInputEvent& ev) {
#if BRO_WITH_COMPOSITOR && BRO_HAVE_WAYLAND_SERVER
    if (!drmCtx_ || !drmCtx_->compositor || !drmCtx_->compositor->isRunning()) return false;
    auto* comp = drmCtx_->compositor.get();
    const double x = ev.x, y = ev.y;

    switch (ev.type) {
        case EvType::MouseMove: {
            if (comp->isDraggingWindow()) {
                bool wasActive = comp->isDragActive();
                bool moved = comp->updateInteractiveDrag(x, y);
                // A title-bar press the client got: release it once it is a drag.
                if (!wasActive && comp->isDragActive()) comp->injectPointerButton(BTN_LEFT, false);
                if (moved) uiDirty_ = true;
                return true;
            }
            if (shellOwnsDrmPointerAt(ev.x, ev.y)) {
                comp->routePointer(-1.0, -1.0);
            } else {
                comp->injectPointerWarp(x, y);
                comp->routePointer(x, y);
            }
            return false;  // the document still tracks the pointer (hover leaves)
        }
        case EvType::MouseDown: {
            const uint32_t wlButton = waylandButton(ev);
            if (shellOwnsDrmPointerAt(ev.x, ev.y)) {
                if (comp->focusedWindow() != 0) comp->focusWindow(0);
                return false;
            }
            uint64_t hitWin = comp->windowAt(x, y);
            if (hitWin != 0) {
                auto d = comp->classifyPress(hitWin, x, y, wmModifiers(ev.modifiers), pressButton(wlButton));
                if (d.action != brocompositor::PressAction::None) {
                    blurShellFocus();
                    if (d.action == brocompositor::PressAction::Move)
                        comp->startInteractiveMove(hitWin, x, y, d.immediate);
                    else
                        comp->startInteractiveResize(hitWin, x, y, d.edges, d.immediate);
                    if (d.forward) {
                        comp->routePointer(x, y);
                        comp->injectPointerButton(wlButton, true);
                    }
                    return true;
                }
            }
            if (comp->routePointer(x, y)) {
                blurShellFocus();
                if (hitWin != 0 && hitWin != comp->focusedWindow()) comp->focusWindow(hitWin);
                comp->injectPointerButton(wlButton, true);
                return true;
            }
            return false;
        }
        case EvType::MouseUp: {
            const uint32_t wlButton = waylandButton(ev);
            if (comp->isDraggingWindow()) {
                bool wasActive = comp->isDragActive();
                comp->endInteractiveDrag();
                // A forwarded press that never became a drag: the client gets its release.
                if (!wasActive && comp->focusedWindow() != 0) comp->injectPointerButton(wlButton, false);
                uiDirty_ = true;
                return true;
            }
            if (!shellOwnsDrmPointerAt(ev.x, ev.y) && comp->focusedWindow() != 0) {
                comp->injectPointerButton(wlButton, false);
                return true;
            }
            return false;
        }
        case EvType::MouseWheel: {
            if (!shellOwnsDrmPointerAt(ev.x, ev.y) && comp->focusedWindow() != 0) {
                comp->injectPointerAxis(0, static_cast<double>(ev.wheelDy), 0);
                return true;
            }
            return false;
        }
        default:
            return false;
    }
#else
    (void)ev;
    return false;
#endif
}

void Engine::deliverDrmInputToShell(const platform::DrmInputEvent& ev) {
    switch (ev.type) {
        case EvType::KeyDown: {
            handleKeyDown(ev.keycode, ev.scancode, ev.modifiers, ev.repeat);
            if (!util::hasPrimaryMod(ev.modifiers)) {
                std::string webKey = sdlKeycodeToWebKey(ev.keycode, ev.modifiers);
                if (webKey.size() == 1) handleTextInput(webKey);
            }
            break;
        }
        case EvType::KeyUp:
            handleKeyUp(ev.keycode, ev.scancode, ev.modifiers, ev.repeat);
            break;
        case EvType::MouseMove:
            handleMouseMove(ev.x, ev.y, ev.dx, ev.dy);
            break;
        case EvType::MouseDown:
            handleMouseDown(ev.x, ev.y, ev.button);
            break;
        case EvType::MouseUp:
            handleMouseUp(ev.x, ev.y, ev.button);
            break;
        case EvType::MouseWheel:
            handleWheel(ev.x, ev.y, ev.wheelDx, ev.wheelDy);
            break;
        case EvType::TouchDown:
            handleTouchDown(ev.touchId, ev.x, ev.y, ev.touchPressure);
            break;
        case EvType::TouchMove:
            handleTouchMove(ev.touchId, ev.x, ev.y, ev.touchPressure);
            break;
        case EvType::TouchUp:
            handleTouchUp(ev.touchId, ev.x, ev.y);
            break;
        default:
            break;
    }
}

#else

void Engine::dispatchDrmInput(const platform::DrmInputEvent&) {}
bool Engine::routeDrmKey(const platform::DrmInputEvent&) { return false; }
bool Engine::routeDrmPointer(const platform::DrmInputEvent&) { return false; }
void Engine::deliverDrmInputToShell(const platform::DrmInputEvent&) {}

#endif

}  // namespace bro::engine
