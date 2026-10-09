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
#include "engine/terminal_layers.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/el_terminal.h"
#include "platform/clipboard.h"
#include "platform/keys.h"
#include "util/time.h"

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
    const bool ctrl = (mod & platform::kmod::Ctrl) != 0;
    const bool shift = (mod & platform::kmod::Shift) != 0;
#ifdef __APPLE__
    if ((mod & platform::kmod::Gui) && keycode == platform::kc::V) return true;
#endif
    return (ctrl && shift && keycode == platform::kc::V) || (shift && !ctrl && keycode == platform::kc::Insert);
}

bool isCopyChord(int keycode, int mod) {
    const bool ctrl = (mod & platform::kmod::Ctrl) != 0;
    const bool shift = (mod & platform::kmod::Shift) != 0;
#ifdef __APPLE__
    if ((mod & platform::kmod::Gui) && keycode == platform::kc::C) return true;
#endif
    return (ctrl && shift && keycode == platform::kc::C) || (ctrl && !shift && keycode == platform::kc::Insert);
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

    const layout::ElTerminal::Host& host = layout::ElTerminal::host();
    if (isPasteChord(keycode, mod)) {
        if (!repeat && host.readClipboard) terminalPaste(host.readClipboard(false));
        return true;
    }
    if (isCopyChord(keycode, mod)) {
        const std::string text = t->selectionText();
        dom::ClipboardEvent copyEvt("copy", true, true);
        copyEvt.setClipboardText(text);
        copyEvt.setIsTrusted(true);
        dispatchEvent(el, copyEvt);
        if (!copyEvt.defaultPrevented() && !text.empty() && host.writeClipboard) host.writeClipboard(text, false);
        return true;
    }

    if (t->keyDown(keycode, scancode, mod, repeat)) {
        uiDirty_ = true;  // the cursor shows solid while typing (the terminal's own layer)
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

void installTerminalHost(bool headless);  // input_terminal_mouse.cpp

void Engine::pumpTerminals() {
    static bool hostInstalled = false;
    if (!hostInstalled) {
        installTerminalHost(displayMode_ == DisplayMode::Headless);
        hostInstalled = true;
    }
    // Headless runs on its virtual clock (the blink phase is then
    // deterministic); windowed on the wall clock.
    const double now = displayMode_ == DisplayMode::Headless ? virtualTime_ : util::currentTimeMs();
    bool repaint = false;
    const bool layered = terminalLayersEnabled();
    layout::ElTerminal::forEach([&](layout::ElTerminal& t) {
        dom::Element* el = t.element();
        dom::Document* doc = el ? el->document() : nullptr;
        const bool focused = windowFocused_ && doc && doc->activeElement() == el;
        // A repaint is the terminal's own layer: the page is not re-recorded
        // (unless the layers are off, BRO_TERMINAL_LAYER=0).
        if (t.pump(now, focused, deviceScale_.render)) {
            repaint = true;
            if (!layered && doc) doc->markPaintDirty();
        }
        if (t.takeCursorChanged() && el && hoveredElement_.get() == el) updateCursorFromHover(el);
    });
    if (repaint) {
        if (layered) uiDirty_ = true;
        else markAppBaseDirty();
    }
}

} // namespace bro::engine
