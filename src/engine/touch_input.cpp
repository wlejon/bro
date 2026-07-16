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

#include "js/runtime.h"
#include "js/event_dispatch.h"
#include "js/dom_bindings.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "util/time.h"

#include <SDL3/SDL.h>
#include <algorithm>
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
    if (!jsRuntime_ || !document_) return false;

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
        js::dispatchDomEvent(jsRuntime_->getContext(), target, pe);
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

// Wrap a JS array of Touch objects in a polyfill TouchList. Consumes `arr`.
static JSValue makeJsTouchList(JSContext* ctx, JSValue arr) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "TouchList");
    JSValue list = JS_IsConstructor(ctx, ctor)
        ? JS_CallConstructor(ctx, ctor, 1, &arr)
        : JS_DupValue(ctx, arr);   // degrade to the raw array
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, arr);
    return list;
}

bool Engine::dispatchTouchEvent(const char* type, const TouchContact& changed,
                                bool cancelable) {
    if (!jsRuntime_ || !document_) return false;
    // Touch events fire at the contact's touchstart target for its whole
    // lifetime (W3C targeting rule) — a finger sliding off the element keeps
    // reporting to it. Target freed by earlier JS ⇒ nothing to dispatch to.
    dom::Element* target = changed.startTarget.get();
    if (!target) return false;

    JSContext* ctx = jsRuntime_->getContext();
    const float ct = static_cast<float>(contentTop());
    const float scroll = scrollY_;

    // Build a JS `Touch` instance via the polyfill constructor.
    // Touch.identifier is the contact's pointerId, so the pointer and touch
    // streams correlate 1:1.
    auto makeJsTouch = [&](const TouchContact& c,
                           dom::Element* touchTarget) -> JSValue {
        JSValue opts = JS_NewObject(ctx);
        float clientY = c.y - ct;
        JS_SetPropertyStr(ctx, opts, "identifier", JS_NewInt32(ctx, c.pointerId));
        JS_SetPropertyStr(ctx, opts, "target",
                          touchTarget ? js::DomBindings::wrapElement(ctx, touchTarget)
                                      : JS_NULL);
        JS_SetPropertyStr(ctx, opts, "clientX", JS_NewFloat64(ctx, c.x));
        JS_SetPropertyStr(ctx, opts, "clientY", JS_NewFloat64(ctx, clientY));
        JS_SetPropertyStr(ctx, opts, "pageX", JS_NewFloat64(ctx, c.x));
        JS_SetPropertyStr(ctx, opts, "pageY", JS_NewFloat64(ctx, clientY + scroll));
        JS_SetPropertyStr(ctx, opts, "screenX", JS_NewFloat64(ctx, c.x));
        JS_SetPropertyStr(ctx, opts, "screenY", JS_NewFloat64(ctx, c.y));
        JS_SetPropertyStr(ctx, opts, "force", JS_NewFloat64(ctx, c.pressure));

        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ctor = JS_GetPropertyStr(ctx, global, "Touch");
        JSValue touch = JS_IsConstructor(ctx, ctor)
            ? JS_CallConstructor(ctx, ctor, 1, &opts)
            : JS_UNDEFINED;
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, global);
        JS_FreeValue(ctx, opts);
        return touch;
    };

    // Live lists come from the contact table: `touches` = every finger on the
    // surface (the caller has already removed ended/cancelled contacts, per
    // spec), `targetTouches` = the live subset that started on this event's
    // target, `changedTouches` = the one contact this event reports.
    JSValue touchesArr = JS_NewArray(ctx);
    JSValue targetArr = JS_NewArray(ctx);
    uint32_t ti = 0, gi = 0;
    for (const auto& c : touchContacts_) {
        dom::Element* cTarget = c.startTarget.get();
        JSValue t = makeJsTouch(c, cTarget);
        JS_SetPropertyUint32(ctx, touchesArr, ti++, JS_DupValue(ctx, t));
        if (cTarget == target) {
            JS_SetPropertyUint32(ctx, targetArr, gi++, JS_DupValue(ctx, t));
        }
        JS_FreeValue(ctx, t);
    }
    JSValue changedArr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, changedArr, 0, makeJsTouch(changed, target));

    int mod = currentModState();
    JSValue opts = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, opts, "bubbles", JS_TRUE);
    JS_SetPropertyStr(ctx, opts, "cancelable", JS_NewBool(ctx, cancelable));
    JS_SetPropertyStr(ctx, opts, "composed", JS_TRUE);
    JS_SetPropertyStr(ctx, opts, "touches", makeJsTouchList(ctx, touchesArr));
    JS_SetPropertyStr(ctx, opts, "targetTouches", makeJsTouchList(ctx, targetArr));
    JS_SetPropertyStr(ctx, opts, "changedTouches", makeJsTouchList(ctx, changedArr));
    JS_SetPropertyStr(ctx, opts, "ctrlKey", JS_NewBool(ctx, (mod & SDL_KMOD_CTRL) != 0));
    JS_SetPropertyStr(ctx, opts, "shiftKey", JS_NewBool(ctx, (mod & SDL_KMOD_SHIFT) != 0));
    JS_SetPropertyStr(ctx, opts, "altKey", JS_NewBool(ctx, (mod & SDL_KMOD_ALT) != 0));
    JS_SetPropertyStr(ctx, opts, "metaKey", JS_NewBool(ctx, (mod & SDL_KMOD_GUI) != 0));

    // Dispatch a real polyfill TouchEvent instance (so `e instanceof
    // TouchEvent` holds) through the standard three-phase path: the C++
    // Event drives propagation, the JS object carries the touch payload.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "TouchEvent");
    JSValue jsEvent = JS_UNDEFINED;
    if (JS_IsConstructor(ctx, ctor)) {
        JSValue typeVal = JS_NewString(ctx, type);
        JSValueConst args[2] = { typeVal, opts };
        jsEvent = JS_CallConstructor(ctx, ctor, 2, args);
        JS_FreeValue(ctx, typeVal);
    }
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, opts);
    if (JS_IsException(jsEvent) || JS_IsUndefined(jsEvent)) {
        if (JS_IsException(jsEvent)) {
            JS_FreeValue(ctx, JS_GetException(ctx));  // clear a throwing ctor
        }
        JS_FreeValue(ctx, jsEvent);
        jsEvent = JS_UNDEFINED;   // polyfill unavailable — plain dispatch
    } else {
        JS_SetPropertyStr(ctx, jsEvent, "isTrusted", JS_TRUE);
    }

    dom::Event evt(type, /*bubbles=*/true, cancelable);
    evt.setIsTrusted(true);
    js::dispatchDomEvent(ctx, target, evt, jsEvent);
    JS_FreeValue(ctx, jsEvent);
    return evt.defaultPrevented();
}

// ---------------------------------------------------------------------------
// Compat mouse events (primary-contact tap)
// ---------------------------------------------------------------------------

void Engine::dispatchCompatMouseForTap(const TouchContact& c) {
    if (!document_ || !jsRuntime_) return;

    const float x = c.x, y = c.y;
    const float ct = static_cast<float>(contentTop());
    const float clientY = y - ct;
    float docX = x, docY = clientY + scrollY_;
    dom::Element* target = hitTest(docX, docY);
    int mod = currentModState();
    double nowMs = util::currentTimeMs();

    ControlContext cctx{document_.get(), jsRuntime_->getContext(),
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
    jsRuntime_->executePendingJobs();

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
    jsRuntime_->executePendingJobs();
    // Focus/caret chrome lives in the cached base layer.
    markAppBaseDirty();
}

// ---------------------------------------------------------------------------
// Contact lifecycle entry points (SDL finger events + headless seam)
// ---------------------------------------------------------------------------

void Engine::handleTouchDown(uint64_t fingerId, float x, float y, float pressure) {
    if (!document_ || !jsRuntime_) return;
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

    // Spec order: pointerdown, then touchstart. Listeners run JS that can
    // re-enter the touch API (headless injection) and reallocate the table,
    // so our entry is only mutated through a fingerId re-lookup afterwards.
    bool prevented = dispatchTouchPointerEvent("pointerdown", c, /*cancelable=*/true);
    prevented = dispatchTouchEvent("touchstart", c, /*cancelable=*/true) || prevented;
    if (prevented) {
        if (TouchContact* live = touchByFinger(fingerId)) {
            live->compatSuppressed = true;
        }
    }
    jsRuntime_->executePendingJobs();
}

void Engine::handleTouchMove(uint64_t fingerId, float x, float y, float pressure) {
    if (!document_ || !jsRuntime_) return;
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

    TouchContact snapshot = *live;   // JS below can invalidate the pointer
    dispatchTouchPointerEvent("pointermove", snapshot, /*cancelable=*/true);
    dispatchTouchEvent("touchmove", snapshot, /*cancelable=*/true);
    jsRuntime_->executePendingJobs();
}

void Engine::handleTouchUp(uint64_t fingerId, float x, float y) {
    if (!document_ || !jsRuntime_) return;
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

    // … then remove the contact — TouchEvent.touches excludes fingers lifted
    // in this event — and fire touchend. Re-find by fingerId: the JS above
    // may have mutated the table.
    for (auto it = touchContacts_.begin(); it != touchContacts_.end(); ++it) {
        if (it->fingerId == fingerId) { touchContacts_.erase(it); break; }
    }
    bool endPrevented = dispatchTouchEvent("touchend", ended, /*cancelable=*/true);

    // Compat mouse for a clean primary tap. preventDefault on pointerdown /
    // touchstart (recorded in compatSuppressed) or on this touchend all
    // suppress it, matching the web's "cancel the compat mouse events" rule.
    if (ended.primary && !ended.moved && !ended.compatSuppressed && !endPrevented) {
        dispatchCompatMouseForTap(ended);
    }
    jsRuntime_->executePendingJobs();
}

void Engine::handleTouchCancel(uint64_t fingerId, float x, float y) {
    if (!document_ || !jsRuntime_) return;
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
    jsRuntime_->executePendingJobs();
}

} // namespace bro::engine
