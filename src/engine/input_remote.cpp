// <remoteview> input routing: a remote screen's view takes the keyboard and
// the pointer the way a remote-desktop window does (remote_view_host.h).
//
// Keys: while a view is captured and focused, every key goes to the
// RemoteViewHost before overlays, hotkeys, the page or text input see it,
// with the window's keyboard grab on so the system's shortcuts arrive as
// keys too. The host keeps a release chord for itself (it un-captures the
// view). A press on a view captures it; blur, or another element taking
// focus, releases it.
//
// Pointer: over a view (or while a button pressed on one is held) motion,
// buttons and the wheel go to the host as well as to the page (which may
// show its own controls on hover); the wheel goes only to the host. While
// the remote screen's pointer is locked, the window's pointer is held in
// relative mode and only its movement goes.

#include "engine/engine.h"
#include "engine/remote_view_host.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "layout/el_remote_view.h"
#include "platform/window.h"

namespace bro::engine {

namespace {

layout::ElRemoteView* viewOf(dom::Element* el) { return el ? el->remoteViewControl() : nullptr; }

}  // namespace

layout::ElRemoteView* Engine::capturedRemoteView() {
    if (!remoteViewHost_ || !document_) return nullptr;
    layout::ElRemoteView* v = viewOf(document_->activeElement());
    return v && v->captured() ? v : nullptr;
}

bool Engine::remoteKey(int scancode, int mod, bool pressed, bool repeat) {
    layout::ElRemoteView* v = capturedRemoteView();
    if (!v) return false;
    remoteViewHost_->key(*v, scancode, mod, pressed, repeat);
    return true;
}

void Engine::remotePointer(float x, float y, float xrel, float yrel, int button, bool pressed) {
    if (!remoteViewHost_ || !document_) return;
    layout::ElRemoteView* v = nullptr;
    if (remoteLockedView_) {
        v = layout::ElRemoteView::byId(remoteLockedView_);
    } else if (remotePointerGrab_) {
        v = layout::ElRemoteView::byId(remotePointerGrab_);
    } else {
        const float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
        v = viewOf(hitTest(docX, docY));
    }
    if (!v || !v->element()) return;
    if (button != 0) {
        const uint32_t bit = 1u << (button & 31);
        if (pressed) {
            remotePointerButtons_ |= bit;
            remotePointerGrab_ = v->viewId();
            v->setCaptured(true);
        } else {
            remotePointerButtons_ &= ~bit;
            if (!remotePointerButtons_) remotePointerGrab_ = 0;
        }
    }
    const auto box = dom::absoluteContentBox(v->element());
    const float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
    remoteViewHost_->pointer(*v, docX - box.x, docY - box.y, box.width, box.height, xrel, yrel, button, pressed);
}

bool Engine::remoteWheel(float x, float y, float dx, float dy) {
    if (!remoteViewHost_ || !document_) return false;
    layout::ElRemoteView* v = nullptr;
    if (remoteLockedView_) v = layout::ElRemoteView::byId(remoteLockedView_);
    else if (remotePointerGrab_) v = layout::ElRemoteView::byId(remotePointerGrab_);
    else v = viewOf(hitTest(x, y - static_cast<float>(contentTop()) + scrollY_));
    if (!v) return false;
    remotePointer(x, y, 0.0f, 0.0f, 0, false);  // the wheel turns where the pointer is
    remoteViewHost_->wheel(*v, dx, dy);
    return true;
}

void Engine::pumpRemoteViews() {
    if (!remoteViewHost_) return;
    layout::ElRemoteView* active = document_ ? viewOf(document_->activeElement()) : nullptr;

    // A captured view that lost focus is released (its keys and buttons let
    // go); one that kept focus while the window lost it lets go of what was
    // held but stays captured for when the window comes back.
    layout::ElRemoteView::forEach([&](layout::ElRemoteView& v) {
        if (v.captured() && &v != active) {
            v.setCaptured(false);
            remoteViewHost_->releaseAll(v);
        }
    });
    layout::ElRemoteView* captured = active && active->captured() ? active : nullptr;
    const bool hasKeyboard = captured && windowFocused_;
    const uint64_t keyboardView = hasKeyboard ? captured->viewId() : 0;
    if (keyboardView != remoteKeyboardView_) {
        if (layout::ElRemoteView* was = layout::ElRemoteView::byId(remoteKeyboardView_))
            if (!hasKeyboard || was != captured) remoteViewHost_->releaseAll(*was);
        remoteKeyboardView_ = keyboardView;
        if (window_) window_->setKeyboardGrab(keyboardView != 0);
    }
    if (!layout::ElRemoteView::byId(remotePointerGrab_)) {
        remotePointerGrab_ = 0;
        remotePointerButtons_ = 0;
    }

    // Relative mode while the captured view's screen holds its pointer locked.
    const uint64_t locked = hasKeyboard && remoteViewHost_->pointerLocked(*captured) ? captured->viewId() : 0;
    if (locked != remoteLockedView_) {
        remoteLockedView_ = locked;
        if (window_ && !lockedElement_.get()) window_->cursor().setRelativeMode(locked != 0);
    }
}

void Engine::refreshHoverCursor() { updateCursorFromHover(hoveredElement_.get()); }

}  // namespace bro::engine
