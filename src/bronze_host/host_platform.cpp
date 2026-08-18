// The small, stateless half of the web platform: base64, the microtask hop,
// the screen, the modal dialogs, and the DOM interface names apps test against.
//
// Nothing here owns state or touches the frame seam. It exists as its own file
// because dom_globals.cpp is about the DOM and the loop, and a grab-bag of
// one-function globals appended to it makes both harder to read than either is
// on its own.
//
// THE INTERFACE NAMES deserve a note, because at first sight they look like
// stubs and they are not. Compiled code cannot build a value on a chosen
// prototype (the embed API has no prototype argument — see the boundary rule in
// README.md), so no host value is ever an instance of one of these however they
// are written. What real code actually does with these names is TEST THEM:
//
//     if (typeof Node !== 'undefined' && el.nodeType === Node.TEXT_NODE)
//     if (typeof HTMLInputElement !== 'undefined') ... else ...
//
// Both need the name to RESOLVE, and the first needs the constant to be right.
// A name that does not resolve is a ReferenceError that takes the whole module
// down at import time — which is the outcome these prevent. Code that reaches
// for `instanceof` instead of `typeof` gets a TypeError, and would get one
// from a stub constructor too: a host function cannot be given a `.prototype`,
// and `instanceof` against a function without one throws rather than answering
// false.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "js/dialog_bindings.h"
#include "platform/sdl_window.h"
#include "util/log.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// base64
// ---------------------------------------------------------------------------

// btoa/atob are BYTE-string codecs, not text codecs: each JS char is one octet
// and a char above 0xFF is an error on the web (InvalidCharacterError). This
// throws a TypeError instead — the embed API has no DOMException, and every
// caller that checks checks for "it threw", not for the name.
const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

Value makeBtoa() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) -> Value {
            Value v = argAt(a, 0);
            if (ev::isObject(v) || ev::isUndefined(v))
                return ev::throwTypeError("btoa: expected a string");
            // toUtf8 gives us UTF-8 bytes; a source string that stayed inside
            // Latin-1 is exactly the input btoa accepts, and anything above it
            // would have been an error anyway, so the multi-byte encoding of a
            // rejected character never reaches the output.
            std::string s = ev::toUtf8(v);
            std::string out;
            out.reserve((s.size() + 2) / 3 * 4);
            size_t i = 0;
            for (; i + 2 < s.size(); i += 3) {
                uint32_t n = (static_cast<uint8_t>(s[i]) << 16) |
                             (static_cast<uint8_t>(s[i + 1]) << 8) |
                             static_cast<uint8_t>(s[i + 2]);
                out += kB64[(n >> 18) & 63];
                out += kB64[(n >> 12) & 63];
                out += kB64[(n >> 6) & 63];
                out += kB64[n & 63];
            }
            if (i < s.size()) {
                uint32_t n = static_cast<uint8_t>(s[i]) << 16;
                bool two = (i + 1 < s.size());
                if (two) n |= static_cast<uint8_t>(s[i + 1]) << 8;
                out += kB64[(n >> 18) & 63];
                out += kB64[(n >> 12) & 63];
                out += two ? kB64[(n >> 6) & 63] : '=';
                out += '=';
            }
            return ev::fromUtf8(out);
        },
        1);
}

Value makeAtob() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) -> Value {
            Value v = argAt(a, 0);
            if (ev::isObject(v) || ev::isUndefined(v))
                return ev::throwTypeError("atob: expected a string");
            std::string s = ev::toUtf8(v);
            std::string out;
            int bits = 0;
            uint32_t acc = 0;
            for (char c : s) {
                if (c == '=' ) break;
                // Whitespace is skipped rather than rejected: the web's atob
                // does that, and base64 pasted out of a file is full of it.
                if (c == '\n' || c == '\r' || c == '\t' || c == ' ' || c == '\f')
                    continue;
                int idx = b64Index(c);
                if (idx < 0)
                    return ev::throwTypeError("atob: not base64");
                acc = (acc << 6) | static_cast<uint32_t>(idx);
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    out += static_cast<char>((acc >> bits) & 0xFF);
                }
            }
            return ev::fromUtf8(out);
        },
        1);
}

// ---------------------------------------------------------------------------
// queueMicrotask
// ---------------------------------------------------------------------------

// Built out of a promise, because that is the only microtask source the embed
// API offers: create one, resolve it, and hang the callback off its `then`.
// The ordering is right — a promise reaction IS a microtask, and it runs at
// the same checkpoint queueMicrotask's own queue is drained at — and the cost
// is one throwaway promise per call, which is what a polyfill would pay too.
//
// A throw out of the callback is reported and swallowed rather than becoming
// an unhandled rejection on a promise nobody can see: the web reports it to
// the global error handler, and an invisible rejection is the one outcome that
// tells the developer nothing.
Value makeQueueMicrotask() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) -> Value {
            Value fn = argAt(a, 0);
            if (!ev::isFunction(fn))
                return ev::throwTypeError("queueMicrotask: expected a function");
            ev::Persistent cb(fn);
            Value promise = ev::createPromise();
            ev::Persistent p(promise);
            ev::resolvePromise(p.get(), ev::undefined());
            Value then = ev::getProperty(p.get(), "then");
            if (!ev::isFunction(then)) {
                LOG_ERROR("[bronze] queueMicrotask: promise has no then");
                return ev::undefined();
            }
            ev::Persistent thenFn(then);
            Value reaction = ev::makeFunction(
                [cb](Value, std::span<const Value>) mutable {
                    ev::CallResult r = ev::call(cb.get(), ev::undefined(), {});
                    if (r.thrown) reportBronzeError("queueMicrotask", r.value);
                    return ev::undefined();
                },
                1);
            ev::CallResult r =
                ev::call(thenFn.get(), p.get(), std::span<const Value>(&reaction, 1));
            if (r.thrown) reportBronzeError("queueMicrotask", r.value);
            return ev::undefined();
        },
        1);
}

// ---------------------------------------------------------------------------
// screen
// ---------------------------------------------------------------------------

// The display the window is on, not the window: three.js's editor uses it to
// decide a default render size (editor/js/Menubar.Render.js), and a `screen`
// that reported the window's own size would make "render at screen resolution"
// mean "render at whatever size the window happens to be".
//
// Accessors rather than values: a window dragged to another monitor changes
// which display these describe, and a snapshot taken at install time would
// still be describing the first one.
//
// HEADLESS PINS TO THE WINDOW, matching what bro's own window.screen does
// (window_bindings.cpp) and for the same reason: a test that printed the real
// desktop size would pass or fail depending on whose desk it ran on.
void screenDims(bool avail, double& w, double& h) {
    engine::Engine* e = hostEngine();
    if (!e) { w = h = 0; return; }
    platform::Window* win = e->window();
    if (win && e->displayMode() != engine::DisplayMode::Headless) {
        for (const platform::DisplayInfo& d : win->getDisplays()) {
            if (!d.isCurrent) continue;
            w = avail ? d.workWidth : d.width;
            h = avail ? d.workHeight : d.height;
            return;
        }
    }
    w = e->contentWidth();
    h = e->contentHeight();
}

Value makeScreenValue() {
    ObjectBuilder b;
    auto dim = [](bool wantWidth, bool avail) {
        return [wantWidth, avail](Value, std::span<const Value>) {
            double w = 0, h = 0;
            screenDims(avail, w, h);
            return ev::fromDouble(wantWidth ? w : h);
        };
    };
    b.accessor("width", dim(true, false), nullptr);
    b.accessor("height", dim(false, false), nullptr);
    b.accessor("availWidth", dim(true, true), nullptr);
    b.accessor("availHeight", dim(false, true), nullptr);
    // bro renders RGBA8 everywhere, so these are constants rather than a probe.
    b.set("colorDepth", ev::fromDouble(24.0));
    b.set("pixelDepth", ev::fromDouble(24.0));
    return b.get();
}

// ---------------------------------------------------------------------------
// Modal dialogs
// ---------------------------------------------------------------------------

// Straight to DialogBindings, which is where headless's auto-answer lives —
// so a compiled app under a driver script walks through its confirmations
// instead of blocking on a window nobody is looking at, exactly as an
// interpreted one does.
std::string messageArg(std::span<const Value> a, size_t i) {
    Value v = argAt(a, i);
    if (ev::isObject(v) || ev::isUndefined(v)) return std::string();
    return ev::toUtf8(v);
}

Value makeAlert() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            js::DialogBindings::showAlert(messageArg(a, 0));
            return ev::undefined();
        },
        1);
}

Value makeConfirm() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            return ev::fromBool(js::DialogBindings::showConfirm(messageArg(a, 0)));
        },
        1);
}

Value makePrompt() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            auto answer = js::DialogBindings::showPrompt(messageArg(a, 0),
                                                         messageArg(a, 1));
            if (!answer) return ev::null();
            return ev::fromUtf8(*answer);
        },
        2);
}

// ---------------------------------------------------------------------------
// Interface names
// ---------------------------------------------------------------------------

// A plain named OBJECT, not a function — see the note at the top of this file
// for why a name that merely RESOLVES is the whole job.
//
// An object rather than a stub constructor. A host function CAN carry
// properties now (embed::setProperty takes a function receiver), but it still
// cannot be given a `.prototype` — that one is refused by name — and without a
// prototype `x instanceof Fn` is a TypeError rather than false. So an object
// loses nothing that a function would win, and `Node.TEXT_NODE` is gained.
Value makeInterfaceValue(const char* name) {
    ObjectBuilder b;
    b.set("name", ev::fromUtf8(name));
    return b.get();
}

// Node is the one interface whose CONSTANTS are read as often as its name:
// `el.nodeType === Node.ELEMENT_NODE` is how library code tests a node without
// assuming a tag. The numbers are the DOM's, and they are not arbitrary.
Value makeNodeInterface() {
    ev::Persistent p(makeInterfaceValue("Node"));
    struct { const char* name; double value; } kConstants[] = {
        {"ELEMENT_NODE", 1},                {"ATTRIBUTE_NODE", 2},
        {"TEXT_NODE", 3},                   {"CDATA_SECTION_NODE", 4},
        {"ENTITY_REFERENCE_NODE", 5},       {"ENTITY_NODE", 6},
        {"PROCESSING_INSTRUCTION_NODE", 7}, {"COMMENT_NODE", 8},
        {"DOCUMENT_NODE", 9},               {"DOCUMENT_TYPE_NODE", 10},
        {"DOCUMENT_FRAGMENT_NODE", 11},     {"NOTATION_NODE", 12},
    };
    for (const auto& c : kConstants)
        p.set(ev::setProperty(p.get(), c.name, ev::fromDouble(c.value)));
    return p.get();
}

}  // namespace

void installPlatformGlobals() {
    ev::registerGlobal("btoa", makeBtoa());
    ev::registerGlobal("atob", makeAtob());
    ev::registerGlobal("queueMicrotask", makeQueueMicrotask());
    ev::registerGlobal("screen", makeScreenValue());
    ev::registerGlobal("alert", makeAlert());
    ev::registerGlobal("confirm", makeConfirm());
    ev::registerGlobal("prompt", makePrompt());
    ev::registerGlobal("Node", makeNodeInterface());
    // The rest, in the manifest's order. Each is a name a real library tests
    // for before deciding what kind of environment it is in.
    for (const char* name : {
             "Event", "UIEvent", "MouseEvent", "PointerEvent", "KeyboardEvent",
             "WheelEvent", "InputEvent", "FocusEvent", "ProgressEvent",
             "Element", "HTMLElement", "HTMLInputElement", "HTMLSelectElement",
             "HTMLTextAreaElement", "HTMLVideoElement", "HTMLMediaElement",
             "HTMLAnchorElement", "HTMLDivElement", "Text", "CharacterData",
             "Comment", "DocumentFragment", "Gamepad", "GamepadButton",
             "GamepadEvent",
         }) {
        ev::registerGlobal(name, makeInterfaceValue(name));
    }
}

}  // namespace bro::bronze_host
