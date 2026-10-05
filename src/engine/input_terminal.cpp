// <terminal> input routing and its per-frame pump.
//
// A focused terminal is a keyboard sink the way a native terminal window is:
// Tab, Ctrl+C, Ctrl+V, the arrows and the function keys all belong to the
// program running in it, so its keydown is routed before the page's own
// handling (focus traversal, the clipboard shortcuts, the engine's hotkeys).
// The page still sees every keydown first and can preventDefault() it, which
// is how an application keeps a key for itself. Copy and paste are
// Ctrl+Shift+C / Ctrl+Shift+V and Shift+Insert (Cmd+C / Cmd+V on macOS).
//
// Text: a key that types is held until the text input it produces arrives,
// so the encoder gets both (ElTerminal::keyDown). IME composition is shown at
// the cursor (setPreedit) and its commit is typed into the child.

#include "engine/engine.h"
#include "engine/input_common.h"
#include "engine/key_mapping.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/el_terminal.h"
#include "platform/clipboard.h"
#include "util/time.h"

#include <SDL3/SDL.h>

namespace bro::engine {

layout::ElTerminal* Engine::focusedTerminal(dom::Element** elOut) {
    if (!document_) return nullptr;
    dom::Element* el = document_->activeElement();
    layout::ElTerminal* t = el ? el->terminalControl() : nullptr;
    if (elOut) *elOut = t ? el : nullptr;
    return t;
}

namespace {

bool isPasteChord(int keycode, int mod) {
    const bool ctrl = (mod & SDL_KMOD_CTRL) != 0;
    const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
#ifdef __APPLE__
    if ((mod & SDL_KMOD_GUI) && keycode == SDLK_V) return true;
#endif
    return (ctrl && shift && keycode == SDLK_V) || (shift && !ctrl && keycode == SDLK_INSERT);
}

bool isCopyChord(int keycode, int mod) {
    const bool ctrl = (mod & SDL_KMOD_CTRL) != 0;
    const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
#ifdef __APPLE__
    if ((mod & SDL_KMOD_GUI) && keycode == SDLK_C) return true;
#endif
    return (ctrl && shift && keycode == SDLK_C) || (ctrl && !shift && keycode == SDLK_INSERT);
}

} // namespace

bool Engine::terminalKeyDown(int keycode, int scancode, int mod, bool repeat) {
    dom::Element* el = nullptr;
    layout::ElTerminal* t = focusedTerminal(&el);
    if (!t) return false;

    dom::ElementHandle handle(document_.get(), el);
    auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
    evt.setIsComposing(t->composing());
    dispatchEvent(el, evt);
    // A listener may have moved focus or removed the element.
    dom::Element* now = nullptr;
    t = focusedTerminal(&now);
    if (!t || now != handle.get()) return true;
    if (evt.defaultPrevented()) {
        // As on the web, a cancelled keydown types nothing either.
        t->keyCancelled();
        return true;
    }
    el = now;

    // Keys belong to the input method while it composes.
    if (t->composing()) return true;

    if (isPasteChord(keycode, mod)) {
        if (!repeat) terminalPaste(platform::getClipboardText());
        return true;
    }
    if (isCopyChord(keycode, mod)) {
        const std::string text = t->selectionText();
        dom::ClipboardEvent copyEvt("copy", true, true);
        copyEvt.setClipboardText(text);
        copyEvt.setIsTrusted(true);
        dispatchEvent(el, copyEvt);
        if (!copyEvt.defaultPrevented() && !text.empty()) platform::setClipboardText(text);
        return true;
    }

    if (t->keyDown(keycode, scancode, mod, repeat)) {
        document_->markPaintDirty();  // the cursor shows solid while typing
        uiDirty_ = true;
        return true;
    }
    // Nothing a terminal encodes (a Cmd shortcut): the engine's hotkeys and
    // the action map still get it.
    if (!handleGlobalHotkey(keycode, mod, repeat))
        dispatchActionEventForKey(sdlKeycodeToWebKey(keycode, mod), "down", 1.0f);
    return true;
}

bool Engine::terminalKeyUp(int keycode, int scancode, int mod, bool repeat) {
    dom::Element* el = nullptr;
    layout::ElTerminal* t = focusedTerminal(&el);
    if (!t) return false;
    auto evt = makeKeyboardEvent("keyup", keycode, scancode, mod, repeat);
    evt.setIsComposing(t->composing());
    dispatchEvent(el, evt);
    if (evt.defaultPrevented()) return true;
    t = focusedTerminal(&el);
    if (t && !t->composing()) t->keyUp(keycode, scancode, mod);
    return true;
}

bool Engine::terminalTextInput(const std::string& text) {
    dom::Element* el = nullptr;
    layout::ElTerminal* t = focusedTerminal(&el);
    if (!t) return false;
    const bool wasComposing = t->composing();
    t->textInput(text);
    if (wasComposing) {
        dispatchCompositionEvent(el, "compositionupdate", text);
        dispatchCompositionEvent(el, "compositionend", text);
    }
    document_->markPaintDirty();
    uiDirty_ = true;
    return true;
}

bool Engine::terminalTextEditing(const std::string& text) {
    dom::Element* el = nullptr;
    layout::ElTerminal* t = focusedTerminal(&el);
    if (!t) return false;
    const bool wasComposing = t->composing();
    if (text.empty() && !wasComposing) return true;
    t->setPreedit(text);
    if (!wasComposing) dispatchCompositionEvent(el, "compositionstart", "");
    dispatchCompositionEvent(el, "compositionupdate", text);
    if (text.empty()) dispatchCompositionEvent(el, "compositionend", "");
    document_->markPaintDirty();
    uiDirty_ = true;
    updateTextInputArea();
    return true;
}

bool Engine::terminalPaste(const std::string& text) {
    dom::Element* el = nullptr;
    layout::ElTerminal* t = focusedTerminal(&el);
    if (!t) return false;
    dom::ElementHandle handle(document_.get(), el);
    dom::ClipboardEvent pasteEvt("paste", true, true);
    pasteEvt.setClipboardText(text);
    if (!text.empty()) pasteEvt.addItem({"text/plain", {}, text});
    pasteEvt.setIsTrusted(true);
    dispatchEvent(el, pasteEvt);
    if (pasteEvt.defaultPrevented() || text.empty()) return true;
    dom::Element* now = nullptr;
    t = focusedTerminal(&now);
    if (t && now == handle.get()) t->paste(text);
    return true;
}

void Engine::pumpTerminals() {
    // Headless runs on its virtual clock (the blink phase is then
    // deterministic); windowed on the wall clock.
    const double now = displayMode_ == DisplayMode::Headless ? virtualTime_ : util::currentTimeMs();
    bool repaint = false;
    layout::ElTerminal::forEach([&](layout::ElTerminal& t) {
        dom::Element* el = t.element();
        dom::Document* doc = el ? el->document() : nullptr;
        const bool focused = windowFocused_ && doc && doc->activeElement() == el;
        if (t.pump(now, focused)) {
            if (doc) doc->markPaintDirty();
            repaint = true;
        }
    });
    if (repaint) markAppBaseDirty();
}

} // namespace bro::engine
