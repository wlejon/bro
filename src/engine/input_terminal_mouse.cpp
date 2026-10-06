// <terminal> mouse routing, and what the terminal asks of its host
// (ElTerminal::Host: the clipboard, the primary selection, opening links).
//
// The page sees every mouse event first, as for any element; a mousedown it
// cancels never reaches the terminal. A press the terminal takes (a
// selection gesture, a report to a program that tracks the mouse) captures
// the pointer: moves and the release go to that terminal wherever the
// pointer is, which is what lets a selection drag past the grid scroll it.
// The wheel goes to the terminal under the pointer unless the page cancels
// it; a terminal at the end of its history lets the page scroll instead.

#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "layout/el_terminal.h"
#include "platform/clipboard.h"
#include "util/log.h"

#include <SDL3/SDL.h>

#include <string>

namespace bro::engine {

namespace {

// The clipboard is the one the page has (navigator.clipboard, execCommand,
// the editing shortcuts): platform::setClipboardText / getClipboardText, in
// headless as in a window, so a terminal's copy is the page's paste and the
// other way round. The primary selection is the OS's only on X11/Wayland in
// a window; everywhere else it is this process-local string.
std::string g_primary;

} // namespace

void installTerminalHost(bool headless) {
    layout::ElTerminal::Host h;
    h.writeClipboard = [headless](const std::string& text, bool primary) {
        if (!primary) return platform::setClipboardText(text);
#if defined(__linux__) || defined(__FreeBSD__)
        if (!headless && SDL_SetPrimarySelectionText(text.c_str())) return true;
#endif
        (void)headless;
        g_primary = text;
        return true;
    };
    h.readClipboard = [headless](bool primary) -> std::string {
        if (!primary) return platform::getClipboardText();
#if defined(__linux__) || defined(__FreeBSD__)
        if (!headless && SDL_HasPrimarySelectionText()) {
            if (char* t = SDL_GetPrimarySelectionText()) {
                std::string s(t);
                SDL_free(t);
                return s;
            }
        }
#endif
        (void)headless;
        return g_primary;
    };
    if (headless) {
        h.openLink = [](const std::string&, const std::string&) {};  // headless opens nothing
    } else {
        h.openLink = [](const std::string& target, const std::string&) {
            if (!SDL_OpenURL(target.c_str())) LOG_WARN("terminal: could not open %s: %s", target.c_str(), SDL_GetError());
        };
    }
    layout::ElTerminal::setHost(std::move(h));
}

void Engine::terminalMouseDown(dom::Element* target, float docX, float docY, int button, int mod, int clicks) {
    layout::ElTerminal* t = target ? target->terminalControl() : nullptr;
    if (!t) return;
    dom::ElementHandle handle(document_.get(), target);
    const layout::ElTerminal::MouseResult r = t->mouseDown(docX, docY, button, mod, clicks);
    if (!handle.get()) return;  // a linkactivate listener removed it
    if (r.capture) terminalCapture_.assign(document_.get(), target);
    if (r.pasteText) terminalPaste(*r.pasteText);
    if (r.handled) uiDirty_ = true;
}

void Engine::terminalMouseMove(dom::Element* target, dom::Element* prevHover, float docX, float docY) {
    const int mod = currentModState();
    if (dom::Element* cap = terminalCapture_.get()) {
        if (auto* t = cap->terminalControl()) t->mouseMove(docX, docY, mod);
        return;
    }
    if (prevHover && prevHover != target)
        if (auto* p = prevHover->terminalControl()) p->mouseLeave();
    if (auto* t = target ? target->terminalControl() : nullptr) t->mouseMove(docX, docY, mod);
}

void Engine::terminalMouseUp(float docX, float docY, int button) {
    dom::Element* cap = terminalCapture_.get();
    if (!cap) return;
    if (auto* t = cap->terminalControl()) t->mouseUp(docX, docY, button, currentModState());
    // Captured until every button the press holds is up.
    if (pressedButtons_ == 0) terminalCapture_.reset();
    uiDirty_ = true;
}

bool Engine::terminalWheel(dom::Element* target, float docX, float docY, float dy) {
    layout::ElTerminal* t = target ? target->terminalControl() : nullptr;
    if (!t || dy == 0.0f) return false;
    return t->wheel(docX, docY, dy, currentModState());
}

} // namespace bro::engine
