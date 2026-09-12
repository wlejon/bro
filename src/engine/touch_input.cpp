// Engine touch input — W3C Pointer Events + Touch Events from SDL finger
// events (or the headless touchDown/touchMove/touchUp/touchCancel seam).
// These are Engine member function implementations, not a separate class.
//
// Model (see also docs/pointer-api.js):
//   * One TouchContact per finger on the surface. pointerIds are minted
//     monotonically starting at 2 — unique per contact, never colliding with
//     the mouse pointer's fixed id 1. The first contact of a contact set
//     (a touch landing on an empty table) is the primary pointer.
//   * Per contact transition the engine dispatches the pointer event first,
//     then the touch event (spec order): pointerdown → touchstart,
//     pointermove → touchmove, pointerup → touchend, pointercancel →
//     touchcancel.
//   * Pointer events hit-test the contact point per event, unless that
//     pointerId is captured (Element.setPointerCapture(pointerId)), in which
//     case they route to the captured element with offsets recomputed.
//     Touch events instead always fire at the contact's touchstart target
//     (the W3C Touch Events targeting rule).
//   * Compat mouse: a primary-contact TAP — down and up without travelling
//     past the slop radius — synthesizes mousedown → mouseup → click through
//     the standard document dispatch helpers after touchend, so mouse-only
//     apps work under touch (including control focus). preventDefault() on
//     pointerdown, touchstart, or touchend suppresses the compat sequence.
//     Touch movement does NOT synthesize mousemove.
//   * Touch never drives hover: hoveredElement_ / :hover / mouseover-out-
//     enter-leave stay mouse-only.
//   * Touch input targets the app document only (system panels, overlays,
//     and engine chrome are mouse-driven).

#include "engine/engine.h"
#include "engine/overlay.h"
#include "engine/replaced_elements.h"

#include "dom/event_dispatch.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "util/time.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::engine {

// A contact that travels farther than this from its down point is a drag,
// not a tap, and gets no compat mouse sequence (matches typical browser /
// OS touch slop).
static constexpr float kTapSlopPx = 10.0f;

Engine::TouchContact* Engine::touchByFinger(uint64_t fingerId) {
    for (auto& c : touchContacts_) {
        if (c.fingerId == fingerId) return &c;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Pointer events for touch contacts
// ---------------------------------------------------------------------------

bool Engine::dispatchTouchPointerEvent(const char* type, const TouchContact& c,
                                       bool cancelable) {
    if (!document_) return false;

    const bool isDown   = std::strcmp(type, "pointerdown") == 0;
    const bool isMove   = std::strcmp(type, "pointermove") == 0;
    const bool isCancel = std::strcmp(type, "pointercancel") == 0;
    const bool ends     = std::strcmp(type, "pointerup") == 0 || isCancel;

    // Capture routes everything but pointerdown (capture is taken during it,
    // not before). Without capture: hit-test the contact point, except
    // pointercancel — the gesture was aborted, there is nothing meaningful
    // under the point, so it goes to the contact's start target.
    dom::Element* captured = isDown ? nullptr : pointerCaptureFor(c.pointerId);
    dom::Element* target = captured;
    if (!target) {
        if (isCancel) {
            target = c.startTarget.get();
        } else {
            float docX = c.x;
            float docY = c.y - static_cast<float>(contentTop()) + scrollY_;
            target = hitTest(docX, docY);
        }
    }

    bool prevented = false;
    if (target) {
        float ct = static_cast<float>(contentTop());
        float clientY = c.y - ct;
        dom::MouseEvent pe(type, /*bubbles=*/true, cancelable);
        pe.setIsTrusted(true);
        pe.setClientX(static_cast<double>(c.x));
        pe.setClientY(static_cast<double>(clientY));
        pe.setScreenX(static_cast<double>(c.x));
        pe.setScreenY(static_cast<double>(c.y));
        pe.setPageX(static_cast<double>(c.x));
        pe.setPageY(static_cast<double>(clientY + scrollY_));
        // Touch contact = the primary "button": button 0 on the down/up
        // transitions, -1 (no button change) on moves; buttons bit 1 while
        // the contact is on the surface.
        pe.setButton(isMove ? -1 : 0);
        pe.setButtons(ends ? 0 : 1);
        int mod = currentModState();
        pe.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
        pe.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
        pe.setAltKey((mod & SDL_KMOD_ALT) != 0);
        pe.setMetaKey((mod & SDL_KMOD_GUI) != 0);
        pe.setPointerId(c.pointerId);
        pe.setPointerType("touch");
        pe.setIsPrimaryPointer(c.primary);
        pe.setPressure(ends ? 0.0 : static_cast<double>(c.pressure));
        applyMouseOffset(pe, target);
        dom::dispatchDomEvent(target, pe);
        prevented = pe.defaultPrevented();
    }

    // Implicit release (spec): the pointerup/pointercancel that ends the
    // contact also ends its capture — even when there was no live target to
    // dispatch to (the holder may have been freed by earlier JS).
    if (ends) {
        auto it = pointerCaptures_.find(c.pointerId);
        if (it != pointerCaptures_.end()) {
            releasePointerCapture(it->second.get(), c.pointerId);
        }
    }
    return prevented;
}

// ---------------------------------------------------------------------------
// W3C Touch Events (touchstart / touchmove / touchend / touchcancel)
// ---------------------------------------------------------------------------

bool Engine::dispatchTouchEvent(const char* type, const TouchContact& changed,
                                bool cancelable) {
    if (!document_) return false;
    // Touch events fire at the contact's touchstart target for its whole
    // lifetime (W3C targeting rule) — a finger sliding off the element keeps
    // reporting to it.
    dom::Element* target = changed.startTarget.get();
    if (!target) return false;

    dom::TouchEvent evt(type, /*bubbles=*/true, cancelable);
    evt.setIsTrusted(true);

    const float ct = static_cast<float>(contentTop());
    const float scroll = scrollY_;

    auto makePoint = [&](const TouchContact& c, dom::Element* touchTarget) -> dom::TouchPoint {
        dom::TouchPoint tp;
        tp.identifier = c.pointerId;
        tp.target = touchTarget;
        tp.clientX = static_cast<double>(c.x);
        tp.clientY = static_cast<double>(c.y - ct);
        tp.pageX = static_cast<double>(c.x);
        tp.pageY = static_cast<double>(c.y - ct + scroll);
        tp.screenX = static_cast<double>(c.x);
        tp.screenY = static_cast<double>(c.y);
        tp.force = static_cast<double>(c.pressure);
        return tp;
    };

    for (const auto& c : touchContacts_) {
        dom::Element* cTarget = c.startTarget.get();
        evt.addTouch(makePoint(c, cTarget));
        if (cTarget == target) {
            evt.addTargetTouch(makePoint(c, cTarget));
        }
    }
    evt.addChangedTouch(makePoint(changed, target));

    int mod = currentModState();
    evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
    evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
    evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
    evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);

    dom::dispatchDomEvent(target, evt);
    return evt.defaultPrevented();
}

// ---------------------------------------------------------------------------
// Compat mouse events (primary-contact tap)
// ---------------------------------------------------------------------------

void Engine::dispatchCompatMouseForTap(const TouchContact& c) {
    if (!document_) return;

    const float x = c.x, y = c.y;
    const float ct = static_cast<float>(contentTop());
    const float clientY = y - ct;
    float docX = x, docY = clientY + scrollY_;
    dom::Element* target = hitTest(docX, docY);
    int mod = currentModState();
    double nowMs = util::currentTimeMs();

    ControlContext cctx{document_.get(),
                        renderer_.get(), window_.get(), &uiDirty_,
                        &overlayMgr_, OverlayContext::App,
                        contentWidth(), contentHeight()};

    auto populate = [&](dom::MouseEvent& evt, int buttons) {
        evt.setIsTrusted(true);
        evt.setClientX(static_cast<double>(x));
        evt.setClientY(static_cast<double>(clientY));
        evt.setScreenX(static_cast<double>(x));
        evt.setScreenY(static_cast<double>(y));
        evt.setPageX(static_cast<double>(x));
        evt.setPageY(static_cast<double>(clientY + scrollY_));
        evt.setButton(0);
        evt.setButtons(buttons);
        evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
        evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
        evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
        evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
        if (target) applyMouseOffset(evt, target);
    };

    // mousedown with full focus-transition semantics — a tap focuses inputs
    // exactly like a click. pressOrdinal keeps the rolling double-click
    // streak, so a quick double-tap produces dblclick like a double-click.
    dom::MouseEvent downEvt("mousedown");
    populate(downEvt, /*buttons=*/1);
    PressIntent intent;
    intent.ordinal = pressOrdinal(appMouseState_, target, x, clientY, nowMs,
                                  inputConfig_.doubleClickThresholdMs,
                                  inputConfig_.doubleClickDistancePx);
    dispatchDocMousePress(cctx, appMouseState_, target, downEvt,
                          x, clientY, intent);

    // mouseup + click / dblclick via the shared release helper.
    dom::MouseEvent upEvt("mouseup");
    populate(upEvt, /*buttons=*/0);
    dispatchDocMouseRelease(cctx, appMouseState_, target, upEvt,
                            x, clientY, /*button=*/0, /*buttons=*/0, mod,
                            /*movementX=*/0.0f, /*movementY=*/0.0f,
                            x, clientY + scrollY_,
                            nowMs,
                            inputConfig_.doubleClickThresholdMs,
                            inputConfig_.doubleClickDistancePx);
    // Focus/caret chrome lives in the cached base layer.
    markAppBaseDirty();
}

// ---------------------------------------------------------------------------
// Two-finger gestures (pinch / pan / rotate)
// ---------------------------------------------------------------------------
// SDL3 dropped SDL2's gesture subsystem, so gestures are recognized here from
// the live contact table: while 2+ fingers are down, the two OLDEST contacts
// (the founding pair) drive WebKit-style gesturestart / gesturechange /
// gestureend events with `scale` (current distance / start distance),
// `rotation` (degrees from start, clockwise positive, unwrapped past ±180)
// and the centroid's `clientX`/`clientY` carried as event properties.
// Events fire on the hit target of the start centroid for the gesture's
// whole lifetime (the Touch Events targeting idiom). Regular pointer/touch
// events keep firing untouched — apps doing their own two-finger math see
// no change.

void Engine::dispatchGestureEvent(const char* type) {
    if (!document_) return;
    dom::Element* target = gesture_.target.get();
    if (!target) return;

    dom::GestureEvent evt(type, /*bubbles=*/true, /*cancelable=*/true);
    evt.setIsTrusted(true);
    evt.setScale(static_cast<double>(gesture_.scale));
    evt.setRotation(static_cast<double>(gesture_.rotation));
    evt.setClientX(static_cast<double>(gesture_.cx));
    evt.setClientY(static_cast<double>(gesture_.cy - static_cast<float>(contentTop())));
    dom::dispatchDomEvent(target, evt);
}

void Engine::gestureMaybeStart() {
    if (gesture_.active || touchContacts_.size() < 2) return;
    if (!document_) return;

    // Founding pair = the two oldest contacts (table is push_back order).
    const TouchContact& a = touchContacts_[0];
    const TouchContact& b = touchContacts_[1];
    const float dx = b.x - a.x, dy = b.y - a.y;
    float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 1.0f) dist = 1.0f;   // degenerate: coincident fingers

    gesture_.active = true;
    gesture_.fingerA = a.fingerId;
    gesture_.fingerB = b.fingerId;
    gesture_.startDist = dist;
    gesture_.startAngle = std::atan2(dy, dx);
    gesture_.scale = 1.0f;
    gesture_.rotation = 0.0f;
    gesture_.cx = (a.x + b.x) * 0.5f;
    gesture_.cy = (a.y + b.y) * 0.5f;

    float docX = gesture_.cx;
    float docY = gesture_.cy - static_cast<float>(contentTop()) + scrollY_;
    dom::Element* target = hitTest(docX, docY);
    if (!target) target = document_->body();
    gesture_.target.assign(document_.get(), target);

    dispatchGestureEvent("gesturestart");
}

void Engine::gestureUpdate(uint64_t movedFinger) {
    if (!gesture_.active) return;
    if (movedFinger != gesture_.fingerA && movedFinger != gesture_.fingerB) return;
    TouchContact* a = touchByFinger(gesture_.fingerA);
    TouchContact* b = touchByFinger(gesture_.fingerB);
    if (!a || !b) return;

    const float dx = b->x - a->x, dy = b->y - a->y;
    float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 1.0f) dist = 1.0f;
    gesture_.scale = dist / gesture_.startDist;

    // Screen y grows downward, so a growing atan2 angle is a visually
    // CLOCKWISE rotation — matching WebKit's clockwise-positive convention.
    // Unwrap relative to the last report so continuous rotation past ±180°
    // keeps accumulating instead of snapping.
    float deg = (std::atan2(dy, dx) - gesture_.startAngle) * (180.0f / 3.14159265358979f);
    while (deg - gesture_.rotation > 180.0f) deg -= 360.0f;
    while (deg - gesture_.rotation < -180.0f) deg += 360.0f;
    gesture_.rotation = deg;

    gesture_.cx = (a->x + b->x) * 0.5f;
    gesture_.cy = (a->y + b->y) * 0.5f;

    dispatchGestureEvent("gesturechange");
}

void Engine::gestureEndIfFounder(uint64_t endedFinger) {
    if (!gesture_.active) return;
    if (endedFinger != gesture_.fingerA && endedFinger != gesture_.fingerB) return;
    dispatchGestureEvent("gestureend");   // final scale/rotation/centroid
    gesture_.active = false;
    gesture_.target.reset();
    // 2+ fingers still down (the other founder + extras): a fresh gesture
    // starts immediately, re-based to scale 1 / rotation 0.
    gestureMaybeStart();
}

// ---------------------------------------------------------------------------
// Contact lifecycle entry points (SDL finger events + headless seam)
// ---------------------------------------------------------------------------

void Engine::handleTouchDown(uint64_t fingerId, float x, float y, float pressure) {
    if (!document_) return;
    if (touchByFinger(fingerId)) return;   // duplicate down for a live contact
    uiDirty_ = true;

    TouchContact c;
    c.fingerId = fingerId;
    c.pointerId = nextTouchPointerId_++;
    c.primary = touchContacts_.empty();
    c.x = c.downX = x;
    c.y = c.downY = y;
    c.pressure = std::clamp(pressure, 0.0f, 1.0f);

    float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
    dom::Element* target = hitTest(docX, docY);
    c.startTarget.assign(document_.get(), target);
    touchContacts_.push_back(c);

    // Spec order: pointerdown, then touchstart.
    bool prevented = dispatchTouchPointerEvent("pointerdown", c, /*cancelable=*/true);
    prevented = dispatchTouchEvent("touchstart", c, /*cancelable=*/true) || prevented;
    if (prevented) {
        if (TouchContact* live = touchByFinger(fingerId)) {
            live->compatSuppressed = true;
        }
    }
    // A second finger landing starts a two-finger gesture (after the normal
    // pointer/touch dispatch, which is never affected by gestures).
    gestureMaybeStart();
}

void Engine::handleTouchMove(uint64_t fingerId, float x, float y, float pressure) {
    if (!document_) return;
    TouchContact* live = touchByFinger(fingerId);
    if (!live) return;   // move for an unknown/ended contact
    uiDirty_ = true;

    live->x = x;
    live->y = y;
    live->pressure = std::clamp(pressure, 0.0f, 1.0f);
    if (!live->moved) {
        float dx = x - live->downX, dy = y - live->downY;
        if (dx * dx + dy * dy > kTapSlopPx * kTapSlopPx) live->moved = true;
    }

    TouchContact snapshot = *live;
    dispatchTouchPointerEvent("pointermove", snapshot, /*cancelable=*/true);
    dispatchTouchEvent("touchmove", snapshot, /*cancelable=*/true);
    gestureUpdate(fingerId);   // gesturechange when a founding finger moved
}

void Engine::handleTouchUp(uint64_t fingerId, float x, float y) {
    if (!document_) return;
    TouchContact* live = touchByFinger(fingerId);
    if (!live) return;
    uiDirty_ = true;

    live->x = x;
    live->y = y;
    if (!live->moved) {
        float dx = x - live->downX, dy = y - live->downY;
        if (dx * dx + dy * dy > kTapSlopPx * kTapSlopPx) live->moved = true;
    }
    TouchContact ended = *live;

    // pointerup first (implicit capture release inside) …
    dispatchTouchPointerEvent("pointerup", ended, /*cancelable=*/true);

    // … then remove the contact and fire touchend.
    for (auto it = touchContacts_.begin(); it != touchContacts_.end(); ++it) {
        if (it->fingerId == fingerId) { touchContacts_.erase(it); break; }
    }
    bool endPrevented = dispatchTouchEvent("touchend", ended, /*cancelable=*/true);

    // A founding finger lifting ends the gesture (and re-starts one over the
    // remaining contacts when 2+ are still down).
    gestureEndIfFounder(fingerId);

    // Compat mouse for a clean primary tap. preventDefault on pointerdown /
    // touchstart (recorded in compatSuppressed) or on this touchend all
    // suppress it, matching the web's "cancel the compat mouse events" rule.
    if (ended.primary && !ended.moved && !ended.compatSuppressed && !endPrevented) {
        dispatchCompatMouseForTap(ended);
    }
}

void Engine::handleTouchCancel(uint64_t fingerId, float x, float y) {
    if (!document_) return;
    TouchContact* live = touchByFinger(fingerId);
    if (!live) return;
    uiDirty_ = true;

    live->x = x;
    live->y = y;
    TouchContact ended = *live;

    // pointercancel / touchcancel are not cancelable, and a cancelled contact
    // never synthesizes compat mouse events.
    dispatchTouchPointerEvent("pointercancel", ended, /*cancelable=*/false);
    for (auto it = touchContacts_.begin(); it != touchContacts_.end(); ++it) {
        if (it->fingerId == fingerId) { touchContacts_.erase(it); break; }
    }
    dispatchTouchEvent("touchcancel", ended, /*cancelable=*/false);
    gestureEndIfFounder(fingerId);
}

} // namespace bro::engine
