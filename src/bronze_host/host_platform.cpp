// The small, stateless half of the web platform: the microtask hop, the
// screen, the modal dialogs, and the DOM interface names apps test against.
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
// so an app under a driver script walks through its confirmations
// instead of blocking on a window nobody is looking at.
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

}  // namespace

void installPlatformGlobals() {
    // Each value is registered (which roots it) the moment it is made, and
    // the globalThis copies are read back from the registry: every make*
    // allocates, so a raw Value held across the next one would be stale.
    struct Entry { const char* name; Value (*make)(); };
    const Entry entries[] = {
        {"queueMicrotask", makeQueueMicrotask},
        {"screen", makeScreenValue},
        {"alert", makeAlert},
        {"confirm", makeConfirm},
        {"prompt", makePrompt},
        {"showOpenFileDialog", makeShowOpenFileDialog},
        {"showOpenFolderDialog", makeShowOpenFolderDialog},
        {"showSaveFileDialog", makeShowSaveFileDialog},
    };
    for (const Entry& e : entries) ev::registerGlobal(e.name, e.make());

    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        ev::Persistent global(gt.value);
        for (const Entry& e : entries) {
            ev::setProperty(global.get(), e.name, ev::globalValue(e.name).value);
        }
    }
    // window.screenX / screenY (and the screenLeft / screenTop aliases): the
    // window's position on the desktop, live like `screen` is; 0 headless,
    // where there is no desktop to be positioned on.
    {
        auto screenPos = [](bool wantX) {
            return [wantX](Value, std::span<const Value>) {
                engine::Engine* e = hostEngine();
                if (e && e->displayMode() != engine::DisplayMode::Headless) {
                    if (platform::Window* w = e->window()) {
                        int x = 0, y = 0;
                        w->getPosition(x, y);
                        return ev::fromDouble(wantX ? x : y);
                    }
                }
                return ev::fromDouble(0.0);
            };
        };
        auto defineOn = [&](Value target) {
            if (!ev::isObject(target)) return;
            ObjectBuilder t(target);
            t.accessor("screenX", screenPos(true), nullptr);
            t.accessor("screenY", screenPos(false), nullptr);
            t.accessor("screenLeft", screenPos(true), nullptr);
            t.accessor("screenTop", screenPos(false), nullptr);
        };
        if (gt.found) defineOn(gt.value);
        ev::GlobalValue winPos = ev::globalValue("window");
        gt = ev::globalValue("globalThis");
        if (winPos.found && ev::isObject(winPos.value) && winPos.value != gt.value) defineOn(winPos.value);
    }
    ev::GlobalValue win = ev::globalValue("window");
    if (win.found && ev::isObject(win.value) && win.value != gt.value) {
        ev::Persistent winRoot(win.value);
        for (const Entry& e : entries) {
            ev::setProperty(winRoot.get(), e.name, ev::globalValue(e.name).value);
        }
    }
    // Gamepad, GamepadButton, GamepadEvent globals
    installGamepadButtonGlobals();
}

}  // namespace bro::bronze_host
