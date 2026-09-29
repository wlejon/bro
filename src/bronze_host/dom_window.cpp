// The `window` global: the global object's window-shaped surface — its self
// aliases, viewport sizes and DPR, event listeners, scrolling, storage, focus,
// matchMedia, location and history. Installed once from installWebHostGlobals
// (dom_globals.cpp), after `document` and before the families that own their
// own files.
//
// Window listeners are held here as Persistent slots, keyed by the document
// whose window list carries them, and dropped on reload by
// clearWindowListeners (resetGlobalExpandos).

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_matchmedia.h"
#include "bronze_host/host_selection.h"
#include "bronze_host/host_window_open.h"

#include "engine/engine.h"
#include "platform/sdl_window.h"
#include "dom/document.h"
#include "dom/event.h"
#include "dom/event_target.h"
#include "util/interrupt.h"

#include <string>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

struct WindowListener {
    dom::ListenerHandle handle;
    std::string type;
    ev::Persistent fn;
    // (type, listener, capture) is the registration's identity on the web:
    // a repeat is a no-op and removal must name the same capture flag.
    bool capture = false;
    // The document whose window list holds it, so removal reaches that list
    // even when a different document is current.
    dom::Document* doc = nullptr;
};

// Process-lived, like the rest of the host state (dom_globals.cpp's lifetime
// note): the listeners' own closures reach back into it.
std::vector<WindowListener>& windowListeners() {
    static auto* list = new std::vector<WindowListener>();
    return *list;
}

// `window.getComputedStyle(el)` — and three.js's editor uses the bare one
// (editor/js/Sidebar.js), so registering only the window property would leave
// it a ReferenceError in exactly the code that needs it.
Value makeGetComputedStyle() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            return hostComputedStyleFor(argAt(a, 0));
        },
        1);
}

}  // namespace

void clearWindowListeners() {
    windowListeners().clear();
}

void installWindowGlobal(engine::Engine* enginePtr) {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    // The global object through the builder's root: every set/def below
    // allocates, so a raw copy of it would not survive the block.
    ObjectBuilder b(gt.value);
    ev::registerGlobal("window", b.get());
    ev::registerGlobal("self", b.get());
    ev::registerGlobal("top", b.get());
    ev::registerGlobal("parent", b.get());
    b.set("window", b.get());
    b.set("self", b.get());
    b.set("top", b.get());
    b.set("parent", b.get());

    b.accessor("devicePixelRatio",
               [enginePtr](Value, std::span<const Value>) {
                   // A secondary window reports the display it sits on.
                   dom::Document* curDoc = currentHostDocument();
                   if (curDoc && enginePtr) {
                       if (auto* wh = enginePtr->windowHostForDocument(curDoc))
                           return ev::fromDouble(wh->displayScale);
                   }
                   return ev::fromDouble(enginePtr->displayScale());
               },
               nullptr);
    b.accessor("innerWidth",
               [enginePtr](Value, std::span<const Value>) {
                   dom::Document* curDoc = currentHostDocument();
                   if (curDoc && enginePtr) {
                       if (auto* wh = enginePtr->windowHostForDocument(curDoc)) {
                           return ev::fromDouble(wh->boxW);
                       }
                       if (auto* ifr = enginePtr->iframeForDocument(curDoc)) {
                           return ev::fromDouble(ifr->boxW);
                       }
                   }
                   return ev::fromDouble(enginePtr ? enginePtr->contentWidth() : 0);
               },
               nullptr);
    b.accessor("innerHeight",
               [enginePtr](Value, std::span<const Value>) {
                   dom::Document* curDoc = currentHostDocument();
                   if (curDoc && enginePtr) {
                       if (auto* wh = enginePtr->windowHostForDocument(curDoc)) {
                           return ev::fromDouble(wh->boxH);
                       }
                       if (auto* ifr = enginePtr->iframeForDocument(curDoc)) {
                           return ev::fromDouble(ifr->boxH);
                       }
                   }
                   return ev::fromDouble(enginePtr ? enginePtr->contentHeight() : 0);
               },
               nullptr);
    b.accessor("outerWidth",
               [enginePtr](Value, std::span<const Value>) {
                   dom::Document* curDoc = currentHostDocument();
                   if (curDoc && enginePtr) {
                       if (auto* wh = enginePtr->windowHostForDocument(curDoc)) {
                           return ev::fromDouble(wh->width);
                       }
                       if (auto* ifr = enginePtr->iframeForDocument(curDoc)) {
                           return ev::fromDouble(ifr->boxW);
                       }
                   }
                   return ev::fromDouble(enginePtr ? enginePtr->viewportWidth() : 0);
               },
               nullptr);
    b.accessor("outerHeight",
               [enginePtr](Value, std::span<const Value>) {
                   dom::Document* curDoc = currentHostDocument();
                   if (curDoc && enginePtr) {
                       if (auto* wh = enginePtr->windowHostForDocument(curDoc)) {
                           return ev::fromDouble(wh->height);
                       }
                       if (auto* ifr = enginePtr->iframeForDocument(curDoc)) {
                           return ev::fromDouble(ifr->boxH);
                       }
                   }
                   return ev::fromDouble(enginePtr ? enginePtr->viewportHeight() : 0);
               },
               nullptr);

    b.accessor("opener",
               [](Value, std::span<const Value>) { return hostWindowOpener(); },
               nullptr);

    b.def("close", 0, [enginePtr](Value, std::span<const Value>) -> Value {
        dom::Document* curDoc = currentHostDocument();
        if (curDoc && enginePtr) {
            if (auto* wh = enginePtr->windowHostForDocument(curDoc)) {
                enginePtr->closeWindowHost(wh->id);
                return ev::undefined();
            }
        }
        ::bro::util::requestInterrupt();
        return ev::undefined();
    });

    b.def("addEventListener", 3, [enginePtr](Value thisValue, std::span<const Value> a) {
        // A bare `addEventListener(...)` arrives with no receiver; the
        // listener's `this` is the window either way.
        ev::Persistent self(ev::isObject(thisValue) ? thisValue
                                                    : ev::globalValue("window").value);
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV) || ev::isUndefined(typeV)) {
            return ev::throwTypeError("window.addEventListener: type must be a string");
        }
        if (!ev::isFunction(argAt(a, 1))) {
            return ev::throwTypeError(
                "window.addEventListener: listener must be a function");
        }
        // Rooted before toUtf8/readOptions, which can allocate.
        ev::Persistent fnP(argAt(a, 1));
        std::string type = ev::toUtf8(argAt(a, 0));
        dom::ListenerOptions opts = readOptions(argAt(a, 2));
        dom::Document* targetDoc = currentHostDocument() ? currentHostDocument() : (enginePtr ? enginePtr->document() : nullptr);
        if (!targetDoc) {
            return ev::throwError(
                "window.addEventListener: the engine refused the registration");
        }
        // A repeat (type, listener, capture) is a no-op, as on the web.
        for (const WindowListener& w : windowListeners()) {
            if (w.doc == targetDoc && w.type == type && w.capture == opts.capture &&
                ev::toBits(w.fn.get()) == ev::toBits(fnP.get())) {
                return ev::undefined();
            }
        }
        std::string origin = "window " + type + " listener";
        bool isOnce = opts.once;
        bool capture = opts.capture;
        std::string lType = type;
        dom::ListenerHandle handle = targetDoc->windowListeners().add(
            type, [fnP, self, origin, targetDoc, isOnce, capture, lType](dom::Event& evt) {
                if (isOnce) {
                    auto& list = windowListeners();
                    for (auto it = list.begin(); it != list.end(); ++it) {
                        if (it->doc == targetDoc && it->type == lType && it->capture == capture &&
                            ev::toBits(it->fn.get()) == ev::toBits(fnP.get())) {
                            list.erase(it);
                            break;
                        }
                    }
                }
                callBronzeListener(fnP, self, evt, origin.c_str(), targetDoc);
            }, opts);
        if (!handle) {
            return ev::throwError(
                "window.addEventListener: the engine refused the registration");
        }
        windowListeners().push_back(
            {handle, std::move(type), std::move(fnP), capture, targetDoc});
        return ev::undefined();
    });
    b.def("dispatchEvent", 1, [](Value, std::span<const Value> a) {
        return hostDispatchToWindow(argAt(a, 0));
    });
    b.def("removeEventListener", 3, [enginePtr](Value, std::span<const Value> a) {
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV) || ev::isUndefined(typeV)) return ev::undefined();
        std::string type = ev::toUtf8(typeV);
        dom::ListenerOptions opts = readOptions(argAt(a, 2));
        dom::Document* targetDoc = currentHostDocument() ? currentHostDocument() : (enginePtr ? enginePtr->document() : nullptr);
        // The listener is read from its argument slot after toUtf8 and
        // readOptions, which may allocate, and compared with nothing
        // allocating in between.
        const uint64_t fnBits = ev::toBits(argAt(a, 1));
        auto& list = windowListeners();
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (it->doc == targetDoc && it->type == type && it->capture == opts.capture &&
                ev::toBits(it->fn.get()) == fnBits) {
                if (it->doc) it->doc->windowListeners().remove(it->handle);
                list.erase(it);
                break;
            }
        }
        return ev::undefined();
    });

    // Registered before installWorkerGlobals' no-op `postMessage` stub,
    // whose found-guard then leaves this one in place.
    // A window property that is also a registered global: set on the
    // window, then registered from a fresh read of the window (b.set
    // allocates, so the value made before it is not reused raw).
    auto setAndRegister = [&b](const char* name, Value v) {
        b.set(name, v);
        ev::registerGlobal(name, ev::getProperty(b.get(), name));
    };

    // Registered before installWorkerGlobals' no-op `postMessage` stub,
    // whose found-guard then leaves this one in place.
    setAndRegister("postMessage", makeWindowPostMessage());
    setAndRegister("getComputedStyle", makeGetComputedStyle());
    ev::registerGlobal("close", ev::getProperty(b.get(), "close"));
    setAndRegister("localStorage", makeLocalStorageValue());
    setAndRegister("sessionStorage", makeSessionStorageValue());
    setAndRegister("screen", makeScreenValue());

    b.def("getSelection", 0, [](Value, std::span<const Value>) {
        auto* e = hostEngine();
        if (!e || !e->document()) return ev::null();
        return wrapSelection(e->document()->selection());
    });

    b.def("open", 1, [](Value, std::span<const Value> a) {
        return handleWindowOpen(a);
    });

    b.def("focus", 0, [](Value, std::span<const Value>) {
        if (auto* e = hostEngine()) {
            if (e->displayMode() != engine::DisplayMode::Headless && e->window()) e->window()->raise();
        }
        return ev::undefined();
    });

    b.def("blur", 0, [](Value, std::span<const Value>) {
        return ev::undefined();
    });

    setAndRegister("matchMedia", ev::makeFunction(
        [](Value, std::span<const Value> a) -> Value {
            std::string query = a.empty() || ev::isUndefined(a[0]) ? "" : ev::toUtf8(a[0]);
            return makeHostMatchMediaObject(query);
        },
        1));

    auto parseScrollArgs = [](std::span<const Value> a, double& x, double& y) {
        if (a.empty()) return;
        if (ev::isObject(a[0])) {
            Value leftV = ev::getProperty(a[0], "left");
            Value topV = ev::getProperty(a[0], "top");
            if (!ev::isUndefined(leftV)) x = ev::toDouble(leftV);
            if (!ev::isUndefined(topV)) y = ev::toDouble(topV);
        } else {
            x = ev::toDouble(a[0]);
            if (a.size() > 1) y = ev::toDouble(a[1]);
        }
    };
    // The window scrolls the document's root scroller: the viewport, or
    // <html> when that is a scroll container of its own
    // (rootScrollerElement). scrollY reads back what scrollTo wrote.
    auto doWindowScrollTo = [](double, double y) {
        scrollRootTo(static_cast<float>(y));
    };

    b.accessor("scrollX", [](Value, std::span<const Value>) { return ev::fromDouble(0.0); }, nullptr);
    b.accessor("pageXOffset", [](Value, std::span<const Value>) { return ev::fromDouble(0.0); }, nullptr);
    b.accessor("scrollY", [](Value, std::span<const Value>) {
        return ev::fromDouble(rootScrollY());
    }, nullptr);
    b.accessor("pageYOffset", [](Value, std::span<const Value>) {
        return ev::fromDouble(rootScrollY());
    }, nullptr);

    // An options object without `top` leaves the vertical offset alone.
    b.def("scrollTo", 2, [parseScrollArgs, doWindowScrollTo](Value, std::span<const Value> a) {
        double x = 0, y = rootScrollY();
        parseScrollArgs(a, x, y);
        doWindowScrollTo(x, y);
        return ev::undefined();
    });
    b.def("scroll", 2, [parseScrollArgs, doWindowScrollTo](Value, std::span<const Value> a) {
        double x = 0, y = rootScrollY();
        parseScrollArgs(a, x, y);
        doWindowScrollTo(x, y);
        return ev::undefined();
    });
    b.def("scrollBy", 2, [parseScrollArgs, doWindowScrollTo](Value, std::span<const Value> a) {
        double dx = 0, dy = 0;
        parseScrollArgs(a, dx, dy);
        doWindowScrollTo(0, rootScrollY() + dy);
        return ev::undefined();
    });

    for (const char* name : {"addEventListener", "removeEventListener", "dispatchEvent",
                             "scrollTo", "scroll", "scrollBy", "open", "focus", "blur"}) {
        ev::registerGlobal(name, ev::getProperty(b.get(), name));
    }

    {
        ObjectBuilder loc;
        loc.set("protocol", ev::fromUtf8("bro:"));
        loc.set("hostname", ev::fromUtf8("app"));
        loc.set("host", ev::fromUtf8("app"));
        loc.set("port", ev::fromUtf8(""));
        loc.set("pathname", ev::fromUtf8("/"));
        loc.set("href", ev::fromUtf8("bro://app/"));
        loc.set("origin", ev::fromUtf8("bro://app"));
        loc.set("search", ev::fromUtf8(""));
        loc.set("hash", ev::fromUtf8(""));
        loc.def("reload", 0, [](Value, std::span<const Value>) -> Value {
            if (auto* eng = hostEngine()) {
                dom::Document* curDoc = currentHostDocument();
                if (curDoc && eng->reloadIframeForDocument(curDoc)) {
                    return ev::undefined();
                }
                eng->requestAppReload();
            }
            return ev::undefined();
        });
        loc.def("replace", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
        loc.def("assign", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
        setAndRegister("location", loc.get());
    }

    {
        ObjectBuilder hist;
        hist.set("length", ev::fromDouble(1.0));
        hist.set("state", ev::null());
        hist.set("scrollRestoration", ev::fromUtf8("auto"));
        hist.def("back", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
        hist.def("forward", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
        hist.def("go", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
        hist.def("pushState", 3, [](Value, std::span<const Value>) { return ev::undefined(); });
        hist.def("replaceState", 3, [](Value, std::span<const Value>) { return ev::undefined(); });
        setAndRegister("history", hist.get());
    }
}

}  // namespace bro::bronze_host
