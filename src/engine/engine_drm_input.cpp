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
//             >= 1000) go to the document. Elsewhere the window stack is
//             walked top down (drmPointerOnClient): a client surface takes
//             the pointer, unless the shell's frame for a window above it
//             (window_frames.h) is hit first; under every window, the
//             document. On a client the window manager's interaction policy
//             (bro.compositor.setInteraction: title band, resize border, drag
//             modifiers) decides whether a press moves or resizes the window
//             before the client gets it. A press raises and focuses its
//             window (frame or client); its moves and release follow it, and
//             a drag (the policy's, a client's xdg move, or the shell's
//             bro.compositor.beginMove) takes the moves until the release.
#include "engine/engine.h"
#include "engine/engine_drm.h"
#include "engine/key_mapping.h"
#include "platform/desktop_hotkeys.h"
#include "platform/keys.h"
#include "util/time.h"

#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
#include "platform/drm_input.h"
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

uint32_t wmModifiers(int32_t mods) {
    uint32_t m = 0;
    if (mods & platform::kmod::Shift) m |= brocompositor::modifier::Shift;
    if (mods & platform::kmod::Ctrl) m |= brocompositor::modifier::Ctrl;
    if (mods & platform::kmod::Alt) m |= brocompositor::modifier::Alt;
    if (mods & platform::kmod::Gui) m |= brocompositor::modifier::Super;
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
    auto hk = platform::desktop::hotkeyKeyFromKeyEvent(ev.keycode, ev.scancode, ev.modifiers, down, ev.repeat);
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

// Whether the pointer at (x, y) is a client's: walking the stack top down,
// the first window whose surface is under it takes it, unless the shell's
// frame for a window above is (the frame's own hit test: a transparent shadow
// with pointer-events: none lets it through). A frame's data-window-overlay
// parts are over its own client, so they are hit before it. The shell's overlays (z-index
// >= 1000) are above every window, its desktop below them all.
bool Engine::drmPointerOnClient(float x, float y, uint64_t* frameWindow) {
    if (frameWindow) *frameWindow = 0;
#if BRO_WITH_COMPOSITOR && BRO_HAVE_WAYLAND_SERVER
    if (!drmCtx_ || !drmCtx_->compositor || !drmCtx_->compositor->isRunning()) return false;
    if (shellOwnsDrmPointerAt(x, y)) return false;
    auto* comp = drmCtx_->compositor.get();
    if (comp->isSessionLocked() || comp->unmanagedAt(x, y)) return true;
    const auto& stack = drmCtx_->frames.stack();
    dom::Element* hit = nullptr;
    bool hitTested = false;
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        dom::Element* frame = drmCtx_->frames.frameOf(it->id);
        if (frame && !hitTested) {
            hit = hitTest(x, y);
            hitTested = true;
        }
        // The frame's overlays are drawn over its client: theirs first.
        if (frame && drmCtx_->frames.overlayHit(it->id, hit)) {
            if (frameWindow) *frameWindow = it->id;
            return false;
        }
        if (comp->windowSurfaceAt(it->id, x, y)) return true;
        if (!frame) continue;
        for (dom::Element* cur = hit; cur; cur = cur->parentElement()) {
            if (cur != frame) continue;
            if (frameWindow) *frameWindow = it->id;
            return false;
        }
    }
    return false;
#else
    (void)x;
    (void)y;
    return false;
#endif
}

// True when a client window took the event and the shell must not see it.
// A press goes where the pointer is; its moves and its release follow it.
bool Engine::routeDrmPointer(const platform::DrmInputEvent& ev) {
#if BRO_WITH_COMPOSITOR && BRO_HAVE_WAYLAND_SERVER
    if (!drmCtx_ || !drmCtx_->compositor || !drmCtx_->compositor->isRunning()) return false;
    auto* comp = drmCtx_->compositor.get();
    auto& ctx = *drmCtx_;
    const double x = ev.x, y = ev.y;

    switch (ev.type) {
        case EvType::MouseMove: {
            if (comp->isDraggingWindow()) {
                bool wasActive = comp->isDragActive();
                bool moved = comp->updateInteractiveDrag(x, y);
                // A title-bar press the client got: release it once it is a drag.
                if (!wasActive && comp->isDragActive() && ctx.pressToClient) {
                    comp->injectPointerButton(BTN_LEFT, false);
                    ctx.pressToClient = false;
                }
                if (moved) {
                    uiDirty_ = true;
                    syncShellWindowFrames();
                }
                return true;
            }
            bool onClient = ctx.pressToClient ||
                            (!ctx.pressToShell && drmPointerOnClient(ev.x, ev.y, nullptr));
            if (!onClient) {
                ctx.pointerOnClient = false;
                comp->routePointer(-1.0, -1.0);
                return false;
            }
            comp->injectPointerWarp(x, y);
            comp->routePointer(x, y);
            if (!ctx.pointerOnClient) {
                // Onto a client: the shell's hover leaves whatever was under it.
                ctx.pointerOnClient = true;
                handleMouseMove(-1.0f, -1.0f, 0.0f, 0.0f);
                lastMouseX_ = ev.x;
                lastMouseY_ = ev.y;
            }
            return true;
        }
        case EvType::MouseDown: {
            const uint32_t wlButton = waylandButton(ev);
            uint64_t frameWin = 0;
            if (!drmPointerOnClient(ev.x, ev.y, &frameWin)) {
                ctx.pressToShell = true;
                ctx.pointerOnClient = false;
                // A press on a window's frame raises and focuses it; on the
                // shell's own surfaces, no client keeps the focus.
                if (frameWin != 0) {
                    if (frameWin != comp->focusedWindow()) comp->focusWindow(frameWin);
                    syncShellWindowFrames();
                } else if (comp->focusedWindow() != 0) {
                    comp->focusWindow(0);
                }
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
                        ctx.pressToClient = true;
                    }
                    syncShellWindowFrames();
                    return true;
                }
            }
            if (comp->routePointer(x, y)) {
                blurShellFocus();
                if (hitWin != 0 && hitWin != comp->focusedWindow()) comp->focusWindow(hitWin);
                comp->injectPointerButton(wlButton, true);
                ctx.pressToClient = true;
                syncShellWindowFrames();
                return true;
            }
            ctx.pressToShell = true;
            return false;
        }
        case EvType::MouseUp: {
            const uint32_t wlButton = waylandButton(ev);
            const bool toShell = ctx.pressToShell, toClient = ctx.pressToClient;
            ctx.pressToShell = ctx.pressToClient = false;
            if (comp->isDraggingWindow()) {
                bool wasActive = comp->isDragActive();
                comp->endInteractiveDrag();
                // A forwarded press that never became a drag: the client gets its release.
                if (!wasActive && toClient) comp->injectPointerButton(wlButton, false);
                uiDirty_ = true;
                syncShellWindowFrames();
                // A drag the shell began (its frame's title bar): its press
                // gets its release, so the shell's own handlers finish.
                return !toShell;
            }
            if (toClient) {
                comp->injectPointerButton(wlButton, false);
                return true;
            }
            if (toShell) return false;
            if (drmPointerOnClient(ev.x, ev.y, nullptr) && comp->focusedWindow() != 0) {
                comp->injectPointerButton(wlButton, false);
                return true;
            }
            return false;
        }
        case EvType::MouseWheel: {
            if (drmPointerOnClient(ev.x, ev.y, nullptr) && comp->focusedWindow() != 0) {
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
            if (!platform::hasPrimaryMod(ev.modifiers)) {
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
bool Engine::drmPointerOnClient(float, float, uint64_t* frameWindow) {
    if (frameWindow) *frameWindow = 0;
    return false;
}
bool Engine::routeDrmKey(const platform::DrmInputEvent&) { return false; }
bool Engine::routeDrmPointer(const platform::DrmInputEvent&) { return false; }
void Engine::deliverDrmInputToShell(const platform::DrmInputEvent&) {}

#endif

}  // namespace bro::engine
