// Per-window input routing — the engine side of "a secondary window is a real
// window you can actually use".
//
// Every input event SDL delivers carries a windowID. The main window keeps the
// full pipeline it always had (overlays → system panels → inspector → gizmo →
// scrollbars → app document); an event on a bro.window.open() secondary lands
// here instead and is dispatched against THAT window's isolated document,
// realm and input state. Nothing crosses over: a click in a palette window can
// never focus an element in the main app, and the main window's :hover, click
// streak, cursor and IME state are untouched by it.
//
// Coordinate spaces: a host window has no engine chrome. There is no menu-bar
// inset to fold out of the mouse y and no viewport scroll to add, so window
// space == content space == document space, and every event coordinate here is
// all three at once. (Contrast handleMouseDown's three-space dance.) Host
// surfaces are window-size units in v1, so there is no HiDPI factor to undo
// either.
//
// Main-window chrome is deliberately NOT consulted for hosts: overlays, system
// panels, the inspector, the gizmo and the viewport scrollbar are all primary-
// window furniture, and the plan's v1 cut keeps pointer lock, touch and the
// gamepad/"action" stream on the main window too.
//
// Threading: these run on the main thread from the event loop or a headless
// injection seam, i.e. outside the frame's record/replay window, so mutating a
// host document here never races the raster worker.

#include "engine/engine.h"
#include "engine/input_common.h"
#include "engine/key_mapping.h"
#include "engine/overflow.h"
#include "engine/replaced_elements.h"
#include "engine/settings.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "js/event_dispatch.h"
#include "js/runtime.h"
#include "layout/el_input.h"
#include "layout/el_select.h"
#include "layout/el_textarea.h"
#include "layout/key_handle_result.h"
#include "layout/layout_node_adapter.h"
#include "platform/sdl_window.h"
#include "util/log.h"
#include "util/platform.h"
#include "util/time.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <functional>

namespace bro::engine {

namespace {
// Reported when a caller asks for the cursor of a window that no longer
// exists. Returned by reference, so it has to outlive the call.
const std::string kDefaultCursor = "default";
}  // namespace

// ---------------------------------------------------------------------------
// Small shared pieces
// ---------------------------------------------------------------------------

dom::Element* Engine::windowHostHitTest(WindowHost& h, float x, float y) {
    if (!h.document) return nullptr;
    auto* root = h.document->layoutRoot();
    if (!root) return nullptr;
    auto* node = htmlayout::layout::hitTest(root, x, y);
    auto* hit = layout::LayoutNodeAdapter::elementFor(node);
    // The documentElement itself is "no target", matching iframeHitTest: a
    // press on empty page background focuses nothing.
    if (!hit || hit == h.document->documentElement()) return nullptr;
    return hit;
}

ControlContext Engine::windowHostControlContext(WindowHost& h) {
    // overlays = nullptr: a <select> or <input type=color> in a secondary
    // window focuses and takes keys, but does not open a popup — the overlay
    // manager draws into the MAIN window's compositor pass, so an overlay
    // opened from here would appear on the wrong window. focusNewControl
    // guards every overlay use on ctx.overlays, so this degrades cleanly.
    // window = the host's OWN window, which is what routes SDL text input
    // (and therefore the IME) to the window the user is typing in.
    return ControlContext{h.document.get(), h.jsCtx, renderer_.get(),
                          h.window.get(), &uiDirty_, /*overlays=*/nullptr,
                          OverlayContext::App, h.boxW, h.boxH};
}

void Engine::windowHostDispatch(WindowHost& h, dom::Element* el, dom::Event& evt) {
    if (!el || !h.jsCtx) return;
    js::dispatchDomEvent(h.jsCtx, el, evt);
}

void Engine::windowHostDispatchInput(WindowHost& h, dom::Element* el,
                                     const std::string& data,
                                     const std::string& inputType,
                                     bool isComposing) {
    if (!el) return;
    dom::InputEvent evt("input");
    evt.setData(data);
    evt.setInputType(inputType);
    evt.setIsComposing(isComposing);
    evt.setIsTrusted(true);
    windowHostDispatch(h, el, evt);
    windowHostRepaint(h);
}

void Engine::windowHostDispatchComposition(WindowHost& h, dom::Element* el,
                                           const char* type,
                                           const std::string& data) {
    if (!el) return;
    dom::CompositionEvent evt(type, true, false);
    evt.setData(data);
    evt.setIsTrusted(true);
    windowHostDispatch(h, el, evt);
}

void Engine::windowHostDispatchFocus(WindowHost& h, dom::Element* oldEl,
                                     dom::Element* newEl) {
    if (oldEl == newEl) return;
    // Same four-event order as the app document's dispatchFocusEvents:
    // blur → focus → focusout → focusin.
    if (oldEl) {
        dom::FocusEvent evt("blur", false, false);
        evt.setRelatedTarget(newEl);
        evt.setIsTrusted(true);
        windowHostDispatch(h, oldEl, evt);
    }
    if (newEl) {
        dom::FocusEvent evt("focus", false, false);
        evt.setRelatedTarget(oldEl);
        evt.setIsTrusted(true);
        windowHostDispatch(h, newEl, evt);
    }
    if (oldEl) {
        dom::FocusEvent evt("focusout", true, false);
        evt.setRelatedTarget(newEl);
        evt.setIsTrusted(true);
        windowHostDispatch(h, oldEl, evt);
    }
    if (newEl) {
        dom::FocusEvent evt("focusin", true, false);
        evt.setRelatedTarget(oldEl);
        evt.setIsTrusted(true);
        windowHostDispatch(h, newEl, evt);
    }
}

// A host document only re-records when tickSubDoc says it is dirty, and a
// caret move or a focus ring change mutates no DOM at all. markDirty() here is
// the host's equivalent of markAppBaseDirty(): it is what makes the new caret
// actually appear on screen (and in the next capture()).
void Engine::windowHostRepaint(WindowHost& h) {
    if (h.document) h.document->markDirty();
    uiDirty_ = true;
}

void Engine::windowHostApplyKeyResult(WindowHost& h, dom::Element* el,
                                      const layout::KeyHandleResult& r) {
    if (r.dispatchChange) {
        dom::Event changeEvt("change");
        changeEvt.setIsTrusted(true);
        windowHostDispatch(h, el, changeEvt);
    }
    if (r.dispatchInput) windowHostDispatchInput(h, el, r.inputData, r.inputType);
    if (r.unfocus) {
        windowHostDispatchFocus(h, el, nullptr);
        if (h.document) h.document->setActiveElement(nullptr);
        h.activeElement = nullptr;
        safeStopTextInput(h.window.get());
    }
    if (r.handled) {
        windowHostRepaint(h);
        windowHostUpdateTextInputArea(h);
    }
}

const std::string& Engine::resolvedCursor(uint64_t hostId) const {
    if (hostId == 0) return resolvedCursor_;
    for (auto& h : windowHosts_)
        if (h->id == hostId) return h->resolvedCursor;
    return kDefaultCursor;
}

void Engine::windowHostUpdateCursor(WindowHost& h, dom::Element* target) {
    std::string css;
    if (target) {
        const auto& cs = target->computedStyle();
        auto it = cs.find("cursor");
        if (it != cs.end()) css = it->second;
    }
    platform::CursorShape shape = cursorShapeFromCss(css);
    h.resolvedCursor = cursorShapeName(shape);
    // Window::setCursor caches the last shape PER WINDOW (sdl_window.h), so
    // setting the host's cursor cannot disturb the main window's, and a
    // repeated shape costs nothing. Pointer lock is main-window-only in v1, so
    // there is no relative-mouse-mode case to skip here.
    if (displayMode_ == DisplayMode::Windowed && h.window)
        h.window->setCursor(shape);
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void Engine::hostMouseDown(uint64_t hostId, float x, float y, int sdlButton) {
    WindowHost* hp = windowHostById(hostId);
    if (!hp || !hp->document || !hp->jsCtx) return;
    WindowHost& h = *hp;

    const float prevX = h.lastMouseX, prevY = h.lastMouseY;
    h.lastMouseX = x;
    h.lastMouseY = y;
    const int button = sdlToDomButton(sdlButton);
    h.pressedButtons |= domButtonMask(button);

    // A press moves the caret or the focus; either way an in-progress
    // composition in THIS window commits first (browser behaviour).
    windowHostCommitComposition(h);

    dom::Element* target = windowHostHitTest(h, x, y);
    const int mod = currentModState();

    dom::MouseEvent evt("mousedown");
    populateMouseEvent(evt, x, y, button, h.pressedButtons,
                       x - prevX, y - prevY, /*scrollY=*/0.0f, mod,
                       /*contentTop=*/0.0f);
    if (target) applyMouseOffset(evt, target);

    PressIntent intent;
    intent.ordinal = pressOrdinal(h.mouseState, target, x, y,
                                  util::currentTimeMs(),
                                  inputConfig_.doubleClickThresholdMs,
                                  inputConfig_.doubleClickDistancePx);
    intent.extend = (mod & SDL_KMOD_SHIFT) != 0;

    // Drag-selection inside this window's text controls. Per-window, so a drag
    // in a palette window cannot extend a selection in the main app.
    h.controlDragElement.reset();
    if (button == 0 && isCaretControl(target))
        h.controlDragElement.assign(h.document.get(), target);

    ControlContext cctx = windowHostControlContext(h);
    dispatchDocMousePress(cctx, h.mouseState, target, evt, x, y, intent);
    // dispatchDocMousePress already moved document->activeElement; mirror it
    // so the keyboard path can read the focus without re-deriving it.
    h.activeElement = h.document ? h.document->activeElement() : nullptr;
    if (jsRuntime_) jsRuntime_->executePendingJobs();
    windowHostUpdateTextInputArea(h);
    windowHostRepaint(h);
}

void Engine::hostMouseUp(uint64_t hostId, float x, float y, int sdlButton) {
    WindowHost* hp = windowHostById(hostId);
    if (!hp || !hp->document || !hp->jsCtx) return;
    WindowHost& h = *hp;

    const float prevX = h.lastMouseX, prevY = h.lastMouseY;
    h.lastMouseX = x;
    h.lastMouseY = y;
    const int button = sdlToDomButton(sdlButton);
    h.pressedButtons &= ~domButtonMask(button);
    if (button == 0) h.controlDragElement.reset();

    dom::Element* target = windowHostHitTest(h, x, y);
    const int mod = currentModState();

    // A release over a range slider ends its drag (the app document does the
    // same in handleMouseUp).
    if (auto* input = getElInput(h.document->activeElement());
        input && input->isDragging()) {
        input->setDragging(false);
        dom::Event changeEvt("change");
        changeEvt.setIsTrusted(true);
        windowHostDispatch(h, h.document->activeElement(), changeEvt);
    }

    dom::MouseEvent upEvt("mouseup");
    populateMouseEvent(upEvt, x, y, button, h.pressedButtons,
                       x - prevX, y - prevY, /*scrollY=*/0.0f, mod,
                       /*contentTop=*/0.0f);
    if (target) applyMouseOffset(upEvt, target);

    ControlContext cctx = windowHostControlContext(h);
    // Follows up with click / dblclick / contextmenu against this window's own
    // click streak.
    dispatchDocMouseRelease(cctx, h.mouseState, target, upEvt,
                            x, y, button, h.pressedButtons, mod,
                            x - prevX, y - prevY, x, y,
                            util::currentTimeMs(),
                            inputConfig_.doubleClickThresholdMs,
                            inputConfig_.doubleClickDistancePx);
    h.activeElement = h.document ? h.document->activeElement() : nullptr;
    if (jsRuntime_) jsRuntime_->executePendingJobs();
    windowHostRepaint(h);
}

void Engine::hostMouseMove(uint64_t hostId, float x, float y,
                           float xrel, float yrel) {
    WindowHost* hp = windowHostById(hostId);
    if (!hp || !hp->document || !hp->jsCtx) return;
    WindowHost& h = *hp;

    const int mod = currentModState();

    // Drag-selection inside a host text control: extend the control's own
    // selection to the pointer. Holds the pointer the way a scrollbar drag
    // does, so it runs before ordinary hover/move handling.
    if (auto* dragEl = h.controlDragElement.get()) {
        if (auto* input = getElInput(dragEl)) {
            input->caretToPoint(x, y, /*extend=*/true);
        } else if (auto* ta = getElTextarea(dragEl)) {
            ta->caretToPoint(x, y, /*extend=*/true);
        }
        h.lastMouseX = x;
        h.lastMouseY = y;
        windowHostRepaint(h);
        return;
    }

    dom::Element* target = windowHostHitTest(h, x, y);
    dom::Element* prevHover = h.hoveredElement;

    if (target != prevHover) {
        auto fire = [&](const char* type, dom::Element* el, bool bubbles,
                        dom::Element* related) {
            if (!el) return;
            dom::MouseEvent evt(type, bubbles, bubbles);
            populateMouseEvent(evt, x, y, -1, h.pressedButtons, xrel, yrel,
                               0.0f, mod, 0.0f);
            evt.setRelatedTarget(related);
            applyMouseOffset(evt, el);
            windowHostDispatch(h, el, evt);
        };
        fire("mouseout", prevHover, true, target);
        fire("mouseleave", prevHover, false, target);
        fire("mouseover", target, true, prevHover);
        fire("mouseenter", target, false, prevHover);

        // :hover restyle. recordSubDoc points ElementRefAdapter at
        // h.hoveredElement before resolving this document, so the pseudo-class
        // resolves per window with no cross-talk. Both endpoints are marked so
        // the leaving element loses its highlight too. The JS above can free
        // either element, so re-read nothing but what we just dispatched to —
        // hoveredElement is a raw pointer like IframeDoc's, refreshed here.
        if (prevHover) prevHover->markDirty();
        if (target) target->markDirty();
        h.hoveredElement = target;
        windowHostRepaint(h);
    }

    // Per-window CSS cursor. Re-resolved on every move, not just hover
    // changes: a restyle can change the computed cursor with the hit target
    // unchanged.
    windowHostUpdateCursor(h, h.hoveredElement);

    if (target) {
        dom::MouseEvent moveEvt("mousemove", true, true);
        populateMouseEvent(moveEvt, x, y, -1, h.pressedButtons, xrel, yrel,
                           0.0f, mod, 0.0f);
        applyMouseOffset(moveEvt, target);
        windowHostDispatch(h, target, moveEvt);
    }
    if (jsRuntime_) jsRuntime_->executePendingJobs();

    h.lastMouseX = x;
    h.lastMouseY = y;
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

void Engine::windowHostAdvanceFocus(WindowHost& h, bool reverse) {
    if (!h.document) return;
    windowHostCommitComposition(h);

    auto* body = h.document->body();
    if (!body) return;

    std::vector<dom::Element*> focusable;
    std::function<void(dom::Node*)> walk = [&](dom::Node* node) {
        if (!node) return;
        if (node->nodeType() == dom::NodeType::Element) {
            auto* el = static_cast<dom::Element*>(node);
            std::string tag = el->tagName();
            for (auto& c : tag)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool ok = (tag == "input" || tag == "textarea" || tag == "select" ||
                       tag == "button");
            if (ok) {
                auto* inp = getElInput(el);
                if (inp && inp->inputType(el) == layout::ElInput::InputType::Hidden)
                    ok = false;
                if (el->attributes().count("disabled")) ok = false;
            }
            if (ok) focusable.push_back(el);
        }
        for (auto* child : node->childNodes()) walk(child);
    };
    walk(body);
    if (focusable.empty()) return;

    auto* activeEl = h.document->activeElement();
    int currentIdx = -1;
    for (int i = 0; i < static_cast<int>(focusable.size()); ++i)
        if (focusable[static_cast<size_t>(i)] == activeEl) { currentIdx = i; break; }

    const int last = static_cast<int>(focusable.size()) - 1;
    int nextIdx = reverse ? ((currentIdx <= 0) ? last : currentIdx - 1)
                          : ((currentIdx < 0 || currentIdx >= last) ? 0 : currentIdx + 1);
    auto* nextEl = focusable[static_cast<size_t>(nextIdx)];

    if (auto* prevInput = getElInput(activeEl)) prevInput->setFocused(false);
    if (auto* prevTa = getElTextarea(activeEl)) prevTa->setFocused(false);

    h.document->setActiveElement(nextEl);
    h.activeElement = nextEl;
    windowHostDispatchFocus(h, activeEl, nextEl);

    auto* newInput = getElInput(nextEl);
    auto* newTa = getElTextarea(nextEl);
    if (newInput) {
        newInput->setFocused(true);
        if (newInput->isTextType(nextEl)) {
            std::string v = nextEl->getAttribute("value");
            newInput->setCursorPos(static_cast<int>(v.size()));
            safeStartTextInput(h.window.get());
        } else {
            safeStopTextInput(h.window.get());
        }
    } else if (newTa) {
        newTa->setFocused(true);
        std::string v = nextEl->getAttribute("value");
        newTa->setCursorPos(static_cast<int>(v.size()));
        safeStartTextInput(h.window.get());
    } else {
        safeStopTextInput(h.window.get());
    }

    windowHostUpdateTextInputArea(h);
    windowHostRepaint(h);
    if (jsRuntime_) jsRuntime_->executePendingJobs();
}

void Engine::hostKeyDown(uint64_t hostId, int keycode, int scancode, int mod,
                         bool repeat) {
    WindowHost* hp = windowHostById(hostId);
    if (!hp || !hp->document || !hp->jsCtx) return;
    WindowHost& h = *hp;

    // The physical keyboard is one device shared by every window: keep the
    // modifier/held-key bookkeeping that feeds simulated modifier state and
    // polled action queries current no matter which window has focus.
    heldModifierMask_ |= modifierBitForKeycode(keycode);
    heldKeys_[keycode] = sdlKeycodeToWebKey(keycode, mod);

    // System hotkeys stay GLOBAL — F8 (or whatever the user bound) toggles the
    // perf/settings panels while a secondary window has focus, exactly as it
    // does from the main one. The panels themselves render on the main window.
    if (handleGlobalHotkey(keycode, mod, repeat)) return;

    // A caret-moving or command key commits an in-progress composition first.
    if (util::hasPrimaryMod(mod) ||
        keycode == SDLK_LEFT || keycode == SDLK_RIGHT ||
        keycode == SDLK_UP || keycode == SDLK_DOWN ||
        keycode == SDLK_HOME || keycode == SDLK_END ||
        keycode == SDLK_BACKSPACE || keycode == SDLK_DELETE ||
        keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER ||
        keycode == SDLK_TAB || keycode == SDLK_ESCAPE) {
        windowHostCommitComposition(h);
    }

    // Tab: dispatch first, advance focus within THIS document unless prevented.
    if (keycode == SDLK_TAB) {
        auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
        dom::Element* target = h.document->activeElement();
        if (!target) target = h.document->body();
        if (target) windowHostDispatch(h, target, evt);
        if (!evt.defaultPrevented()) windowHostAdvanceFocus(h, (mod & SDL_KMOD_SHIFT) != 0);
        if (jsRuntime_) jsRuntime_->executePendingJobs();
        return;
    }

    // Delegate to this window's focused control.
    auto* activeEl = h.document->activeElement();
    layout::KeyHandleResult result;
    if (auto* input = getElInput(activeEl); input && input->isFocused()) {
        result = input->handleKeyDown(activeEl, keycode, mod);
    } else if (auto* ta = getElTextarea(activeEl); ta && ta->isFocused()) {
        result = ta->handleKeyDown(activeEl, keycode, mod);
    }
    if (result.handled) {
        windowHostApplyKeyResult(h, activeEl, result);
        auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
        windowHostDispatch(h, activeEl, evt);
        if (jsRuntime_) jsRuntime_->executePendingJobs();
        return;
    }

    // Default: the focused element, else body.
    auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
    dom::Element* target = activeEl ? activeEl : h.document->body();
    if (target) windowHostDispatch(h, target, evt);
    if (jsRuntime_) jsRuntime_->executePendingJobs();
    // NOTE: "action" events (bro.settings key bindings) are deliberately NOT
    // fired from a host window. They are app-input semantics bound to the main
    // app realm (v1 cut, like gamepad) — firing them here would turn typing a
    // "w" into a palette window's text field into a movement action.
}

void Engine::hostKeyUp(uint64_t hostId, int keycode, int scancode, int mod,
                       bool repeat) {
    WindowHost* hp = windowHostById(hostId);
    if (!hp || !hp->document || !hp->jsCtx) return;
    WindowHost& h = *hp;

    heldModifierMask_ &= ~modifierBitForKeycode(keycode);
    heldKeys_.erase(keycode);

    auto evt = makeKeyboardEvent("keyup", keycode, scancode, mod, repeat);
    auto* activeEl = h.document->activeElement();
    bool focusedControl = false;
    if (auto* input = getElInput(activeEl)) focusedControl = input->isFocused();
    if (auto* ta = getElTextarea(activeEl)) focusedControl = focusedControl || ta->isFocused();
    if (getElSelect(activeEl)) focusedControl = true;
    dom::Element* target = focusedControl ? activeEl : h.document->body();
    if (target) windowHostDispatch(h, target, evt);
    if (jsRuntime_) jsRuntime_->executePendingJobs();
}

void Engine::hostTextInput(uint64_t hostId, const std::string& text) {
    WindowHost* hp = windowHostById(hostId);
    if (!hp || !hp->document || !hp->jsCtx) return;
    WindowHost& h = *hp;
    if (isControlChar(text)) return;

    auto* activeEl = h.document->activeElement();

    // A TEXT_INPUT arriving mid-composition is the IME commit: replace the
    // preedit with the committed text and close the composition with Chrome's
    // observable order (compositionupdate → input → compositionend).
    {
        layout::KeyHandleResult commit;
        if (auto* ta = getElTextarea(activeEl);
            ta && ta->isFocused() && ta->isComposing()) {
            commit = ta->compositionCommit(activeEl, text);
        } else if (auto* input = getElInput(activeEl);
                   input && input->isFocused() && input->isComposing()) {
            commit = input->compositionCommit(activeEl, text);
        }
        if (commit.handled) {
            windowHostDispatchComposition(h, activeEl, "compositionupdate", text);
            windowHostDispatchInput(h, activeEl, text, "insertCompositionText", true);
            windowHostDispatchComposition(h, activeEl, "compositionend", text);
            windowHostRepaint(h);
            windowHostUpdateTextInputArea(h);
            if (jsRuntime_) jsRuntime_->executePendingJobs();
            return;
        }
    }

    layout::KeyHandleResult result;
    if (auto* ta = getElTextarea(activeEl); ta && ta->isFocused()) {
        result = ta->handleTextInput(activeEl, text);
    } else if (auto* input = getElInput(activeEl); input && input->isFocused()) {
        result = input->handleTextInput(activeEl, text);
    }
    if (result.handled) windowHostApplyKeyResult(h, activeEl, result);
    // v1 cut: contenteditable editing (and its IME composition) stays app-
    // document-only. A contenteditable host in a secondary window still gets
    // keydown/keyup/textinput DOM events; the engine just doesn't splice text
    // into it. See docs/window-api.js.
    if (jsRuntime_) jsRuntime_->executePendingJobs();
}

// ---------------------------------------------------------------------------
// IME
// ---------------------------------------------------------------------------

void Engine::windowHostCommitComposition(WindowHost& h) {
    if (!h.document) return;
    auto* activeEl = h.document->activeElement();
    layout::KeyHandleResult r;
    std::string data;
    if (auto* input = getElInput(activeEl);
        input && input->isFocused() && input->isComposing()) {
        data = input->compositionText();
        r = input->compositionCommit(activeEl, data);
    } else if (auto* ta = getElTextarea(activeEl);
               ta && ta->isFocused() && ta->isComposing()) {
        data = ta->compositionText();
        r = ta->compositionCommit(activeEl, data);
    } else {
        return;
    }
    if (!r.handled) return;
    windowHostDispatchComposition(h, activeEl, "compositionupdate", data);
    windowHostDispatchInput(h, activeEl, data, "insertCompositionText", true);
    windowHostDispatchComposition(h, activeEl, "compositionend", data);
    windowHostRepaint(h);
    windowHostUpdateTextInputArea(h);
}

void Engine::hostTextEditing(uint64_t hostId, const std::string& text,
                             int start, int /*length*/) {
    WindowHost* hp = windowHostById(hostId);
    if (!hp || !hp->document || !hp->jsCtx) return;
    WindowHost& h = *hp;

    auto* activeEl = h.document->activeElement();
    auto* input = getElInput(activeEl);
    auto* ta = getElTextarea(activeEl);
    const bool inputOk = input && input->isFocused();
    const bool taOk = !inputOk && ta && ta->isFocused();
    // v1: only text controls compose in a secondary window (contenteditable
    // composition is app-document-only).
    if (!inputOk && !taOk) return;

    const bool wasComposing = inputOk ? input->isComposing() : ta->isComposing();

    if (text.empty()) {
        // Empty editing event: the composition ended without a commit.
        if (!wasComposing) return;
        layout::KeyHandleResult r = inputOk ? input->compositionCancel(activeEl)
                                            : ta->compositionCancel(activeEl);
        if (!r.handled) return;
        windowHostDispatchComposition(h, activeEl, "compositionupdate", "");
        windowHostDispatchInput(h, activeEl, "", "insertCompositionText", true);
        windowHostDispatchComposition(h, activeEl, "compositionend", "");
        windowHostRepaint(h);
        windowHostUpdateTextInputArea(h);
        if (jsRuntime_) jsRuntime_->executePendingJobs();
        return;
    }

    // compositionstart.data is the text the composition replaces.
    std::string replacedSel;
    if (!wasComposing)
        replacedSel = inputOk ? input->selectedText() : ta->selectedText();

    layout::KeyHandleResult r = inputOk
        ? input->compositionUpdate(activeEl, text, start)
        : ta->compositionUpdate(activeEl, text, start);
    if (!r.handled) return;

    if (!wasComposing)
        windowHostDispatchComposition(h, activeEl, "compositionstart", replacedSel);
    windowHostDispatchComposition(h, activeEl, "compositionupdate", text);
    windowHostDispatchInput(h, activeEl, text, "insertCompositionText", true);
    windowHostRepaint(h);
    windowHostUpdateTextInputArea(h);
    if (jsRuntime_) jsRuntime_->executePendingJobs();
}

void Engine::windowHostUpdateTextInputArea(WindowHost& h) {
    if (!h.window || !h.document) return;
    auto* activeEl = h.document->activeElement();
    float x = 0, y = 0, w = 0, ht = 0;
    bool have = false;
    if (auto* input = getElInput(activeEl);
        input && input->isFocused() && input->isTextType(activeEl)) {
        have = input->caretRect(x, y, w, ht);
    } else if (auto* ta = getElTextarea(activeEl); ta && ta->isFocused()) {
        have = ta->caretRect(x, y, w, ht);
    }
    if (!have) return;
    // No engine inset to fold back in: a host window's control-draw space IS
    // its window space.
    SDL_Rect rect;
    rect.x = static_cast<int>(std::lround(x));
    rect.y = static_cast<int>(std::lround(y));
    rect.w = static_cast<int>(std::lround(std::max(1.0f, w)));
    rect.h = static_cast<int>(std::lround(std::max(1.0f, ht)));
    SDL_SetTextInputArea(h.window->getSDLWindow(), &rect, 0);
}

// ---------------------------------------------------------------------------
// Wheel
// ---------------------------------------------------------------------------

void Engine::hostWheel(uint64_t hostId, float x, float y, float dx, float dy) {
    WindowHost* hp = windowHostById(hostId);
    if (!hp || !hp->document || !hp->jsCtx) return;
    WindowHost& h = *hp;

    dom::Element* target = windowHostHitTest(h, x, y);

    const float pxPerTick = inputConfig_.scrollSpeed;
    const float pxX = util::wheelDeltaToPixels(dx, pxPerTick);
    const float pxY = util::wheelDeltaToPixels(dy, pxPerTick);
    const float pxV = util::wheelDeltaToPixels(util::verticalWheelDelta(dx, dy),
                                               pxPerTick);

    if (target) {
        dom::WheelEvent wheelEvt("wheel", true, true);
        populateMouseEvent(wheelEvt, x, y, -1, h.pressedButtons, 0.0f, 0.0f,
                           0.0f, currentModState(), 0.0f);
        // SDL's positive wheel.y is "scroll up"; the DOM's positive deltaY is
        // "toward the bottom of the content". Negate, as handleWheel does.
        wheelEvt.setDeltaX(static_cast<double>(-pxX));
        wheelEvt.setDeltaY(static_cast<double>(-pxY));
        wheelEvt.setDeltaZ(0.0);
        wheelEvt.setDeltaMode(dom::WheelEvent::DOM_DELTA_PIXEL);
        applyMouseOffset(wheelEvt, target);
        windowHostDispatch(h, target, wheelEvt);
        if (wheelEvt.defaultPrevented()) {
            if (jsRuntime_) jsRuntime_->executePendingJobs();
            return;
        }
    }

    // Default scroll: the nearest scrollable overflow ancestor, with the same
    // browser-style chaining the app document uses (fall through to the next
    // scroller when this one is pinned at that edge). There is NO engine
    // viewport scrollbar for a secondary window in v1 — a host document that
    // overflows its window scrolls only through its own overflow boxes.
    for (auto* el = target; el; el = composedParent(el)) {
        if (!overflowScrollable(getOverflowY(el->computedStyle()))) continue;
        const float maxST = maxScrollTop(el);
        if (maxST <= 0.0f) continue;
        const float prevScroll = el->scrollTopValue();
        const bool canScroll = (pxV > 0.0f) ? (prevScroll > 0.5f)
                                            : (prevScroll < maxST - 0.5f);
        if (!canScroll) continue;
        const float next = std::clamp(prevScroll - pxV, 0.0f, maxST);
        el->setScrollTopValue(next);
        if (next != prevScroll) {
            dom::Event scrollEvt("scroll", false, false);
            scrollEvt.setIsTrusted(true);
            windowHostDispatch(h, el, scrollEvt);
        }
        break;
    }
    if (jsRuntime_) jsRuntime_->executePendingJobs();
    windowHostRepaint(h);
}

// ---------------------------------------------------------------------------
// Drag & drop
// ---------------------------------------------------------------------------

void Engine::windowHostDispatchDrop(WindowHost& h, float x, float y,
                                    const std::vector<std::string>* paths,
                                    const std::string* text) {
    dom::Element* target = windowHostHitTest(h, x, y);
    if (!target) target = h.document->body();
    if (!target) return;

    for (const char* type : {"dragenter", "dragover", "drop"}) {
        dom::DragEvent evt(type, true, true);
        if (paths) for (const auto& p : *paths) evt.addFile(p);
        if (text) evt.setDataText(*text);
        evt.setIsTrusted(true);
        windowHostDispatch(h, target, evt);
    }
    if (jsRuntime_) jsRuntime_->executePendingJobs();
}

void Engine::hostDropFile(uint64_t hostId, const std::vector<std::string>& paths,
                          float x, float y) {
    WindowHost* hp = windowHostById(hostId);
    if (!hp || !hp->document || !hp->jsCtx) return;
    if (paths.empty()) return;
    windowHostDispatchDrop(*hp, (x >= 0) ? x : hp->lastMouseX,
                           (y >= 0) ? y : hp->lastMouseY, &paths, nullptr);
}

void Engine::hostDropText(uint64_t hostId, const std::string& text,
                          float x, float y) {
    WindowHost* hp = windowHostById(hostId);
    if (!hp || !hp->document || !hp->jsCtx) return;
    windowHostDispatchDrop(*hp, (x >= 0) ? x : hp->lastMouseX,
                           (y >= 0) ? y : hp->lastMouseY, nullptr, &text);
}

// ---------------------------------------------------------------------------
// Focus + page visibility
// ---------------------------------------------------------------------------

void Engine::windowHostSetVisibility(WindowHost& h, bool visible) {
    if (!h.jsCtx) return;
    // Per-realm: window_bindings installs __bro_set_visibility into every
    // realm, and the JS side guards a repeat so redundant calls are free.
    JSValue global = JS_GetGlobalObject(h.jsCtx);
    JSValue fn = JS_GetPropertyStr(h.jsCtx, global, "__bro_set_visibility");
    if (JS_IsFunction(h.jsCtx, fn)) {
        JSValue arg = JS_NewBool(h.jsCtx, visible);
        JSValue ret = JS_Call(h.jsCtx, fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(h.jsCtx, ret);
        JS_FreeValue(h.jsCtx, arg);
    }
    JS_FreeValue(h.jsCtx, fn);
    JS_FreeValue(h.jsCtx, global);
    if (jsRuntime_) jsRuntime_->executePendingJobs();
}

} // namespace bro::engine
