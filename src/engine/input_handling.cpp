// Engine input handling methods — split from engine.cpp for readability.
// These are Engine member function implementations, not a separate class.

#include "engine/engine.h"
#include "engine/hit_testing.h"
#include "engine/key_mapping.h"
#include "engine/overflow.h"
#include "engine/overlay.h"
#include "engine/replaced_elements.h"
#include "engine/settings.h"

#include "platform/sdl_window.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/event_dispatch.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/key_handle_result.h"
#include "util/time.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace bro::engine {

// ---------------------------------------------------------------------------
// Action event dispatch helper
// ---------------------------------------------------------------------------

/// Dispatch an "action" event on document body if the key maps to a defined action.
/// The JS event has: detail = { action: "name", phase: "down"|"up" }
static void dispatchActionEvent(JSContext* ctx, Settings* settings,
                                dom::Element* target,
                                int keycode, int mod, const char* phase) {
    if (!settings || !target || !ctx) return;

    std::string webKey = sdlKeycodeToWebKey(keycode, mod);
    std::string action = settings->getActionForKey(webKey);
    if (action.empty()) return;

    // Create JS event with detail
    JSValue jsEvent = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, jsEvent, "type", JS_NewString(ctx, "action"));
    JS_SetPropertyStr(ctx, jsEvent, "bubbles", JS_NewBool(ctx, 1));
    JS_SetPropertyStr(ctx, jsEvent, "cancelable", JS_NewBool(ctx, 1));

    JSValue detail = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, detail, "action", JS_NewString(ctx, action.c_str()));
    JS_SetPropertyStr(ctx, detail, "phase", JS_NewString(ctx, phase));
    JS_SetPropertyStr(ctx, detail, "key", JS_NewString(ctx, webKey.c_str()));
    JS_SetPropertyStr(ctx, jsEvent, "detail", detail);

    dom::Event evt("action");
    evt.setIsTrusted(true);
    js::dispatchDomEvent(ctx, target, evt, jsEvent);

    JS_FreeValue(ctx, jsEvent);
}

// ---------------------------------------------------------------------------
// Input focus helpers
// ---------------------------------------------------------------------------

// Safe wrapper: SDL_GetModState() requires SDL_INIT_VIDEO. In --no-gpu
// headless mode no SDL video subsystem is initialized, so the call would
// dereference an internal NULL pointer and crash.  Return 0 (no modifiers)
// when there is no window.
static int safeGetModState(platform::Window* window) {
    return window ? static_cast<int>(SDL_GetModState()) : 0;
}

// Safe wrappers for SDL text input — no-ops when there is no window.
static void safeStartTextInput(platform::Window* window) {
    if (window) SDL_StartTextInput(window->getSDLWindow());
}
static void safeStopTextInput(platform::Window* window) {
    if (window) SDL_StopTextInput(window->getSDLWindow());
}

// Returns true if the element is a focusable text-editing control (input or textarea)
static bool isTextEditable(dom::Element* el) {
    return getElInput(el) || getElTextarea(el);
}

// Build a KeyboardEvent with all modifier fields set.
static dom::KeyboardEvent makeKeyboardEvent(const char* type,
                                            int keycode, int scancode,
                                            int mod, bool repeat) {
    dom::KeyboardEvent evt(type);
    evt.setKey(sdlKeycodeToWebKey(keycode, mod));
    evt.setCode(sdlScancodeToWebCode(scancode));
    evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
    evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
    evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
    evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
    evt.setRepeat(repeat);
    evt.setIsTrusted(true);

    // Set location for left/right modifier keys
    if (scancode == SDL_SCANCODE_LSHIFT || scancode == SDL_SCANCODE_LCTRL ||
        scancode == SDL_SCANCODE_LALT || scancode == SDL_SCANCODE_LGUI)
        evt.setLocation(1); // DOM_KEY_LOCATION_LEFT
    else if (scancode == SDL_SCANCODE_RSHIFT || scancode == SDL_SCANCODE_RCTRL ||
             scancode == SDL_SCANCODE_RALT || scancode == SDL_SCANCODE_RGUI)
        evt.setLocation(2); // DOM_KEY_LOCATION_RIGHT
    else if (keycode >= SDLK_KP_DIVIDE && keycode <= SDLK_KP_EQUALS)
        evt.setLocation(3); // DOM_KEY_LOCATION_NUMPAD

    return evt;
}

// Compute element-relative offset coordinates (per spec: relative to padding edge)
static void computeOffset(dom::MouseEvent& evt, dom::Element* target) {
    if (!target) return;
    auto& box = target->layoutBox();
    float absX = box.contentRect.x;
    float absY = box.contentRect.y;
    for (auto* lp = target->layoutParent(); lp; lp = lp->layoutParent()) {
        auto& pb = lp->layoutBox();
        absX += pb.contentRect.x;
        absY += pb.contentRect.y;
        absY -= lp->scrollTopValue();
    }
    evt.setOffsetX(evt.clientX() - static_cast<double>(absX));
    evt.setOffsetY(evt.clientY() - static_cast<double>(absY));
}

// Convert SDL3 mouse button id (1=left, 2=middle, 3=right, 4=X1, 5=X2)
// into the DOM MouseEvent.button index (0=left, 1=middle, 2=right, 3=back, 4=forward).
// SDL and the DOM disagree on both the base index and the middle/right ordering.
static int sdlToDomButton(int sdlButton) {
    switch (sdlButton) {
        case 1: return 0;  // SDL left   -> DOM primary
        case 2: return 1;  // SDL middle -> DOM auxiliary
        case 3: return 2;  // SDL right  -> DOM secondary
        case 4: return 3;  // SDL X1     -> DOM back
        case 5: return 4;  // SDL X2     -> DOM forward
        default: return sdlButton - 1;
    }
}

// MouseEvent.buttons bitmask values, keyed by DOM button index.
// Note that DOM swaps right (2) and middle (4) relative to a naive 1<<n encoding.
static int domButtonMask(int domButton) {
    switch (domButton) {
        case 0: return 1;   // left
        case 1: return 4;   // middle
        case 2: return 2;   // right
        case 3: return 8;   // back
        case 4: return 16;  // forward
        default: return 0;
    }
}

// Build a MouseEvent with standard fields populated
static void populateMouseEvent(dom::MouseEvent& evt, float x, float y,
                               int button, int buttons,
                               float movementX, float movementY,
                               float scrollY, int mod) {
    evt.setClientX(static_cast<double>(x));
    evt.setClientY(static_cast<double>(y));
    evt.setScreenX(static_cast<double>(x));
    evt.setScreenY(static_cast<double>(y));
    evt.setPageX(static_cast<double>(x));
    evt.setPageY(static_cast<double>(y + scrollY));
    evt.setMovementX(static_cast<double>(movementX));
    evt.setMovementY(static_cast<double>(movementY));
    evt.setButton(button);
    evt.setButtons(buttons);
    evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
    evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
    evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
    evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
    evt.setIsTrusted(true);
}

// ---------------------------------------------------------------------------
// Scrollbar hit testing helper
// ---------------------------------------------------------------------------

/// Walk the DOM tree to find which overflow element's scrollbar contains (x, y).
/// Returns the element whose scrollbar was hit, or nullptr.
/// Sets outMetrics to the scrollbar metrics for the hit element.
static dom::Element* findElementScrollbarHit(
    dom::Element* elem, float x, float y,
    float offsetX, float offsetY,
    Scrollbar& scrollbar, ScrollbarMetrics& outMetrics)
{
    if (!elem) return nullptr;
    auto& style = elem->computedStyle();
    {
        auto it = style.find("display");
        if (it != style.end() && it->second == "none") return nullptr;
    }

    auto& lbox = elem->layoutBox();
    float absX = lbox.contentRect.x + offsetX;
    float absY = lbox.contentRect.y + offsetY;

    // Recurse into composed children FIRST to find the deepest match
    float childOffsetX = absX;
    float childOffsetY = absY - elem->scrollTopValue();
    dom::Element* hit = nullptr;
    elem->forEachComposedChild([&](dom::Element* child) {
        if (!hit) {
            hit = findElementScrollbarHit(child, x, y,
                childOffsetX, childOffsetY, scrollbar, outMetrics);
        }
    });
    if (hit) return hit;

    // Only show scrollbars for scroll/auto, not hidden
    std::string ov = getOverflowY(style);
    if (overflowScrollable(ov)) {
        float maxST = maxScrollTop(elem);
        if (maxST > 0) {
            float viewH = lbox.contentRect.height;
            float contentH = viewH + maxST;
            float bx = absX - lbox.padding.left - lbox.border.left;
            float by = absY - lbox.padding.top - lbox.border.top;
            float bw = lbox.fullWidth();
            float bh = lbox.fullHeight();

            auto& es = scrollbar.style();
            auto m = scrollbar.layout(
                bx + bw - es.width - es.margin,
                by, bh, contentH, viewH,
                elem->scrollTopValue());
            if (scrollbar.hitTest(x, y, m)) {
                outMetrics = m;
                return elem;
            }
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Focus event dispatching
// ---------------------------------------------------------------------------

void Engine::dispatchFocusEvents(dom::Element* oldTarget, dom::Element* newTarget) {
    if (oldTarget == newTarget) return;

    (void)jsRuntime_; // focus events are dispatched via dispatchEvent

    // blur (non-bubbling) on old target
    if (oldTarget) {
        dom::FocusEvent blurEvt("blur", false, false);
        blurEvt.setRelatedTarget(newTarget);
        blurEvt.setIsTrusted(true);
        dispatchEvent(oldTarget, blurEvt);
    }

    // focus (non-bubbling) on new target
    if (newTarget) {
        dom::FocusEvent focusEvt("focus", false, false);
        focusEvt.setRelatedTarget(oldTarget);
        focusEvt.setIsTrusted(true);
        dispatchEvent(newTarget, focusEvt);
    }

    // focusout (bubbling) on old target
    if (oldTarget) {
        dom::FocusEvent focusoutEvt("focusout", true, false);
        focusoutEvt.setRelatedTarget(newTarget);
        focusoutEvt.setIsTrusted(true);
        dispatchEvent(oldTarget, focusoutEvt);
    }

    // focusin (bubbling) on new target
    if (newTarget) {
        dom::FocusEvent focusinEvt("focusin", true, false);
        focusinEvt.setRelatedTarget(oldTarget);
        focusinEvt.setIsTrusted(true);
        dispatchEvent(newTarget, focusinEvt);
    }
}

// ---------------------------------------------------------------------------
// Scroll event dispatching
// ---------------------------------------------------------------------------

void Engine::dispatchScrollEvent(dom::Element* el) {
    if (!el) return;
    dom::Event evt("scroll", false, false); // scroll doesn't bubble
    evt.setIsTrusted(true);
    dispatchEvent(el, evt);
}

// ---------------------------------------------------------------------------
// Mouse events
// ---------------------------------------------------------------------------

void Engine::handleMouseDown(float x, float y, int button) {
    // x, y = raw mouse position (screen space).
    // docX, docY = document space (for hit testing into the scrolled document).
    // IMPORTANT: overlay positions (lastDrawPos, color picker, select dropdown)
    // are in screen space — always use x/y when comparing, never docX/docY.
    float docX = x, docY = y + scrollY_;
    uiDirty_ = true;

    // Convert SDL button id to DOM convention up front so every downstream
    // event sees the standard 0=left/1=middle/2=right indexing.
    button = sdlToDomButton(button);

    // Overlay manager sees every input event before the DOM so hover/click
    // can't leak through to elements underneath. (Same pattern in the other
    // handleMouse*/handleKey*/handleTextInput methods.)
    if (overlayMgr_.handleMouseDown(x, y, button)) {
        pressedButtons_ |= domButtonMask(button);
        uiDirty_ = true;
        return;
    }

    // Forward to system overlay first — if it consumes, skip app handling
    if (systemHandleMouseDown(x, y, button)) {
        pressedButtons_ |= domButtonMask(button);
        return;
    }

    // Engine-level 3D gizmo — sits between modal UI and DOM. Consumes only
    // when a handle is hit; otherwise falls through to DOM / canvas.
    if (gizmoHandleMouseDown(x, y, button)) {
        pressedButtons_ |= domButtonMask(button);
        return;
    }

    // Update button bitmask (DOM convention: 1=left, 2=right, 4=middle, ...)
    pressedButtons_ |= domButtonMask(button);

    // --- Scrollbar interaction (before DOM hit testing) ---

    // Check viewport scrollbar
    {
        float vh = static_cast<float>(viewportHeight_);
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            0.0f, vh, documentHeight_, vh, scrollY_);
        if (viewportScrollbar_.hitTest(x, y, m)) {
            if (viewportScrollbar_.thumbHitTest(x, y, m)) {
                viewportScrollbar_.beginDrag(y, m);
                draggingViewportScrollbar_ = true;
            } else {
                // Click on track — page scroll
                scrollY_ = viewportScrollbar_.scrollToPosition(y,
                    documentHeight_, vh, m);
            }
            uiDirty_ = true;
            return; // consumed
        }
    }

    // Check element scrollbars
    if (document_ && document_->documentElement()) {
        ScrollbarMetrics em;
        dom::Element* hitElem = findElementScrollbarHit(
            document_->documentElement(), x, y,
            0.0f, -scrollY_, elementScrollbar_, em);
        if (hitElem) {
            if (elementScrollbar_.thumbHitTest(x, y, em)) {
                elementScrollbar_.beginDrag(y, em);
                scrollbarDragTarget_ = hitElem;
            } else {
                // Click on track — page scroll
                float viewH = hitElem->layoutBox().contentRect.height;
                float maxST = maxScrollTop(hitElem);
                float contentH = viewH + maxST;
                float newScroll = elementScrollbar_.scrollToPosition(y,
                    contentH, viewH, em);
                float prev = hitElem->scrollTopValue();
                float clamped = std::clamp(newScroll, 0.0f, maxST);
                hitElem->setScrollTopValue(clamped);
                if (clamped != prev) dispatchScrollEvent(hitElem);
            }
            uiDirty_ = true;
            return; // consumed
        }
    }

    if (document_) {
        dom::MouseEvent evt("mousedown");
        int mod = safeGetModState(window_.get());
        populateMouseEvent(evt, x, y, button, pressedButtons_,
                          x - lastMouseX_, y - lastMouseY_, scrollY_, mod);

        dom::Element* target = hitTest(docX, docY);
        mouseDownTarget_ = target;
        if (target) {
            // Track focus change
            auto* prevActive = document_->activeElement();

            // Unfocus previous controls and check if click was consumed
            ControlContext cctx{document_.get(), jsRuntime_->getContext(),
                               renderer_.get(), window_.get(), &uiDirty_,
                               &overlayMgr_, OverlayContext::App,
                               viewportWidth_, viewportHeight_};
            auto disp = unfocusPreviousControl(cctx, prevActive);
            if (disp == ClickDisposition::Consumed) {
                computeOffset(evt, target);
                dispatchEvent(target, evt);
                jsRuntime_->executePendingJobs();
                return;
            }

            // Set new active element and dispatch focus events
            document_->setActiveElement(target);
            if (target != prevActive) {
                bro::engine::dispatchFocusEvents(cctx, prevActive, target);
            }
            jsRuntime_->executePendingJobs();

            // Focus/activate the newly-clicked control
            focusNewControl(cctx, target, x, docY);

            computeOffset(evt, target);
            dispatchEvent(target, evt);
        }
    }
}

void Engine::handleMouseUp(float x, float y, int button) {
    // x, y = screen space. docX, docY = document space (see handleMouseDown).
    float docX = x, docY = y + scrollY_;
    uiDirty_ = true;

    // Match handleMouseDown: SDL -> DOM button index.
    button = sdlToDomButton(button);

    // Update button bitmask (DOM convention)
    pressedButtons_ &= ~domButtonMask(button);

    if (overlayMgr_.handleMouseUp(x, y, button)) {
        uiDirty_ = true;
        return;
    }

    // Forward to system overlay first
    if (systemHandleMouseUp(x, y, button)) {
        return;
    }

    // Gizmo consumes mouseUp only when the drag was active.
    if (gizmoHandleMouseUp(x, y, button)) {
        return;
    }

    // End scrollbar drags
    if (viewportScrollbar_.isDragging()) {
        viewportScrollbar_.endDrag();
        draggingViewportScrollbar_ = false;
        uiDirty_ = true;
    }
    if (elementScrollbar_.isDragging()) {
        elementScrollbar_.endDrag();
        scrollbarDragTarget_ = nullptr;
        uiDirty_ = true;
    }

    // Stop range slider dragging
    if (document_) {
        auto* activeEl = document_->activeElement();
        auto* input = getElInput(activeEl);
        if (input && input->isDragging()) {
            input->setDragging(false);
            dom::Event changeEvt("change");
            dispatchEvent(activeEl, changeEvt);
            uiDirty_ = true;
        }
    }

    if (document_) {
        dom::Element* target = hitTest(docX, docY);
        int mod = safeGetModState(window_.get());

        // Dispatch mouseup event
        {
            dom::MouseEvent upEvt("mouseup");
            populateMouseEvent(upEvt, x, y, button, pressedButtons_,
                              x - lastMouseX_, y - lastMouseY_, scrollY_, mod);
            if (target) {
                computeOffset(upEvt, target);
                dispatchEvent(target, upEvt);
            }
        }

        // Dispatch click event (only if mouseup is on the same element as mousedown)
        if (target && target == mouseDownTarget_) {
            // Double-click detection (thresholds from config)
            double now = util::currentTimeMs();

            if (lastClickTarget_ == target &&
                (now - lastClickTimeMs_) < inputConfig_.doubleClickThresholdMs &&
                std::abs(x - lastClickX_) < inputConfig_.doubleClickDistancePx &&
                std::abs(y - lastClickY_) < inputConfig_.doubleClickDistancePx) {
                clickCount_++;
            } else {
                clickCount_ = 1;
            }
            lastClickTimeMs_ = now;
            lastClickX_ = x;
            lastClickY_ = y;
            lastClickTarget_ = target;

            dom::MouseEvent clickEvt("click");
            populateMouseEvent(clickEvt, x, y, button, pressedButtons_,
                              x - lastMouseX_, y - lastMouseY_, scrollY_, mod);
            clickEvt.setDetail(clickCount_);
            computeOffset(clickEvt, target);
            dispatchEvent(target, clickEvt);

            // Dispatch dblclick on second click
            if (clickCount_ == 2) {
                dom::MouseEvent dblEvt("dblclick", true, true);
                populateMouseEvent(dblEvt, x, y, button, pressedButtons_,
                                  x - lastMouseX_, y - lastMouseY_, scrollY_, mod);
                dblEvt.setDetail(2);
                computeOffset(dblEvt, target);
                dispatchEvent(target, dblEvt);
            }

            // Dispatch contextmenu on right-click
            if (button == 2) {
                dom::MouseEvent ctxEvt("contextmenu", true, true);
                populateMouseEvent(ctxEvt, x, y, button, pressedButtons_,
                                  x - lastMouseX_, y - lastMouseY_, scrollY_, mod);
                computeOffset(ctxEvt, target);
                dispatchEvent(target, ctxEvt);
            }
        }

        mouseDownTarget_ = nullptr;
        if (jsRuntime_) jsRuntime_->executePendingJobs();
    }
}

void Engine::handleMouseMove(float x, float y, float xrel, float yrel) {
    // If the locked element was removed from the DOM, release the lock.
    if (lockedElement_ && !lockedElement_->isAlive()) exitPointerLock();

    // Pointer lock: OS cursor is pinned by SDL's relative mouse mode. SDL still
    // accumulates a virtual x/y in motion events, but we ignore it — clientX/Y
    // stays frozen at the lock position and movementX/Y carries the delta.
    if (lockedElement_) {
        if (document_) {
            int mod = safeGetModState(window_.get());
            dom::MouseEvent moveEvt("mousemove", true, true);
            populateMouseEvent(moveEvt, lockedMouseX_, lockedMouseY_, -1,
                               pressedButtons_, xrel, yrel, scrollY_, mod);
            computeOffset(moveEvt, lockedElement_);
            dispatchEvent(lockedElement_, moveEvt);
            if (jsRuntime_) jsRuntime_->executePendingJobs();
        }
        return;
    }

    // x, y = screen space. docX, docY = document space (see handleMouseDown).
    float docX = x, docY = y + scrollY_;

    // Overlay manager sees mousemove first. While an overlay is active,
    // DOM hover is suppressed entirely — otherwise hovering elements under
    // the dropdown/picker would trigger :hover styles and JS handlers.
    bool overlayActive = overlayMgr_.hasActive();
    if (overlayActive && overlayMgr_.handleMouseMove(x, y)) {
        uiDirty_ = true;
    }

    // Forward to system overlay first (but don't block app mousemove —
    // overlay consumes only if mouse is over an overlay element)
    if (isSystemVisible()) {
        systemHandleMouseMove(x, y);
    }

    // Gizmo mousemove — drives hover state always; consumes only while
    // actively dragging (returns true in that case).
    if (gizmoHandleMouseMove(x, y)) {
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Viewport scrollbar drag
    if (viewportScrollbar_.isDragging()) {
        float vh = static_cast<float>(viewportHeight_);
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            0.0f, vh, documentHeight_, vh, scrollY_);
        float maxScroll = std::max(0.0f, documentHeight_ - vh);
        scrollY_ = std::clamp(
            viewportScrollbar_.updateDrag(y, documentHeight_, vh, m),
            0.0f, maxScroll);
        uiDirty_ = true;
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Element scrollbar drag
    if (elementScrollbar_.isDragging() && scrollbarDragTarget_) {
        auto* elem = scrollbarDragTarget_;
        float viewH = elem->layoutBox().contentRect.height;
        float maxST = maxScrollTop(elem);
        float contentH = viewH + maxST;

        auto& lbox = elem->layoutBox();
        auto& es = elementScrollbar_.style();
        float bh = lbox.fullHeight();
        auto m = elementScrollbar_.layout(0, 0, bh, contentH, viewH,
            elem->scrollTopValue());
        float newScroll = elementScrollbar_.updateDrag(y, contentH, viewH, m);
        float prev = elem->scrollTopValue();
        float clamped = std::clamp(newScroll, 0.0f, maxST);
        elem->setScrollTopValue(clamped);
        if (clamped != prev) dispatchScrollEvent(elem);
        uiDirty_ = true;
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Viewport scrollbar hover
    {
        float vh = static_cast<float>(viewportHeight_);
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            0.0f, vh, documentHeight_, vh, scrollY_);
        bool wasHovered = viewportScrollbar_.isHovered();
        viewportScrollbar_.setHovered(viewportScrollbar_.thumbHitTest(x, y, m));
        if (wasHovered != viewportScrollbar_.isHovered()) uiDirty_ = true;
    }

    // Element scrollbar hover (per-element tracking)
    if (document_ && document_->documentElement()) {
        ScrollbarMetrics em;
        dom::Element* hitElem = findElementScrollbarHit(
            document_->documentElement(), x, y,
            0.0f, -scrollY_, elementScrollbar_, em);
        dom::Element* prevHovered = scrollbarHoveredElement_;
        if (hitElem && elementScrollbar_.thumbHitTest(x, y, em)) {
            scrollbarHoveredElement_ = hitElem;
        } else {
            scrollbarHoveredElement_ = nullptr;
        }
        if (prevHovered != scrollbarHoveredElement_) uiDirty_ = true;
    }

    // Range slider dragging
    if (document_) {
        auto* activeEl = document_->activeElement();
        auto* rangeInput = getElInput(activeEl);
        if (rangeInput && rangeInput->isDragging()) {
            auto dp = rangeInput->lastDrawPos();
            float thumbR = 7.0f;
            float trackStart = dp.x + thumbR;
            float trackEnd = dp.x + dp.w - thumbR;
            float pct = (trackEnd > trackStart) ?
                std::clamp((x - trackStart) / (trackEnd - trackStart), 0.0f, 1.0f) : 0.0f;
            float mn = rangeInput->rangeMin(), mx = rangeInput->rangeMax();
            float val = mn + pct * (mx - mn);
            float step = rangeInput->rangeStep();
            if (step > 0) {
                val = mn + std::round((val - mn) / step) * step;
                val = std::clamp(val, mn, mx);
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", static_cast<double>(val));
            activeEl->setAttribute("value", buf);
            dispatchInputEvent(activeEl);
            uiDirty_ = true;
        }
    }

    // Dispatch mousemove event (suppressed while any overlay is active)
    if (document_ && !overlayActive) {
        dom::Element* target = hitTest(docX, docY);

        // Dispatch mouseover/mouseout when element changes (bubbling versions)
        if (target != hoveredElement_) {
            int mod = safeGetModState(window_.get());

            // mouseout on previous element (bubbles)
            if (hoveredElement_) {
                dom::MouseEvent outEvt("mouseout", true, true);
                populateMouseEvent(outEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod);
                outEvt.setRelatedTarget(target);
                computeOffset(outEvt, hoveredElement_);
                dispatchEvent(hoveredElement_, outEvt);
            }

            // mouseleave on previous element (doesn't bubble)
            if (hoveredElement_) {
                dom::MouseEvent leaveEvt("mouseleave", false, false);
                populateMouseEvent(leaveEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod);
                leaveEvt.setRelatedTarget(target);
                computeOffset(leaveEvt, hoveredElement_);
                dispatchEvent(hoveredElement_, leaveEvt);
            }

            // mouseover on new element (bubbles)
            if (target) {
                dom::MouseEvent overEvt("mouseover", true, true);
                populateMouseEvent(overEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod);
                overEvt.setRelatedTarget(hoveredElement_);
                computeOffset(overEvt, target);
                dispatchEvent(target, overEvt);
            }

            // mouseenter on new element (doesn't bubble)
            if (target) {
                dom::MouseEvent enterEvt("mouseenter", false, false);
                populateMouseEvent(enterEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod);
                enterEvt.setRelatedTarget(hoveredElement_);
                computeOffset(enterEvt, target);
                dispatchEvent(target, enterEvt);
            }

            // Mark old and new hovered elements dirty for :hover style re-resolve
            if (hoveredElement_) hoveredElement_->markDirty();
            if (target) target->markDirty();
            hoveredElement_ = target;
            uiDirty_ = true;
        }

        // Always dispatch mousemove
        if (target) {
            int mod = safeGetModState(window_.get());
            dom::MouseEvent moveEvt("mousemove", true, true);
            populateMouseEvent(moveEvt, x, y, -1, pressedButtons_,
                              xrel, yrel, scrollY_, mod);
            computeOffset(moveEvt, target);
            dispatchEvent(target, moveEvt);
        }

        if (jsRuntime_) jsRuntime_->executePendingJobs();
    }

    lastMouseX_ = x;
    lastMouseY_ = y;
}

// ---------------------------------------------------------------------------
// Pointer lock
// ---------------------------------------------------------------------------

bool Engine::requestPointerLock(dom::Element* target) {
    if (!target || !target->isAlive()) return false;
    if (lockedElement_ == target) return true;

    lockedElement_ = target;
    lockedMouseX_ = lastMouseX_;
    lockedMouseY_ = lastMouseY_;

    if (window_) {
        SDL_SetWindowRelativeMouseMode(window_->getSDLWindow(), true);
    }

    // Notify listeners on document (document.addEventListener forwards to documentElement).
    if (document_ && document_->documentElement()) {
        dom::Event evt("pointerlockchange", true, false);
        evt.setIsTrusted(true);
        dispatchEvent(document_->documentElement(), evt);
        if (jsRuntime_) jsRuntime_->executePendingJobs();
    }
    return true;
}

void Engine::exitPointerLock() {
    if (!lockedElement_) return;
    lockedElement_ = nullptr;

    if (window_) {
        // Warp back to the pre-lock cursor position before releasing
        // relative mode — per SDL3 docs, this is how you pin the cursor
        // to a specific location on exit. Without it the OS cursor
        // reappears wherever SDL last placed it during capture, which
        // looks like the cursor "jumps" when the user releases the drag.
        SDL_WarpMouseInWindow(window_->getSDLWindow(), lockedMouseX_, lockedMouseY_);
        SDL_SetWindowRelativeMouseMode(window_->getSDLWindow(), false);
    }

    if (document_ && document_->documentElement()) {
        dom::Event evt("pointerlockchange", true, false);
        evt.setIsTrusted(true);
        dispatchEvent(document_->documentElement(), evt);
        if (jsRuntime_) jsRuntime_->executePendingJobs();
    }
}

// ---------------------------------------------------------------------------
// Keyboard events
// ---------------------------------------------------------------------------

// Helper: update input value and dispatch "input" event for v-model
void Engine::dispatchInputEvent(dom::Element* el, const std::string& data,
                                const std::string& inputType) {
    if (!el) return;
    dom::InputEvent evt("input");
    evt.setData(data);
    evt.setInputType(inputType);
    evt.setIsTrusted(true);
    dispatchEvent(el, evt);
    jsRuntime_->executePendingJobs();
    uiDirty_ = true;
}

/// Apply a KeyHandleResult from a control: dispatch events, handle unfocus.
void Engine::applyKeyResult(dom::Element* el, const layout::KeyHandleResult& r) {
    if (r.dispatchChange) {
        dom::Event changeEvt("change");
        dispatchEvent(el, changeEvt);
    }
    if (r.dispatchInput) {
        dispatchInputEvent(el, r.inputData, r.inputType);
    }
    if (r.unfocus) {
        dispatchFocusEvents(el, nullptr);
        safeStopTextInput(window_.get());
    }
    if (r.handled) {
        uiDirty_ = true;
    }
}

void Engine::handleKeyDown(int keycode, int scancode, int mod, bool repeat) {
    if (overlayMgr_.handleKeyDown(keycode, mod)) {
        uiDirty_ = true;
        return;
    }

    // Check for system actions via the settings action binding system
    if (!repeat && settings_) {
        std::string webKey = sdlKeycodeToWebKey(keycode, mod);
        std::string action = settings_->getActionForKey(webKey);
        if (action == "system_toggle_perf") {
            toggleSystemPerf();
            uiDirty_ = true;
            return;
        }
        if (action == "system_toggle_settings") {
            toggleSystemSettings();
            uiDirty_ = true;
            return;
        }
    }

    if (!document_) return;

    // Tab key: dispatch to JS first; only advance focus if not prevented
    if (keycode == SDLK_TAB) {
        auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
        dom::Element* target = document_->activeElement();
        if (!target) target = document_->body();
        if (target) dispatchEvent(target, evt);

        if (!evt.defaultPrevented()) {
            advanceFocus((mod & SDL_KMOD_SHIFT) != 0);
            uiDirty_ = true;
        }
        return;
    }

    // Clipboard: Ctrl+C / Ctrl+X / Ctrl+V
    if ((mod & SDL_KMOD_CTRL) &&
        (keycode == SDLK_C || keycode == SDLK_X || keycode == SDLK_V)) {

        auto* activeEl = document_->activeElement();
        dom::Element* target = activeEl ? activeEl : document_->body();

        if (keycode == SDLK_V) {
            // Paste: read system clipboard, dispatch paste event
            char* clipText = SDL_GetClipboardText();
            std::string text = clipText ? clipText : "";
            SDL_free(clipText);

            dom::ClipboardEvent pasteEvt("paste", true, true);
            pasteEvt.setClipboardText(text);
            pasteEvt.setIsTrusted(true);
            dispatchEvent(target, pasteEvt);

            // If not prevented, insert into focused input/textarea
            if (!pasteEvt.defaultPrevented() && !text.empty() && activeEl) {
                layout::KeyHandleResult r;
                if (auto* input = getElInput(activeEl); input && input->isFocused()) {
                    r = input->handleTextInput(activeEl, text);
                } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
                    r = textarea->handleTextInput(activeEl, text);
                }
                if (r.handled) {
                    applyKeyResult(activeEl, r);
                    uiDirty_ = true;
                }
            }
        } else {
            // Copy or Cut: get text from focused input/textarea
            std::string text;
            if (activeEl) {
                if (auto* input = getElInput(activeEl); input && input->isFocused()) {
                    text = activeEl->getAttribute("value");
                } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
                    text = activeEl->getAttribute("value");
                }
            }

            std::string evtType = (keycode == SDLK_C) ? "copy" : "cut";
            dom::ClipboardEvent clipEvt(evtType, true, true);
            clipEvt.setClipboardText(text);
            clipEvt.setIsTrusted(true);
            dispatchEvent(target, clipEvt);

            if (!clipEvt.defaultPrevented() && !text.empty()) {
                SDL_SetClipboardText(text.c_str());

                // Cut: clear the field
                if (keycode == SDLK_X && activeEl) {
                    activeEl->setAttribute("value", "");
                    if (auto* input = getElInput(activeEl))
                        input->setCursorPos(0);
                    else if (auto* textarea = getElTextarea(activeEl))
                        textarea->setCursorPos(0);

                    dom::InputEvent inputEvt("input", true, false);
                    inputEvt.setInputType("deleteByCut");
                    inputEvt.setIsTrusted(true);
                    dispatchEvent(activeEl, inputEvt);
                    if (activeEl->document()) activeEl->document()->markDirty();
                    uiDirty_ = true;
                }
            }
        }
        return;
    }

    // Delegate to the active control
    auto* activeEl = document_->activeElement();
    layout::KeyHandleResult result;

    if (auto* input = getElInput(activeEl); input && input->isFocused()) {
        result = input->handleKeyDown(activeEl, keycode, mod);
    } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
        result = textarea->handleKeyDown(activeEl, keycode, mod);
    }

    if (result.handled) {
        applyKeyResult(activeEl, result);
        // Still dispatch keydown event for JS listeners
        auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
        dispatchEvent(activeEl, evt);
        return;
    }

    // Default: dispatch keydown to body
    auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
    dom::Element* target = document_->body();
    if (target) {
        dispatchEvent(target, evt);
    }

    // Dispatch action event if key is bound to an action
    if (settings_ && jsRuntime_ && document_->body()) {
        dispatchActionEvent(jsRuntime_->getContext(), settings_.get(),
                            document_->body(), keycode, mod, "down");
    }
}

void Engine::handleKeyUp(int keycode, int scancode, int mod, bool repeat) {
    if (!document_) return;

    // Dispatch keyup to the focused input if any, otherwise body
    auto evt = makeKeyboardEvent("keyup", keycode, scancode, mod, repeat);

    auto* activeEl = document_->activeElement();
    bool focusedControl = false;
    if (auto* input = getElInput(activeEl)) focusedControl = input->isFocused();
    if (auto* ta = getElTextarea(activeEl)) focusedControl = focusedControl || ta->isFocused();
    if (getElSelect(activeEl)) focusedControl = true;
    dom::Element* target = focusedControl ? activeEl : document_->body();
    if (target) {
        dispatchEvent(target, evt);
    }

    // Dispatch action event if key is bound to an action
    if (settings_ && jsRuntime_ && document_->body()) {
        dispatchActionEvent(jsRuntime_->getContext(), settings_.get(),
                            document_->body(), keycode, mod, "up");
    }
}

// Filter out control characters (tab, etc.) that shouldn't be inserted as text
static bool isControlChar(const std::string& text) {
    if (text.empty()) return true;
    unsigned char c = static_cast<unsigned char>(text[0]);
    // Allow printable ASCII and multi-byte UTF-8 sequences
    if (text.size() == 1 && c < 0x20 && c != '\n') return true; // control chars except newline
    if (text.size() == 1 && c == 0x7f) return true; // DEL
    return false;
}

void Engine::handleTextInput(const std::string& text) {
    if (!document_) return;

    // Filter control characters for all inputs
    if (isControlChar(text)) return;

    if (overlayMgr_.handleTextInput(text)) {
        uiDirty_ = true;
        return;
    }

    auto* activeEl = document_->activeElement();
    layout::KeyHandleResult result;

    if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
        result = textarea->handleTextInput(activeEl, text);
    } else if (auto* input = getElInput(activeEl); input && input->isFocused()) {
        result = input->handleTextInput(activeEl, text);
    }

    if (result.handled) {
        applyKeyResult(activeEl, result);
    }
}

// ---------------------------------------------------------------------------
// Tab focus navigation
// ---------------------------------------------------------------------------

void Engine::advanceFocus(bool reverse) {
    if (!document_) return;

    // Build list of focusable elements in DOM order
    std::vector<dom::Element*> focusable;
    auto* body = document_->body();
    if (!body) return;

    // Collect all elements via querySelectorAll for common focusable tags
    auto inputs = body->querySelectorAll("input");
    auto textareas = body->querySelectorAll("textarea");
    auto selects = body->querySelectorAll("select");
    auto buttons = body->querySelectorAll("button");

    // Merge into a single list — we need DOM order, so collect all elements
    // and filter. Use a simple recursive walk.
    std::function<void(dom::Node*)> walk = [&](dom::Node* node) {
        if (!node) return;
        if (node->nodeType() == dom::NodeType::Element) {
            auto* el = static_cast<dom::Element*>(node);
            std::string tag = el->tagName();
            for (auto& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool isFocusable = (tag == "input" || tag == "textarea" || tag == "select" || tag == "button");
            if (isFocusable) {
                // Skip hidden inputs
                auto* inp = getElInput(el);
                if (inp && inp->inputType(el) == layout::ElInput::InputType::Hidden)
                    isFocusable = false;
                // Skip disabled
                if (el->getAttribute("disabled") == "true" || el->attributes().count("disabled"))
                    isFocusable = false;
            }
            if (isFocusable) focusable.push_back(el);
        }
        for (auto* child : node->childNodes()) walk(child);
    };
    walk(body);

    if (focusable.empty()) return;

    // Find current active element
    auto* activeEl = document_->activeElement();
    int currentIdx = -1;
    for (int i = 0; i < static_cast<int>(focusable.size()); ++i) {
        if (focusable[i] == activeEl) { currentIdx = i; break; }
    }

    // Compute next index
    int nextIdx;
    if (reverse) {
        nextIdx = (currentIdx <= 0) ? static_cast<int>(focusable.size()) - 1 : currentIdx - 1;
    } else {
        nextIdx = (currentIdx < 0 || currentIdx >= static_cast<int>(focusable.size()) - 1) ? 0 : currentIdx + 1;
    }

    auto* nextEl = focusable[nextIdx];

    // Unfocus current
    if (activeEl) {
        auto* prevInput = getElInput(activeEl);
        if (prevInput) prevInput->setFocused(false);
        auto* prevTa = getElTextarea(activeEl);
        if (prevTa) prevTa->setFocused(false);
        // Tab-advancing away from a <select> dismisses any open dropdown.
        if (getElSelect(activeEl)) overlayMgr_.close();
    }

    // Focus next
    document_->setActiveElement(nextEl);
    dispatchFocusEvents(activeEl, nextEl);

    auto* newInput = getElInput(nextEl);
    auto* newTa = getElTextarea(nextEl);

    if (newInput) {
        newInput->setFocused(true);
        if (newInput->isTextType(nextEl)) {
            std::string v = nextEl->getAttribute("value");
            newInput->setCursorPos(static_cast<int>(v.size()));
            safeStartTextInput(window_.get());
        } else {
            safeStopTextInput(window_.get());
        }
    } else if (newTa) {
        newTa->setFocused(true);
        std::string v = nextEl->getAttribute("value");
        newTa->setCursorPos(static_cast<int>(v.size()));
        safeStartTextInput(window_.get());
    } else {
        safeStopTextInput(window_.get());
    }

    uiDirty_ = true;
}

// ---------------------------------------------------------------------------
// Mouse wheel
// ---------------------------------------------------------------------------

void Engine::handleWheel(float x, float y, float dx, float dy) {
    if (!document_) return;

    if (overlayMgr_.handleWheel(x, y, dx, dy)) {
        uiDirty_ = true;
        return;
    }

    float docX = x, docY = y + scrollY_;
    dom::Element* target = hitTest(docX, docY);

    // Dispatch wheel event to JS
    if (target) {
        dom::WheelEvent wheelEvt("wheel", true, true);
        int mod = safeGetModState(window_.get());
        populateMouseEvent(wheelEvt, x, y, -1, pressedButtons_,
                          x - lastMouseX_, y - lastMouseY_, scrollY_, mod);
        // SDL gives scroll amounts in lines; convert to pixels for DOM_DELTA_PIXEL
        wheelEvt.setDeltaX(static_cast<double>(-dx * inputConfig_.scrollSpeed));
        wheelEvt.setDeltaY(static_cast<double>(-dy * inputConfig_.scrollSpeed));
        wheelEvt.setDeltaZ(0.0);
        wheelEvt.setDeltaMode(dom::WheelEvent::DOM_DELTA_PIXEL);
        computeOffset(wheelEvt, target);
        dispatchEvent(target, wheelEvt);

        // If JS called preventDefault(), don't do default scrolling
        if (wheelEvt.defaultPrevented()) {
            if (jsRuntime_) jsRuntime_->executePendingJobs();
            return;
        }
    }

    // Check if mouse is over a focused textarea
    auto* activeEl = document_->activeElement();
    auto* textarea = getElTextarea(activeEl);
    if (textarea && textarea->isFocused()) {
        float lineH = 16.0f;
        float scroll = textarea->scrollY() - dy * lineH * 3.0f;
        scroll = std::max(scroll, 0.0f);
        textarea->setScrollY(scroll);
        uiDirty_ = true;
        return;
    }

    // Also allow scrolling textarea under mouse cursor (not just active one)
    auto* hoverTa = getElTextarea(target);
    if (hoverTa) {
        float lineH = 16.0f;
        float scroll = hoverTa->scrollY() - dy * lineH * 3.0f;
        scroll = std::max(scroll, 0.0f);
        hoverTa->setScrollY(scroll);
        uiDirty_ = true;
        return;
    }

    // Check if target or an ancestor is a scrollable overflow element
    {
        auto* el = target;
        while (el) {
            std::string ov = getOverflowY(el->computedStyle());
            if (overflowScrollable(ov)) {
                float maxST = maxScrollTop(el);
                if (maxST <= 0) break; // content fits, no scrolling needed
                float scrollPx = -dy * inputConfig_.scrollSpeed;
                float prevScroll = el->scrollTopValue();
                float newScroll = std::clamp(prevScroll + scrollPx, 0.0f, maxST);
                el->setScrollTopValue(newScroll);
                if (newScroll != prevScroll) {
                    dispatchScrollEvent(el);
                }
                uiDirty_ = true;
                return;
            }
            // overflow:hidden means this element isn't scrollable, but wheel
            // events should still propagate to scrollable ancestors (matches
            // browser behavior). Just skip and keep walking up.
            el = composedParent(el);
        }
    }

    // Viewport scrolling
    float maxScroll = std::max(0.0f, documentHeight_ - static_cast<float>(viewportHeight_));
    float prevScroll = scrollY_;
    scrollY_ = std::clamp(scrollY_ - dy * inputConfig_.scrollSpeed, 0.0f, maxScroll);
    if (scrollY_ != prevScroll) {
        // Dispatch scroll event on document element
        if (document_->documentElement()) {
            dispatchScrollEvent(document_->documentElement());
        }
    }
    uiDirty_ = true;
}

// ---------------------------------------------------------------------------
// File/text drop handling
// ---------------------------------------------------------------------------

void Engine::handleDropFile(const std::string& path, float x, float y) {
    if (!document_) return;

    // Use provided coordinates, fall back to last mouse position
    float dropX = (x >= 0) ? x : lastMouseX_;
    float dropY = (y >= 0) ? y : lastMouseY_;
    float docX = dropX, docY = dropY + scrollY_;
    dom::Element* target = hitTest(docX, docY);
    if (!target) target = document_->body();
    if (!target) return;

    // Dispatch dragenter, dragover, then drop
    dom::DragEvent enterEvt("dragenter", true, true);
    enterEvt.addFile(path);
    enterEvt.setIsTrusted(true);
    dispatchEvent(target, enterEvt);

    dom::DragEvent overEvt("dragover", true, true);
    overEvt.addFile(path);
    overEvt.setIsTrusted(true);
    dispatchEvent(target, overEvt);

    dom::DragEvent dropEvt("drop", true, true);
    dropEvt.addFile(path);
    dropEvt.setIsTrusted(true);
    dispatchEvent(target, dropEvt);
}

void Engine::handleDropText(const std::string& text, float x, float y) {
    if (!document_) return;

    float dropX = (x >= 0) ? x : lastMouseX_;
    float dropY = (y >= 0) ? y : lastMouseY_;
    float docX = dropX, docY = dropY + scrollY_;
    dom::Element* target = hitTest(docX, docY);
    if (!target) target = document_->body();
    if (!target) return;

    dom::DragEvent enterEvt("dragenter", true, true);
    enterEvt.setDataText(text);
    enterEvt.setIsTrusted(true);
    dispatchEvent(target, enterEvt);

    dom::DragEvent overEvt("dragover", true, true);
    overEvt.setDataText(text);
    overEvt.setIsTrusted(true);
    dispatchEvent(target, overEvt);

    dom::DragEvent dropEvt("drop", true, true);
    dropEvt.setDataText(text);
    dropEvt.setIsTrusted(true);
    dispatchEvent(target, dropEvt);
}

// ---------------------------------------------------------------------------
// Clipboard simulation (for headless testing)
// ---------------------------------------------------------------------------

void Engine::simulatePaste(const std::string& text) {
    if (!document_) return;

    auto* activeEl = document_->activeElement();
    dom::Element* target = activeEl ? activeEl : document_->body();

    dom::ClipboardEvent pasteEvt("paste", true, true);
    pasteEvt.setClipboardText(text);
    pasteEvt.setIsTrusted(true);
    dispatchEvent(target, pasteEvt);

    // If not prevented, insert into focused input/textarea
    if (!pasteEvt.defaultPrevented() && !text.empty() && activeEl) {
        layout::KeyHandleResult r;
        if (auto* input = getElInput(activeEl); input && input->isFocused()) {
            r = input->handleTextInput(activeEl, text);
        } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
            r = textarea->handleTextInput(activeEl, text);
        }
        if (r.handled) {
            applyKeyResult(activeEl, r);
            uiDirty_ = true;
        }
    }
}

std::string Engine::simulateCopy() {
    if (!document_) return "";

    auto* activeEl = document_->activeElement();
    dom::Element* target = activeEl ? activeEl : document_->body();

    std::string text;
    if (activeEl) {
        if (auto* input = getElInput(activeEl); input && input->isFocused()) {
            text = activeEl->getAttribute("value");
        } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
            text = activeEl->getAttribute("value");
        }
    }

    dom::ClipboardEvent clipEvt("copy", true, true);
    clipEvt.setClipboardText(text);
    clipEvt.setIsTrusted(true);
    dispatchEvent(target, clipEvt);

    return text;
}

std::string Engine::simulateCut() {
    if (!document_) return "";

    auto* activeEl = document_->activeElement();
    dom::Element* target = activeEl ? activeEl : document_->body();

    std::string text;
    if (activeEl) {
        if (auto* input = getElInput(activeEl); input && input->isFocused()) {
            text = activeEl->getAttribute("value");
        } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
            text = activeEl->getAttribute("value");
        }
    }

    dom::ClipboardEvent clipEvt("cut", true, true);
    clipEvt.setClipboardText(text);
    clipEvt.setIsTrusted(true);
    dispatchEvent(target, clipEvt);

    if (!clipEvt.defaultPrevented() && !text.empty() && activeEl) {
        activeEl->setAttribute("value", "");
        if (auto* input = getElInput(activeEl))
            input->setCursorPos(0);
        else if (auto* textarea = getElTextarea(activeEl))
            textarea->setCursorPos(0);

        dom::InputEvent inputEvt("input", true, false);
        inputEvt.setInputType("deleteByCut");
        inputEvt.setIsTrusted(true);
        dispatchEvent(activeEl, inputEvt);
        if (activeEl->document()) activeEl->document()->markDirty();
        uiDirty_ = true;
    }

    return text;
}

} // namespace bro::engine
