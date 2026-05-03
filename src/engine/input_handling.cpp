// Engine input handling methods — split from engine.cpp for readability.
// These are Engine member function implementations, not a separate class.

#include "engine/engine.h"
#include "engine/key_mapping.h"
#include "engine/overflow.h"
#include "engine/overlay.h"
#include "engine/replaced_elements.h"
#include "engine/settings.h"

#include "scene/scene_graph.h"
#include "scene/html_node.h"
#include "layout/layout_node_adapter.h"
#include "layout/box.h"

#include "platform/sdl_window.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/event_dispatch.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "dom/text_node.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/key_handle_result.h"
#include "layout/selection_geometry.h"
#include "layout/skia_text_metrics.h"

// ---------------------------------------------------------------------------
// Contenteditable edit helpers — shared by handleKeyDown (backspace/delete,
// cut/paste) and handleTextInput (typing). Defined here so both sites see
// the template definition.
// ---------------------------------------------------------------------------

// Return true if `node` is inside a contenteditable host (attribute set to
// anything other than "false"). Skips `contenteditable="false"`.
static bool inEditableHost(bro::dom::Node* node) {
    for (bro::dom::Node* n = node; n; n = n->parentNode()) {
        if (n->nodeType() != bro::dom::NodeType::Element) continue;
        auto* el = static_cast<bro::dom::Element*>(n);
        if (!el->hasAttribute("contenteditable")) continue;
        return el->getAttribute("contenteditable") != "false";
    }
    return false;
}

// Delete the content currently covered by the Selection's range, collapsing
// it to the start position. Returns the post-deletion caret (node, offset).
static void deleteRangeContents(bro::dom::Document* doc,
                                bro::dom::Range& r,
                                bro::dom::Node*& node, int& off) {
    node = r.startContainer();
    off = r.startOffset();
    r.deleteContents();
    if (doc) doc->markDirty();
}

// Dispatch beforeinput → optionally perform an edit → dispatch input.
// `runEdit` is the mutation callback; runs only when beforeinput wasn't
// default-prevented. Events fire on the nearest editable Element ancestor.
template <typename EditFn>
static void runEditableMutation(bro::dom::Document* doc,
                                bro::js::Runtime* rt,
                                bro::dom::Node* focusNode,
                                const std::string& inputType,
                                const std::string& data,
                                EditFn&& runEdit) {
    if (!doc || !focusNode) return;
    auto* host = focusNode;
    while (host && host->nodeType() != bro::dom::NodeType::Element)
        host = host->parentNode();
    auto* hostEl = host ? static_cast<bro::dom::Element*>(host) : doc->body();

    bro::dom::InputEvent beforeEvt("beforeinput", /*bubbles=*/true, /*cancelable=*/true);
    beforeEvt.setInputType(inputType);
    beforeEvt.setData(data);
    beforeEvt.setIsTrusted(true);
    if (hostEl && rt) {
        bro::js::dispatchDomEvent(rt->getContext(), hostEl, beforeEvt);
    }
    if (beforeEvt.defaultPrevented()) return;

    runEdit();

    bro::dom::InputEvent inputEvt("input", /*bubbles=*/true, /*cancelable=*/false);
    inputEvt.setInputType(inputType);
    inputEvt.setData(data);
    inputEvt.setIsTrusted(true);
    if (hostEl && rt) {
        bro::js::dispatchDomEvent(rt->getContext(), hostEl, inputEvt);
    }
    doc->markDirty();
}

// Insert `text` at the current Selection. If the selection isn't collapsed,
// deletes the contents first. Caret ends up after the inserted text.
static void selectionInsertText(bro::dom::Document* doc, const std::string& text) {
    if (!doc) return;
    auto* sel = doc->selection();
    if (!sel || sel->rangeCount() == 0) return;
    auto* range = sel->getRangeAt(0);
    if (!range) return;

    bro::dom::Node* caretNode = nullptr;
    int caretOff = 0;
    if (!range->collapsed()) {
        deleteRangeContents(doc, *range, caretNode, caretOff);
    } else {
        caretNode = range->startContainer();
        caretOff = range->startOffset();
    }
    if (!caretNode) return;

    if (caretNode->nodeType() == bro::dom::NodeType::Text) {
        auto* tn = static_cast<bro::dom::TextNode*>(caretNode);
        tn->insertData(caretOff, text);
        int newOff = caretOff + static_cast<int>(text.size());
        sel->collapse(tn, newOff);
    } else if (caretNode->nodeType() == bro::dom::NodeType::Element) {
        auto* el = static_cast<bro::dom::Element*>(caretNode);
        auto* tn = doc->createTextNode(text);
        auto& kids = el->childNodes();
        if (caretOff >= static_cast<int>(kids.size())) {
            el->appendChild(tn);
        } else {
            el->insertBefore(tn, kids[caretOff]);
        }
        sel->collapse(tn, static_cast<int>(text.size()));
    }
}
#include "util/time.h"
#include "util/log.h"
#include "util/platform.h"

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

// Element-relative offset coords live in replaced_elements.cpp as
// applyMouseOffset — shared by app and system paths.

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

// Build a MouseEvent with standard fields populated.
// `x, y` are screen-space; `contentTop` is the engine-reserved top inset
// (menu bar) so clientY/pageY are reported in the web-standard content space.
static void populateMouseEvent(dom::MouseEvent& evt, float x, float y,
                               int button, int buttons,
                               float movementX, float movementY,
                               float scrollY, int mod,
                               float contentTop = 0.0f) {
    float cy = y - contentTop;
    evt.setClientX(static_cast<double>(x));
    evt.setClientY(static_cast<double>(cy));
    evt.setScreenX(static_cast<double>(x));
    evt.setScreenY(static_cast<double>(y));
    evt.setPageX(static_cast<double>(x));
    evt.setPageY(static_cast<double>(cy + scrollY));
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

// findElementScrollbarHit lives in engine/overflow.h so system_panels.cpp
// and the app input handler share one implementation.

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
    float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
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

    // Inspector picker mode: a click in the app viewport selects the hovered
    // element instead of dispatching to the app. Only the primary button picks;
    // other buttons fall through (so right-click context menus etc. still work
    // even when picker is on, though that's an unlikely combo).
    if (inspector_.pickerMode && inspector_.visible && button == 0) {
        dom::Element* hit = inspector_.pickerHover ? inspector_.pickerHover : hitTest(docX, docY);
        if (hit) inspectorPickElement(hit);
        inspectorSetPickerMode(false);
        // Notify the panel UI so it refreshes (selection + button state).
        for (auto& doc : systemDocs_) {
            if (doc.name != "inspector" || !doc.jsCtx) continue;
            JSValue global = JS_GetGlobalObject(doc.jsCtx);
            JSValue fn = JS_GetPropertyStr(doc.jsCtx, global, "__onInspectorChanged");
            if (JS_IsFunction(doc.jsCtx, fn)) {
                JSValue r = JS_Call(doc.jsCtx, fn, global, 0, nullptr);
                JS_FreeValue(doc.jsCtx, r);
            }
            JS_FreeValue(doc.jsCtx, fn);
            JS_FreeValue(doc.jsCtx, global);
        }
        pressedButtons_ |= domButtonMask(button);
        return;
    }

    // Engine-level 3D gizmo — sits between modal UI and DOM. Consumes only
    // when a handle is hit; otherwise falls through to DOM / canvas.
    if (gizmoHandleMouseDown(docX, docY, button)) {
        pressedButtons_ |= domButtonMask(button);
        return;
    }

    // Update button bitmask (DOM convention: 1=left, 2=right, 4=middle, ...)
    pressedButtons_ |= domButtonMask(button);

    // --- Scrollbar interaction (before DOM hit testing) ---

    // Check viewport scrollbar (sits in the content area, below the menu bar)
    {
        float ct = static_cast<float>(contentTop());
        float vh = static_cast<float>(contentHeight());
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            ct, vh, documentHeight_, vh, scrollY_);
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
                          x - lastMouseX_, y - lastMouseY_, scrollY_, mod, static_cast<float>(contentTop()));

        dom::Element* target = hitTest(docX, docY);
        if (target) applyMouseOffset(evt, target);

        // World-space HtmlNode hit test: if the click landed on a canvas
        // that owns a SceneGraph, ray-cast into the scene's HtmlNode
        // billboards. A hit consumes the event — the canvas itself does
        // not see mousedown in that case (parallels how a regular DOM
        // child element captures clicks before its parent).
        scene::HtmlNode* hnHit = nullptr;
        dom::Element* hnEl = nullptr;
        float hnPxX = 0.0f, hnPxY = 0.0f;
        if (target && pickHtmlNodeUnderMouse(target, docX, docY,
                                              hnHit, hnEl, hnPxX, hnPxY)) {
            htmlNodeMouseDownNode_ = hnHit;
            htmlNodeMouseDownElement_ = hnEl;
            dispatchHtmlNodeMouseEvent("mousedown", hnEl, hnPxX, hnPxY,
                                        button, pressedButtons_, mod,
                                        x - lastMouseX_, y - lastMouseY_,
                                        /*bubbles=*/true);
            return;
        }
        htmlNodeMouseDownNode_ = nullptr;
        htmlNodeMouseDownElement_ = nullptr;

        ControlContext cctx{document_.get(), jsRuntime_->getContext(),
                           renderer_.get(), window_.get(), &uiDirty_,
                           &overlayMgr_, OverlayContext::App,
                           viewportWidth_, viewportHeight_};
        dispatchDocMousePress(cctx, appMouseState_, target, evt, x, docY);
        jsRuntime_->executePendingJobs();

        // Mouse-driven text selection. Left button only; bail out if the
        // click landed on a text-editing control (input/textarea manage
        // their own caret via ElInput) or a button-like control.
        if (button == 0 && document_ && textMetrics_) {
            bool isEditableControl = false;
            if (target) {
                const std::string& tag = target->tagName();
                if (tag == "INPUT" || tag == "TEXTAREA" || tag == "SELECT" ||
                    tag == "BUTTON" || tag == "OPTION") {
                    isEditableControl = true;
                }
            }
            if (!isEditableControl) {
                auto hit = layout::hitTestText(document_.get(), docX, docY, *textMetrics_);
                auto* sel = document_->selection();
                // Validate the hit textnode is still owned by the document.
                // Hit-testing can surface layout-cached pointers into detached
                // subtrees; binding a Range to one guarantees a dangling
                // endpoint the instant that subtree is freed.
                if (hit.textNode && document_->ownsNode(hit.textNode)) {
                    int detail = appMouseState_.clickCount;
                    if (detail >= 3) {
                        // Triple-click: select the entire text node.
                        sel->setRange(hit.textNode, 0,
                                      hit.textNode,
                                      static_cast<int>(hit.textNode->length()),
                                      dom::Selection::Forward);
                        selectionDragging_ = false;
                    } else if (detail == 2) {
                        // Double-click: expand to word boundaries in the source
                        // string around the hit offset.
                        const std::string& s = hit.textNode->data();
                        int off = std::max(0, std::min(hit.srcOffset,
                            static_cast<int>(s.size())));
                        auto isWordChar = [](unsigned char c) {
                            return std::isalnum(c) || c == '_';
                        };
                        int lo = off;
                        while (lo > 0 && isWordChar(
                            static_cast<unsigned char>(s[lo - 1]))) lo--;
                        int hi = off;
                        while (hi < static_cast<int>(s.size()) &&
                               isWordChar(static_cast<unsigned char>(s[hi]))) hi++;
                        sel->setRange(hit.textNode, lo, hit.textNode, hi,
                                      dom::Selection::Forward);
                        selectionDragging_ = false;
                    } else {
                        sel->collapse(hit.textNode, hit.srcOffset);
                        selectionAnchorNode_ = hit.textNode;
                        selectionAnchorOffset_ = hit.srcOffset;
                        selectionDragging_ = true;
                        selectionPressX_ = docX;
                        selectionPressY_ = docY;
                        selectionPastThreshold_ = false;
                    }
                    uiDirty_ = true;
                } else {
                    // Click outside any text: clear selection.
                    sel->removeAllRanges();
                    selectionDragging_ = false;
                    selectionAnchorNode_ = nullptr;
                    uiDirty_ = true;
                }
            }
        }
    }
}

void Engine::handleMouseUp(float x, float y, int button) {
    // x, y = screen space. docX, docY = document space (see handleMouseDown).
    float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
    uiDirty_ = true;

    // Match handleMouseDown: SDL -> DOM button index.
    button = sdlToDomButton(button);

    // Update button bitmask (DOM convention)
    pressedButtons_ &= ~domButtonMask(button);

    if (overlayMgr_.handleMouseUp(x, y, button)) {
        uiDirty_ = true;
        return;
    }

    // End scrollbar drags FIRST, before any early-return consumer — the
    // mouseup that ends a drag must always terminate the drag regardless of
    // where the pointer happens to be, otherwise the thumb stays glued to
    // the cursor forever.
    if (viewportScrollbar_.isDragging()) {
        viewportScrollbar_.endDrag();
        draggingViewportScrollbar_ = false;
        uiDirty_ = true;
    }
    if (elementScrollbar_.isDragging()) {
        elementScrollbar_.endDrag();
        scrollbarDragTarget_ = nullptr;
        if (scrollbarDragSystemDoc_) {
            systemDirty_ = true;
            scrollbarDragSystemDoc_ = nullptr;
        }
        uiDirty_ = true;
        // The mouseup that ends a scrollbar drag is purely for drag
        // termination — it shouldn't also be delivered to DOM listeners
        // underneath (where it would look like a random click).
        return;
    }

    // Forward to system overlay first
    if (systemHandleMouseUp(x, y, button)) {
        return;
    }

    // Gizmo consumes mouseUp only when the drag was active.
    if (gizmoHandleMouseUp(docX, docY, button)) {
        return;
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

    if (button == 0) {
        // Terminate any in-progress selection drag regardless of where the
        // pointer released — next mousedown starts fresh.
        selectionDragging_ = false;
    }

    if (document_) {
        dom::Element* target = hitTest(docX, docY);
        int mod = safeGetModState(window_.get());
        float ct = static_cast<float>(contentTop());
        float movX = x - lastMouseX_;
        float movY = y - lastMouseY_;
        float clientY = y - ct;
        float pageY = clientY + scrollY_;

        // Mirror the mousedown HtmlNode routing on release: if the press
        // landed on a HtmlNode and the release ray-casts to the same node,
        // dispatch mouseup + click; otherwise dispatch mouseup only on the
        // press target so handlers see a balanced down/up pair.
        scene::HtmlNode* hnHit = nullptr;
        dom::Element* hnEl = nullptr;
        float hnPxX = 0.0f, hnPxY = 0.0f;
        bool hnReleaseHit = (target && pickHtmlNodeUnderMouse(target, docX, docY,
                                                                hnHit, hnEl, hnPxX, hnPxY));
        if (htmlNodeMouseDownNode_) {
            if (hnReleaseHit && hnHit == htmlNodeMouseDownNode_) {
                dispatchHtmlNodeMouseEvent("mouseup", hnEl, hnPxX, hnPxY,
                                            button, pressedButtons_, mod,
                                            movX, movY, /*bubbles=*/true);
                if (button == 0 && hnEl == htmlNodeMouseDownElement_) {
                    dispatchHtmlNodeMouseEvent("click", hnEl, hnPxX, hnPxY,
                                                button, pressedButtons_, mod,
                                                movX, movY, /*bubbles=*/true);
                }
            } else {
                dispatchHtmlNodeMouseEvent("mouseup", htmlNodeMouseDownElement_,
                                            hnPxX, hnPxY,
                                            button, pressedButtons_, mod,
                                            movX, movY, /*bubbles=*/true);
            }
            htmlNodeMouseDownNode_ = nullptr;
            htmlNodeMouseDownElement_ = nullptr;
            return;
        }

        dom::MouseEvent upEvt("mouseup");
        populateMouseEvent(upEvt, x, y, button, pressedButtons_,
                          movX, movY, scrollY_, mod, ct);
        if (target) applyMouseOffset(upEvt, target);

        ControlContext cctx{document_.get(), jsRuntime_->getContext(),
                           renderer_.get(), window_.get(), &uiDirty_,
                           &overlayMgr_, OverlayContext::App,
                           viewportWidth_, viewportHeight_};
        dispatchDocMouseRelease(cctx, appMouseState_, target, upEvt,
                                x, clientY, button, pressedButtons_, mod,
                                movX, movY, x, pageY,
                                util::currentTimeMs(),
                                inputConfig_.doubleClickThresholdMs,
                                inputConfig_.doubleClickDistancePx);
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
                               pressedButtons_, xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
            applyMouseOffset(moveEvt, lockedElement_);
            dispatchEvent(lockedElement_, moveEvt);
            if (jsRuntime_) jsRuntime_->executePendingJobs();
        }
        return;
    }

    // x, y = screen space. docX, docY = document space (see handleMouseDown).
    float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;

    // Overlay manager sees mousemove first. While an overlay is active,
    // DOM hover is suppressed entirely — otherwise hovering elements under
    // the dropdown/picker would trigger :hover styles and JS handlers.
    bool overlayActive = overlayMgr_.hasActive();
    if (overlayActive && overlayMgr_.handleMouseMove(x, y)) {
        uiDirty_ = true;
    }

    // Forward to system overlay first. When the pointer is inside a visible
    // system panel (menu bar, modal card, modal backdrop), systemHandleMouseMove
    // returns true — consume the event so it doesn't bleed through to the app
    // behind the modal. When it returns false the pointer is outside any
    // system panel and the app handles the move normally. Exception: an
    // in-progress scrollbar drag must keep updating even when the pointer
    // strays outside the panel, so fall through in that case.
    if (isSystemVisible() && !elementScrollbar_.isDragging() &&
        systemHandleMouseMove(x, y)) {
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Inspector picker mode: hit-test the app document, update pickerHover so
    // the box-model overlay redraws on the new element, then suppress the rest
    // of the move (no app hover/JS dispatch while picking).
    if (inspector_.pickerMode && inspector_.visible) {
        dom::Element* hit = hitTest(docX, docY);
        if (hit != inspector_.pickerHover) {
            inspector_.pickerHover = hit;
            uiDirty_ = true;
        }
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Gizmo mousemove — drives hover state always; consumes only while
    // actively dragging (returns true in that case).
    if (gizmoHandleMouseMove(docX, docY)) {
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Mouse-driven text selection: while dragging, extend the selection's
    // focus to follow the pointer. The anchor is whatever was captured on
    // mousedown (selectionAnchor*).
    if (selectionDragging_ && document_ && textMetrics_ && selectionAnchorNode_) {
        // Require the pointer to travel past a small threshold before we begin
        // extending the selection. Without this, sub-pixel jitter during a
        // plain click registers as a drag and snaps the focus to whatever
        // text run hit-testing picks nearest — which inside a tiled/grid
        // layout is often a far-away cell, creating an apparent "selects the
        // whole page" bug from the user's perspective.
        if (!selectionPastThreshold_) {
            const float kThreshold = 4.0f;
            float dx = docX - selectionPressX_;
            float dy = docY - selectionPressY_;
            if (dx*dx + dy*dy < kThreshold * kThreshold) {
                // Still a potential click; leave the caret collapsed at anchor.
            } else {
                selectionPastThreshold_ = true;
            }
        }
        auto hit = selectionPastThreshold_
            ? layout::hitTestText(document_.get(), docX, docY, *textMetrics_)
            : layout::TextHit{};
        // Drop the drag if either endpoint's textnode is no longer live.
        // selectionAnchorNode_ was captured on mousedown and can be freed
        // by app code mid-drag (e.g. HUD rebuilds) even if the hit is fresh.
        if (hit.textNode && document_->ownsNode(hit.textNode) &&
            document_->ownsNode(selectionAnchorNode_)) {
            auto* sel = document_->selection();
            // Compute direction: if focus is before anchor, backward. Use
            // comparePoint against a range collapsed at the anchor — probing
            // with setEnd doesn't work because Range::normalize clamps
            // reversed endpoints instead of swapping them.
            dom::Range probe;
            probe.setStart(selectionAnchorNode_, selectionAnchorOffset_);
            bool backward = probe.comparePoint(hit.textNode, hit.srcOffset) < 0;
            if (backward) {
                sel->setRange(hit.textNode, hit.srcOffset,
                              selectionAnchorNode_, selectionAnchorOffset_,
                              dom::Selection::Backward);
            } else {
                sel->setRange(selectionAnchorNode_, selectionAnchorOffset_,
                              hit.textNode, hit.srcOffset,
                              dom::Selection::Forward);
            }
            uiDirty_ = true;
        }
    }

    // Viewport scrollbar drag
    if (viewportScrollbar_.isDragging()) {
        float ct = static_cast<float>(contentTop());
        float vh = static_cast<float>(contentHeight());
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            ct, vh, documentHeight_, vh, scrollY_);
        float maxScroll = std::max(0.0f, documentHeight_ - vh);
        scrollY_ = std::clamp(
            viewportScrollbar_.updateDrag(y, documentHeight_, vh, m),
            0.0f, maxScroll);
        uiDirty_ = true;
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Element scrollbar drag — works the same whether the target lives in
    // the app doc or a system panel; only the dirty-bit bookkeeping and
    // scroll event dispatch differ.
    if (elementScrollbar_.isDragging() && scrollbarDragTarget_) {
        auto* elem = scrollbarDragTarget_;
        float viewH = elem->layoutBox().contentRect.height;
        float maxST = maxScrollTop(elem);
        float contentH = viewH + maxST;

        auto& lbox = elem->layoutBox();
        float bh = lbox.fullHeight();
        auto m = elementScrollbar_.layout(0, 0, bh, contentH, viewH,
            elem->scrollTopValue());
        float newScroll = elementScrollbar_.updateDrag(y, contentH, viewH, m);
        float prev = elem->scrollTopValue();
        float clamped = std::clamp(newScroll, 0.0f, maxST);
        elem->setScrollTopValue(clamped);
        if (clamped != prev) {
            if (scrollbarDragSystemDoc_) {
                if (scrollbarDragSystemDoc_->document)
                    scrollbarDragSystemDoc_->document->markDirty();
                systemDirty_ = true;
            } else {
                dispatchScrollEvent(elem);
            }
        }
        uiDirty_ = true;
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Viewport scrollbar hover
    {
        float ct = static_cast<float>(contentTop());
        float vh = static_cast<float>(contentHeight());
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            ct, vh, documentHeight_, vh, scrollY_);
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
            float thumbR = layout::ElInput::rangeThumbRadius(dp.h);
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
                                  xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
                outEvt.setRelatedTarget(target);
                applyMouseOffset(outEvt, hoveredElement_);
                dispatchEvent(hoveredElement_, outEvt);
            }

            // mouseleave on previous element (doesn't bubble)
            if (hoveredElement_) {
                dom::MouseEvent leaveEvt("mouseleave", false, false);
                populateMouseEvent(leaveEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
                leaveEvt.setRelatedTarget(target);
                applyMouseOffset(leaveEvt, hoveredElement_);
                dispatchEvent(hoveredElement_, leaveEvt);
            }

            // mouseover on new element (bubbles)
            if (target) {
                dom::MouseEvent overEvt("mouseover", true, true);
                populateMouseEvent(overEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
                overEvt.setRelatedTarget(hoveredElement_);
                applyMouseOffset(overEvt, target);
                dispatchEvent(target, overEvt);
            }

            // mouseenter on new element (doesn't bubble)
            if (target) {
                dom::MouseEvent enterEvt("mouseenter", false, false);
                populateMouseEvent(enterEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
                enterEvt.setRelatedTarget(hoveredElement_);
                applyMouseOffset(enterEvt, target);
                dispatchEvent(target, enterEvt);
            }

            // Mark old and new hovered elements dirty for :hover style re-resolve
            if (hoveredElement_) hoveredElement_->markDirty();
            if (target) target->markDirty();
            hoveredElement_ = target;
            uiDirty_ = true;
        }

        // World-space HtmlNode hover + move routing. Tracked in parallel
        // with hoveredElement_ — the canvas remains the outer-doc hover
        // target while the inner HtmlNode element gets enter/leave/over/
        // out/move events for its own hover state and listeners.
        scene::HtmlNode* hnNode = nullptr;
        dom::Element* hnEl = nullptr;
        float hnPxX = 0.0f, hnPxY = 0.0f;
        bool hnHit = (target && pickHtmlNodeUnderMouse(target, docX, docY,
                                                        hnNode, hnEl, hnPxX, hnPxY));
        if (hnEl != hoveredHtmlElement_) {
            int mod = safeGetModState(window_.get());
            if (hoveredHtmlElement_) {
                dispatchHtmlNodeMouseEvent("mouseout", hoveredHtmlElement_,
                                            hnPxX, hnPxY, -1, pressedButtons_,
                                            mod, xrel, yrel, /*bubbles=*/true,
                                            hnEl);
                dispatchHtmlNodeMouseEvent("mouseleave", hoveredHtmlElement_,
                                            hnPxX, hnPxY, -1, pressedButtons_,
                                            mod, xrel, yrel, /*bubbles=*/false,
                                            hnEl);
                hoveredHtmlElement_->markDirty();
            }
            if (hnEl) {
                dispatchHtmlNodeMouseEvent("mouseover", hnEl,
                                            hnPxX, hnPxY, -1, pressedButtons_,
                                            mod, xrel, yrel, /*bubbles=*/true,
                                            hoveredHtmlElement_);
                dispatchHtmlNodeMouseEvent("mouseenter", hnEl,
                                            hnPxX, hnPxY, -1, pressedButtons_,
                                            mod, xrel, yrel, /*bubbles=*/false,
                                            hoveredHtmlElement_);
                hnEl->markDirty();
            }
            hoveredHtmlElement_ = hnEl;
            hoveredHtmlNode_ = hnNode;
            uiDirty_ = true;
        }

        // Always dispatch mousemove. Route into the inner HtmlNode if the
        // pointer is currently over one — otherwise normal DOM target.
        if (hnHit) {
            int mod = safeGetModState(window_.get());
            dispatchHtmlNodeMouseEvent("mousemove", hnEl, hnPxX, hnPxY,
                                        -1, pressedButtons_, mod,
                                        xrel, yrel, /*bubbles=*/true);
        } else if (target) {
            int mod = safeGetModState(window_.get());
            dom::MouseEvent moveEvt("mousemove", true, true);
            populateMouseEvent(moveEvt, x, y, -1, pressedButtons_,
                              xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
            applyMouseOffset(moveEvt, target);
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

// Call a JS global function with a single boolean argument. Silently no-ops
// if the function isn't defined yet (engine init orders JS wiring vs.
// visibility notifications in ways callers shouldn't have to reason about).
static void callJsBoolFn(JSContext* ctx, const char* name, bool arg) {
    if (!ctx) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, name);
    if (JS_IsFunction(ctx, fn)) {
        JSValue a = JS_NewBool(ctx, arg);
        JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &a);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, a);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
}

void Engine::setPageVisibility(bool visible) {
    if (!jsRuntime_) return;
    callJsBoolFn(jsRuntime_->getContext(), "__bro_set_visibility", visible);
    if (jsRuntime_) jsRuntime_->executePendingJobs();
}

void Engine::setFullscreenState(bool fullscreen) {
    if (!jsRuntime_) return;
    callJsBoolFn(jsRuntime_->getContext(), "__bro_set_fullscreen", fullscreen);
    if (jsRuntime_) jsRuntime_->executePendingJobs();
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

    // Esc cancels inspector picker mode without dismissing the panel itself.
    if (inspector_.pickerMode && keycode == SDLK_ESCAPE && !repeat) {
        inspectorSetPickerMode(false);
        for (auto& doc : systemDocs_) {
            if (doc.name != "inspector" || !doc.jsCtx) continue;
            JSValue global = JS_GetGlobalObject(doc.jsCtx);
            JSValue fn = JS_GetPropertyStr(doc.jsCtx, global, "__onInspectorChanged");
            if (JS_IsFunction(doc.jsCtx, fn)) {
                JSValue r = JS_Call(doc.jsCtx, fn, global, 0, nullptr);
                JS_FreeValue(doc.jsCtx, r);
            }
            JS_FreeValue(doc.jsCtx, fn);
            JS_FreeValue(doc.jsCtx, global);
        }
        uiDirty_ = true;
        return;
    }

    // While the settings modal is open, route keys to its panels first.
    // Modal means modal: app keystrokes are fully suppressed until the modal
    // closes. Esc closes the modal unless a panel listener preventDefaulted
    // the event (e.g. the input panel cancelling a rebind capture). The
    // user's configured system_toggle_settings hotkey also still closes it.
    if (systemSettingsVisible_) {
        bool prevented = systemHandleKeyDown(keycode, scancode, mod, repeat);
        if (!prevented && !repeat) {
            if (keycode == SDLK_ESCAPE) {
                toggleSystemSettings();
                uiDirty_ = true;
            } else if (settings_) {
                std::string webKey = sdlKeycodeToWebKey(keycode, mod);
                if (settings_->getActionForKey(webKey) == "system_toggle_settings") {
                    toggleSystemSettings();
                    uiDirty_ = true;
                }
            }
        }
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

    // Clipboard: ⌘-C / ⌘-X / ⌘-V on macOS, Ctrl equivalents elsewhere.
    if (util::hasPrimaryMod(mod) &&
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
            if (!text.empty()) {
                pasteEvt.addItem({"text/plain", {}, text});
            }
            // Pull any image formats the system has. SDL3 normalizes CF_DIB/CF_DIBV5
            // to "image/bmp" and the Windows CF_PNG format to "image/png" for us.
            for (const char* mime : {"image/png", "image/bmp", "image/jpeg"}) {
                if (!SDL_HasClipboardData(mime)) continue;
                size_t n = 0;
                void* p = SDL_GetClipboardData(mime, &n);
                if (p && n > 0) {
                    auto* bp = static_cast<const uint8_t*>(p);
                    pasteEvt.addItem({mime, std::vector<uint8_t>(bp, bp + n), ""});
                }
                if (p) SDL_free(p);
            }
            pasteEvt.setIsTrusted(true);
            dispatchEvent(target, pasteEvt);

            // If not prevented and no form field consumes it, try inserting
            // into a contenteditable host via the Selection.
            if (!pasteEvt.defaultPrevented() && !text.empty()) {
                bool handledByForm = false;
                if (activeEl) {
                    auto* input = getElInput(activeEl);
                    auto* ta = getElTextarea(activeEl);
                    handledByForm = (input && input->isFocused()) ||
                                    (ta && ta->isFocused());
                }
                if (!handledByForm) {
                    auto* sel = document_->selection();
                    if (sel && sel->rangeCount() > 0) {
                        auto* fn = sel->focusNode();
                        if (fn && inEditableHost(fn)) {
                            runEditableMutation(document_.get(), jsRuntime_.get(), fn,
                                "insertFromPaste", text,
                                [&] { selectionInsertText(document_.get(), text); });
                            uiDirty_ = true;
                        }
                    }
                }
            }
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
            // Copy or Cut: first try the focused input/textarea; if none has
            // a value, fall back to the document's Selection so users can
            // copy text they highlighted with click+drag outside form fields.
            std::string text;
            bool fromFormField = false;
            if (activeEl) {
                if (auto* input = getElInput(activeEl); input && input->isFocused()) {
                    text = activeEl->getAttribute("value");
                    fromFormField = true;
                } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
                    text = activeEl->getAttribute("value");
                    fromFormField = true;
                }
            }
            if (!fromFormField) {
                if (auto* sel = document_->selection(); sel && !sel->isCollapsed()) {
                    text = sel->toString();
                }
            }

            std::string evtType = (keycode == SDLK_C) ? "copy" : "cut";
            dom::ClipboardEvent clipEvt(evtType, true, true);
            clipEvt.setClipboardText(text);
            clipEvt.setIsTrusted(true);
            dispatchEvent(target, clipEvt);

            if (!clipEvt.defaultPrevented() && !text.empty()) {
                SDL_SetClipboardText(text.c_str());

                // Cut: clear the field (form field case)
                if (keycode == SDLK_X && fromFormField && activeEl) {
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
                } else if (keycode == SDLK_X && !fromFormField) {
                    // Cut from a DOM Selection inside contenteditable.
                    auto* sel = document_->selection();
                    if (sel && sel->rangeCount() > 0 && !sel->isCollapsed()) {
                        auto* fn = sel->focusNode();
                        if (fn && inEditableHost(fn)) {
                            runEditableMutation(document_.get(), jsRuntime_.get(), fn,
                                "deleteByCut", "",
                                [&] {
                                    auto* range = sel->getRangeAt(0);
                                    if (!range) return;
                                    dom::Node* after = nullptr; int afterOff = 0;
                                    deleteRangeContents(document_.get(), *range, after, afterOff);
                                    if (after) sel->collapse(after, afterOff);
                                });
                            uiDirty_ = true;
                        }
                    }
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

    // ----------------------------------------------------------------------
    // DOM Selection keyboard navigation. Fires when no form field handled
    // the key. Moves the focus endpoint of the active Selection; Shift
    // extends the selection instead of collapsing it.
    // ----------------------------------------------------------------------
    if (document_) {
        auto* sel = document_->selection();
        if (sel && sel->rangeCount() > 0) {
            bool shift = (mod & SDL_KMOD_SHIFT) != 0;
            bool ctrl = util::hasPrimaryMod(mod);
            bool handled = false;

            auto moveFocus = [&](dom::Node* n, int off) {
                if (shift) {
                    sel->extend(n, off);
                } else {
                    sel->collapse(n, off);
                }
            };

            dom::Node* focusN = sel->focusNode();
            int focusO = sel->focusOffset();
            auto* focusText = (focusN && focusN->nodeType() == dom::NodeType::Text)
                ? static_cast<dom::TextNode*>(focusN) : nullptr;

            // -----------------------------------------------------------
            // Contenteditable editing: Backspace / Delete / Enter, plus
            // cut via Ctrl+X (copy lands in the earlier clipboard block).
            // Only fires when the focus endpoint sits in an editable host.
            // -----------------------------------------------------------
            bool editable = focusN && inEditableHost(focusN);
            if (editable && (keycode == SDLK_BACKSPACE || keycode == SDLK_DELETE)) {
                std::string inputType = (keycode == SDLK_BACKSPACE)
                    ? "deleteContentBackward" : "deleteContentForward";
                runEditableMutation(document_.get(), jsRuntime_.get(), focusN,
                    inputType, "",
                    [&] {
                        auto* range = sel->getRangeAt(0);
                        if (!range) return;
                        if (!range->collapsed()) {
                            dom::Node* after = nullptr; int afterOff = 0;
                            deleteRangeContents(document_.get(), *range, after, afterOff);
                            if (after) sel->collapse(after, afterOff);
                            return;
                        }
                        // Collapsed: delete one character backward/forward
                        // within the current text node when possible.
                        if (focusText) {
                            int len = static_cast<int>(focusText->length());
                            if (keycode == SDLK_BACKSPACE && focusO > 0) {
                                focusText->deleteData(focusO - 1, 1);
                                sel->collapse(focusText, focusO - 1);
                            } else if (keycode == SDLK_DELETE && focusO < len) {
                                focusText->deleteData(focusO, 1);
                                sel->collapse(focusText, focusO);
                            }
                        }
                    });
                handled = true;
            } else if (editable && keycode == SDLK_RETURN) {
                // Enter inserts a <br> element — v1 treats contenteditable
                // as plaintext-only (no block splitting on Enter).
                runEditableMutation(document_.get(), jsRuntime_.get(), focusN,
                    "insertLineBreak", "\n",
                    [&] {
                        auto* range = sel->getRangeAt(0);
                        if (!range) return;
                        if (!range->collapsed()) {
                            dom::Node* after = nullptr; int afterOff = 0;
                            deleteRangeContents(document_.get(), *range, after, afterOff);
                            if (after) sel->collapse(after, afterOff);
                        }
                        auto* br = document_->createElement("BR");
                        range = sel->getRangeAt(0);
                        if (range) range->insertNode(br);
                        // Move caret past the <br>. For a text-node caret,
                        // insertNode splits the text; the caret now sits
                        // immediately after the <br> in its parent.
                        if (br->parentNode()) {
                            auto* p = br->parentNode();
                            const auto& kids = p->childNodes();
                            for (size_t i = 0; i < kids.size(); ++i) {
                                if (kids[i] == br) {
                                    sel->collapse(p, static_cast<int>(i + 1));
                                    break;
                                }
                            }
                        }
                    });
                handled = true;
            } else if (ctrl && keycode == SDLK_A) {
                // Ctrl+A: select all children of the containing contenteditable
                // host, or fall back to the body.
                dom::Node* host = focusN;
                while (host && host->nodeType() != dom::NodeType::Element)
                    host = host->parentNode();
                auto* el = static_cast<dom::Element*>(host);
                while (el && !el->hasAttribute("contenteditable"))
                    el = el->parentElement();
                dom::Node* target = el ? static_cast<dom::Node*>(el)
                                       : static_cast<dom::Node*>(document_->body());
                if (target) sel->selectAllChildren(target);
                handled = true;
            } else if (focusText) {
                const std::string& data = focusText->data();
                int len = static_cast<int>(data.size());
                if (keycode == SDLK_LEFT) {
                    if (focusO > 0) {
                        moveFocus(focusText, focusO - 1);
                        handled = true;
                    }
                } else if (keycode == SDLK_RIGHT) {
                    if (focusO < len) {
                        moveFocus(focusText, focusO + 1);
                        handled = true;
                    }
                } else if (keycode == SDLK_HOME) {
                    moveFocus(focusText, 0);
                    handled = true;
                } else if (keycode == SDLK_END) {
                    moveFocus(focusText, len);
                    handled = true;
                }
            }

            if (handled) {
                uiDirty_ = true;
                auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
                if (focusN && focusN->parentNode()) {
                    auto* parentEl = focusN->nodeType() == dom::NodeType::Element
                        ? static_cast<dom::Element*>(focusN)
                        : focusN->parentNode()->nodeType() == dom::NodeType::Element
                            ? static_cast<dom::Element*>(focusN->parentNode())
                            : document_->body();
                    if (parentEl) dispatchEvent(parentEl, evt);
                }
                return;
            }
        }
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
    if (systemSettingsVisible_) {
        systemHandleKeyUp(keycode, scancode, mod, repeat);
        return;
    }
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
        return;
    }

    // No form-field consumer — maybe the selection is inside a
    // contenteditable host. Fire beforeinput → mutate → input.
    auto* sel = document_->selection();
    if (!sel || sel->rangeCount() == 0) return;
    auto* focusNode = sel->focusNode();
    if (!focusNode || !inEditableHost(focusNode)) return;

    runEditableMutation(document_.get(), jsRuntime_.get(), focusNode,
                        "insertText", text,
                        [&] { selectionInsertText(document_.get(), text); });
    uiDirty_ = true;
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

    // System panels (menu bar, modals) take wheel input first. Scrolls a
    // panel-local overflow box if the pointer is over one, and fully swallows
    // the event while a modal is open so the app behind doesn't scroll.
    if (isSystemVisible() && systemHandleWheel(x, y, dx, dy)) {
        uiDirty_ = true;
        return;
    }

    float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
    dom::Element* target = hitTest(docX, docY);

    // Convert raw SDL wheel delta to pixels once; reuse below for default
    // scroll and the JS wheel event. See util::wheelDeltaToPixels for why
    // this needs to distinguish classic ticks from precise/trackpad input.
    const float pxPerTick = inputConfig_.scrollSpeed;
    const float pxX = util::wheelDeltaToPixels(dx, pxPerTick);
    const float pxY = util::wheelDeltaToPixels(dy, pxPerTick);
    // Vertical-only default scroll: on macOS some trackpad configurations
    // deliver vertical gestures through the X channel. Use the dominant
    // axis so the engine's built-in scrolling matches native app behavior.
    const float pxV = util::wheelDeltaToPixels(
        util::verticalWheelDelta(dx, dy), pxPerTick);

    // Dispatch wheel event to JS
    if (target) {
        dom::WheelEvent wheelEvt("wheel", true, true);
        int mod = safeGetModState(window_.get());
        populateMouseEvent(wheelEvt, x, y, -1, pressedButtons_,
                          x - lastMouseX_, y - lastMouseY_, scrollY_, mod, static_cast<float>(contentTop()));
        // DOM convention: positive deltaY = scroll toward bottom of content.
        // SDL convention: positive wheel.y = scroll up (classic wheel-up).
        // Negate so the JS wheel event matches the browser contract.
        wheelEvt.setDeltaX(static_cast<double>(-pxX));
        wheelEvt.setDeltaY(static_cast<double>(-pxY));
        wheelEvt.setDeltaZ(0.0);
        wheelEvt.setDeltaMode(dom::WheelEvent::DOM_DELTA_PIXEL);
        applyMouseOffset(wheelEvt, target);
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
        float scroll = textarea->scrollY() - pxV;
        scroll = std::max(scroll, 0.0f);
        textarea->setScrollY(scroll);
        uiDirty_ = true;
        return;
    }

    // Also allow scrolling textarea under mouse cursor (not just active one)
    auto* hoverTa = getElTextarea(target);
    if (hoverTa) {
        float scroll = hoverTa->scrollY() - pxV;
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
                float prevScroll = el->scrollTopValue();
                float newScroll = std::clamp(prevScroll - pxV, 0.0f, maxST);
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

    // Viewport scrolling — push into the smoothing residual rather than
    // mutating scrollY_ directly. drainWheelSmoothing() eases it in over
    // the next few frames, which turns irregular macOS momentum events
    // into steady deceleration. The residual is unclamped here; the
    // drain clamps against the live document height each frame (so late
    // re-layouts don't leave us stuck past the bottom).
    wheelResidualY_ -= pxV;
    uiDirty_ = true;
}

void Engine::drainWheelSmoothing(float frameDtSec) {
    if (wheelResidualY_ == 0.0f) return;

    // Exponential ease: each frame apply a fraction of the residual.
    // Higher rate = snappier response / more momentum jitter passing
    // through; lower rate = smoother but floatier. ~60 gives ~63%/frame
    // at 60 fps — settles in 2–3 frames, nearly imperceptible lag on
    // steady swipes while taming irregular momentum tails.
    constexpr float kSmoothRate = 60.0f;
    float t = 1.0f - std::exp(-frameDtSec * kSmoothRate);
    if (t > 1.0f) t = 1.0f;

    // Snap the residual to scrollY_ once it's small enough to avoid
    // infinitely shrinking float tails.
    float apply = wheelResidualY_ * t;
    if (std::abs(wheelResidualY_) < 0.5f) {
        apply = wheelResidualY_;
        wheelResidualY_ = 0.0f;
    } else {
        wheelResidualY_ -= apply;
    }

    if (!document_) { wheelResidualY_ = 0.0f; return; }

    float maxScroll = std::max(0.0f, documentHeight_ - static_cast<float>(contentHeight()));
    float prevScroll = scrollY_;
    scrollY_ = std::clamp(scrollY_ + apply, 0.0f, maxScroll);
    if (scrollY_ == 0.0f || scrollY_ == maxScroll) {
        // Hit an edge — discard remaining residual so we don't fight it.
        wheelResidualY_ = 0.0f;
    }
    if (scrollY_ != prevScroll) {
        if (document_->documentElement()) {
            dispatchScrollEvent(document_->documentElement());
        }
        uiDirty_ = true;
    }
}

// ---------------------------------------------------------------------------
// File/text drop handling
// ---------------------------------------------------------------------------

void Engine::handleDropFile(const std::string& path, float x, float y) {
    if (!document_) return;

    // Use provided coordinates, fall back to last mouse position
    float dropX = (x >= 0) ? x : lastMouseX_;
    float dropY = (y >= 0) ? y : lastMouseY_;
    float docX = dropX, docY = dropY - static_cast<float>(contentTop()) + scrollY_;
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
    float docX = dropX, docY = dropY - static_cast<float>(contentTop()) + scrollY_;
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
    if (!text.empty()) {
        pasteEvt.addItem({"text/plain", {}, text});
    }
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

// ---------------------------------------------------------------------------
// World-space HtmlNode mouse routing
// ---------------------------------------------------------------------------

scene::SceneGraph* Engine::sceneGraphForElement(const dom::Element* el) const {
    if (!el) return nullptr;
    for (auto& sg : sceneGraphs_) {
        if (sg.element == el && sg.graph) return sg.graph.get();
    }
    return nullptr;
}

bool Engine::elementAbsoluteOrigin(dom::Element* el, float& outX, float& outY) const {
    if (!el) return false;
    auto& box = el->layoutBox();
    float ax = box.contentRect.x;
    float ay = box.contentRect.y;
    for (auto* lp = el->layoutParent(); lp; lp = lp->layoutParent()) {
        auto& pb = lp->layoutBox();
        ax += pb.contentRect.x;
        ay += pb.contentRect.y;
        ay -= lp->scrollTopValue();
    }
    outX = ax;
    outY = ay;
    return true;
}

bool Engine::pickHtmlNodeUnderMouse(dom::Element* canvasEl, float docX, float docY,
                                    scene::HtmlNode*& outNode, dom::Element*& outEl,
                                    float& outLocalPxX, float& outLocalPxY) {
    outNode = nullptr;
    outEl = nullptr;
    auto* sg = sceneGraphForElement(canvasEl);
    if (!sg) return false;

    float originX = 0.0f, originY = 0.0f;
    if (!elementAbsoluteOrigin(canvasEl, originX, originY)) return false;
    const float canvasLocalX = docX - originX;
    const float canvasLocalY = docY - originY;

    scene::SceneGraph::HtmlNodePick pick;
    if (!sg->pickHtmlNode(canvasLocalX, canvasLocalY, pick)) return false;
    if (!pick.node) return false;

    auto* doc = pick.node->document();
    if (!doc) return false;
    auto* root = doc->layoutRoot();
    if (!root) return false;

    auto* layoutNode = htmlayout::layout::hitTest(root, pick.localPxX, pick.localPxY);
    auto* hitEl = layout::LayoutNodeAdapter::elementFor(layoutNode);
    if (!hitEl) hitEl = doc->documentElement();
    if (!hitEl) return false;

    outNode = pick.node;
    outEl = hitEl;
    outLocalPxX = pick.localPxX;
    outLocalPxY = pick.localPxY;
    return true;
}

void Engine::dispatchHtmlNodeMouseEvent(const std::string& type,
                                        dom::Element* target,
                                        float localPxX, float localPxY,
                                        int button, int pressedButtons, int mods,
                                        float movX, float movY, bool bubbles,
                                        dom::Element* relatedTarget) {
    if (!target) return;
    dom::MouseEvent evt(type, bubbles, /*cancelable=*/true);
    evt.setIsTrusted(true);
    evt.setClientX(static_cast<double>(localPxX));
    evt.setClientY(static_cast<double>(localPxY));
    evt.setScreenX(static_cast<double>(localPxX));
    evt.setScreenY(static_cast<double>(localPxY));
    evt.setPageX(static_cast<double>(localPxX));
    evt.setPageY(static_cast<double>(localPxY));
    evt.setMovementX(static_cast<double>(movX));
    evt.setMovementY(static_cast<double>(movY));
    evt.setButton(button);
    evt.setButtons(pressedButtons);
    evt.setShiftKey((mods & SDL_KMOD_SHIFT) != 0);
    evt.setCtrlKey ((mods & SDL_KMOD_CTRL ) != 0);
    evt.setAltKey  ((mods & SDL_KMOD_ALT  ) != 0);
    evt.setMetaKey ((mods & SDL_KMOD_GUI  ) != 0);
    if (relatedTarget) evt.setRelatedTarget(relatedTarget);

    // offsetX/Y is the same as clientX/Y here — the inner document's layout
    // origin matches the raster surface origin.
    evt.setOffsetX(static_cast<double>(localPxX));
    evt.setOffsetY(static_cast<double>(localPxY));

    dispatchEvent(target, evt);
    if (jsRuntime_) jsRuntime_->executePendingJobs();
}

} // namespace bro::engine
