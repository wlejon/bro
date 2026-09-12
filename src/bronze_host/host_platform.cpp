// The small, stateless half of the web platform: base64, the microtask hop,
// the screen, the modal dialogs, and the DOM interface names apps test against.
//
// Nothing here owns state or touches the frame seam. It exists as its own file
// because dom_globals.cpp is about the DOM and the loop, and a grab-bag of
// one-function globals appended to it makes both harder to read than either is
// on its own.
//
// THE INTERFACE NAMES deserve a note, because at first sight they look like
// stubs and they are not. No host value is an instance of one of them: these
// are bare names, and the wrapper families they name have not been converted to
// real classes (`Image` is the one that has — host_image.cpp). What real code
// actually does with these names is TEST THEM:
//
//     if (typeof Node !== 'undefined' && el.nodeType === Node.TEXT_NODE)
//     if (typeof HTMLInputElement !== 'undefined') ... else ...
//
// Both need the name to RESOLVE, and the first needs the constant to be right.
// A name that does not resolve is a ReferenceError that takes the whole module
// down at import time — which is the outcome these prevent. Code that reaches
// for `instanceof` instead of `typeof` gets a TypeError today, and would get
// one from a stub constructor too, because `instanceof` against a function
// with no `prototype` throws rather than answering false. The fix is not a
// better stub: it is converting the family behind the name, at which point
// reading `prototype` mints a real one and the answer is simply true.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "platform/dialogs.h"
#include "platform/sdl_window.h"
#include "util/log.h"

#include <cstdint>
#include <optional>
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
            std::string s = ev::toUtf8(v);
            std::vector<uint8_t> latin1;
            latin1.reserve(s.size());
            const uint8_t* u = reinterpret_cast<const uint8_t*>(s.data());
            size_t len = s.size();
            for (size_t k = 0; k < len;) {
                uint8_t b0 = u[k];
                if (b0 < 0x80) {
                    latin1.push_back(b0);
                    k++;
                } else if ((b0 & 0xE0) == 0xC0 && k + 1 < len) {
                    uint32_t cp = ((b0 & 0x1F) << 6) | (u[k + 1] & 0x3F);
                    if (cp > 255) return ev::throwTypeError("btoa: character out of range");
                    latin1.push_back(static_cast<uint8_t>(cp));
                    k += 2;
                } else {
                    return ev::throwTypeError("btoa: character out of range");
                }
            }

            std::string out;
            out.reserve((latin1.size() + 2) / 3 * 4);
            size_t i = 0;
            for (; i + 2 < latin1.size(); i += 3) {
                uint32_t n = (latin1[i] << 16) |
                             (latin1[i + 1] << 8) |
                             latin1[i + 2];
                out += kB64[(n >> 18) & 63];
                out += kB64[(n >> 12) & 63];
                out += kB64[(n >> 6) & 63];
                out += kB64[n & 63];
            }
            if (i < latin1.size()) {
                uint32_t n = latin1[i] << 16;
                bool two = (i + 1 < latin1.size());
                if (two) n |= latin1[i + 1] << 8;
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
            std::vector<uint8_t> decoded;
            int bits = 0;
            uint32_t acc = 0;
            for (char c : s) {
                if (c == '=') break;
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
                    decoded.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
                }
            }
            // In WHATWG DOM spec, atob() returns a binary string where each
            // character's code point is 0..255. In UTF-8, code points 128..255
            // are 2-byte sequences: 0xC2/0xC3 followed by 0x80..0xBF.
            std::string utf8_out;
            utf8_out.reserve(decoded.size() * 2);
            for (uint8_t b : decoded) {
                if (b < 0x80) {
                    utf8_out += static_cast<char>(b);
                } else {
                    utf8_out += static_cast<char>(0xC0 | (b >> 6));
                    utf8_out += static_cast<char>(0x80 | (b & 0x3F));
                }
            }
            return ev::fromUtf8(utf8_out);
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

} // namespace

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

namespace {

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
            platform::Dialogs::showAlert(messageArg(a, 0));
            return ev::undefined();
        },
        1);
}

Value makeConfirm() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            return ev::fromBool(platform::Dialogs::showConfirm(messageArg(a, 0)));
        },
        1);
}

Value makePrompt() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            auto answer = platform::Dialogs::showPrompt(messageArg(a, 0),
                                                         messageArg(a, 1));
            if (!answer) return ev::null();
            return ev::fromUtf8(*answer);
        },
        2);
}

// The file dialogs (docs/dialogs-api.js). A refused filter is thrown, not
// returned: the return is an empty list either way, and the caller wrote the
// filter SDL is objecting to.
Value stringArray(const std::vector<std::string>& items) {
    ev::CallResult parsed = ev::parseJson("[]");
    ev::Persistent arr{parsed.value};
    for (uint32_t i = 0; i < items.size(); ++i) {
        ev::Persistent s{ev::fromUtf8(items[i])};
        arr.set(ev::setElement(arr.get(), i, s.get()));
    }
    return arr.get();
}

std::string stringArg(std::span<const Value> a, size_t i) {
    Value v = argAt(a, i);
    if (!ev::isString(v)) return std::string();
    return ev::toUtf8(v);
}

bool boolArg(std::span<const Value> a, size_t i) {
    return i < a.size() && ev::toBool(a[i]);
}

Value makeShowOpenFileDialog() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            std::vector<std::string> picked;
            std::string refusal;
            if (!platform::Dialogs::showOpenFileDialog(stringArg(a, 0), boolArg(a, 1),
                                                       picked, refusal)) {
                return ev::throwTypeError(refusal);
            }
            return stringArray(picked);
        },
        2);
}

Value makeShowOpenFolderDialog() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            std::vector<std::string> picked;
            std::string refusal;
            if (!platform::Dialogs::showOpenFolderDialog(stringArg(a, 0), boolArg(a, 1),
                                                         picked, refusal)) {
                return ev::throwTypeError(refusal);
            }
            return stringArray(picked);
        },
        2);
}

Value makeShowSaveFileDialog() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            std::optional<std::string> saved;
            std::string refusal;
            if (!platform::Dialogs::showSaveFileDialog(stringArg(a, 0), stringArg(a, 1),
                                                       saved, refusal)) {
                return ev::throwTypeError(refusal);
            }
            if (!saved) return ev::null();
            return ev::fromUtf8(*saved);
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



// ---------------------------------------------------------------------------
// TextDecoder / TextEncoder
// ---------------------------------------------------------------------------

Value makeTextDecoder() {
    return ev::makeFunction(
        [](Value, std::span<const Value> /*a*/) -> Value {
            ObjectBuilder b;
            b.set("encoding", ev::fromUtf8("utf-8"));
            b.def("decode", 1, [](Value, std::span<const Value> a) -> Value {
                if (a.empty()) return ev::fromUtf8("");
                Value v = a[0];
                if (ev::isTypedArray(v)) {
                    ev::TypedArrayInfo info = ev::typedArrayInfo(v);
                    if (info && info.byteLength > 0) {
                        return ev::fromUtf8(std::string_view(
                            reinterpret_cast<const char*>(info.data), info.byteLength));
                    }
                    return ev::fromUtf8("");
                }
                if (ev::isArrayBuffer(v)) {
                    ev::ArrayBufferInfo info = ev::arrayBufferInfo(v);
                    if (info && info.byteLength > 0) {
                        return ev::fromUtf8(std::string_view(
                            reinterpret_cast<const char*>(info.data), info.byteLength));
                    }
                    return ev::fromUtf8("");
                }
                return ev::fromUtf8("");
            });
            return b.get();
        },
        0);
}

Value makeTextEncoder() {
    return ev::makeFunction(
        [](Value, std::span<const Value> /*a*/) -> Value {
            ObjectBuilder b;
            b.set("encoding", ev::fromUtf8("utf-8"));
            b.def("encode", 1, [](Value, std::span<const Value> a) -> Value {
                std::string s;
                if (!a.empty() && !ev::isUndefined(a[0]) && !ev::isNull(a[0])) {
                    s = ev::toUtf8(a[0]);
                }
                Value arr = ev::createTypedArray(bronze::embed::elements::Uint8, static_cast<uint32_t>(s.size()));
                if (!s.empty()) {
                    ev::fillTypedArray(arr, std::span<const uint8_t>(
                                                reinterpret_cast<const uint8_t*>(s.data()), s.size()));
                }
                return arr;
            });
            return b.get();
        },
        0);
}

}  // namespace

Value makeEventConstructor(const char* name) {
    std::string eventName = name;
    Value fn = ev::makeFunction(
        [eventName](Value, std::span<const Value> a) -> Value {
            ObjectBuilder b;
            Value typeV = a.empty() ? ev::fromUtf8("") : a[0];
            b.set("type", typeV);
            bool bubbles = false;
            bool cancelable = false;
            bool composed = false;
            Value detail = ev::null();
            if (a.size() > 1 && ev::isObject(a[1])) {
                Value bProp = ev::getProperty(a[1], "bubbles");
                if (!ev::isUndefined(bProp)) bubbles = ev::toBool(bProp);
                Value cProp = ev::getProperty(a[1], "cancelable");
                if (!ev::isUndefined(cProp)) cancelable = ev::toBool(cProp);
                Value compProp = ev::getProperty(a[1], "composed");
                if (!ev::isUndefined(compProp)) composed = ev::toBool(compProp);
                Value dProp = ev::getProperty(a[1], "detail");
                if (!ev::isUndefined(dProp)) detail = dProp;
                Value dataProp = ev::getProperty(a[1], "data");
                if (!ev::isUndefined(dataProp)) b.set("data", dataProp);
                Value originProp = ev::getProperty(a[1], "origin");
                if (!ev::isUndefined(originProp)) b.set("origin", originProp);
                Value lastEventIdProp = ev::getProperty(a[1], "lastEventId");
                if (!ev::isUndefined(lastEventIdProp)) b.set("lastEventId", lastEventIdProp);
                Value portsProp = ev::getProperty(a[1], "ports");
                if (!ev::isUndefined(portsProp)) b.set("ports", portsProp);
            }
            b.set("bubbles", ev::fromBool(bubbles));
            b.set("cancelable", ev::fromBool(cancelable));
            b.set("composed", ev::fromBool(composed));
            b.set("target", ev::null());
            b.set("currentTarget", ev::null());
            b.set("timeStamp", ev::fromDouble(0.0));
            b.set("detail", detail);
            b.set("defaultPrevented", ev::fromBool(false));
            b.def("preventDefault", 0, [](Value self_, std::span<const Value>) {
                ev::setProperty(self_, "defaultPrevented", ev::fromBool(true));
                return ev::undefined();
            });
            b.def("stopPropagation", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
            b.def("stopImmediatePropagation", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
            return b.get();
        },
        2);
    return fn;
}

void installPlatformGlobals() {
    ev::registerGlobal("btoa", makeBtoa());
    ev::registerGlobal("atob", makeAtob());
    ev::registerGlobal("queueMicrotask", makeQueueMicrotask());
    ev::registerGlobal("screen", makeScreenValue());
    ev::registerGlobal("alert", makeAlert());
    ev::registerGlobal("confirm", makeConfirm());
    ev::registerGlobal("prompt", makePrompt());
    ev::registerGlobal("showOpenFileDialog", makeShowOpenFileDialog());
    ev::registerGlobal("showOpenFolderDialog", makeShowOpenFolderDialog());
    ev::registerGlobal("showSaveFileDialog", makeShowSaveFileDialog());
    // The rest, in the manifest's order. Each is a name a real library tests
    // for before deciding what kind of environment it is in.
    for (const char* name : {
             "Event", "UIEvent", "MouseEvent", "PointerEvent", "KeyboardEvent",
             "WheelEvent", "InputEvent", "FocusEvent", "ProgressEvent",
         }) {
        ev::registerGlobal(name, makeEventConstructor(name));
    }
    for (const char* name : {
             "Text", "CharacterData",
             "Comment", "DocumentFragment", "Gamepad", "GamepadButton",
             "GamepadEvent",
         }) {
        ev::registerGlobal(name, makeInterfaceValue(name));
    }
    ev::registerGlobal("TextDecoder", makeTextDecoder());
    ev::registerGlobal("TextEncoder", makeTextEncoder());
}

}  // namespace bro::bronze_host
