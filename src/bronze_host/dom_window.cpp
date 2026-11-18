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
#include "bronze_host/host_builder.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_window_message.h"
#include "bronze_host/host_events.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_web_globals.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_global_kinds.h"
#include "bronze_host/host_matchmedia.h"
#include "bronze_host/host_compartment.h"
#include "bronze_host/host_selection.h"
#include "bronze_host/host_window_open.h"

#include "engine/engine.h"
#include "platform/window.h"
#include "dom/document.h"
#include "dom/event.h"
#include "dom/event_target.h"
#include "util/interrupt.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

std::unordered_map<uint64_t, std::string>& windowNames() {
    static auto* map = new std::unordered_map<uint64_t, std::string>();
    return *map;
}

std::unordered_map<uint64_t, std::string>& windowStatuses() {
    static auto* map = new std::unordered_map<uint64_t, std::string>();
    return *map;
}

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
    windowNames().clear();
    windowStatuses().clear();
}

void clearWindowName(uint64_t scopeId) {
    if (scopeId == 0) {
        windowNames().clear();
        windowStatuses().clear();
    } else {
        windowNames().erase(scopeId);
        windowStatuses().erase(scopeId);
    }
}

void installWindowGlobal(engine::Engine* enginePtr) {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    ObjectBuilder b(gt.value);
    // The registrations define the global object's own properties too, so
    // nothing here writes a registered name on it afterwards: a write after
    // the registration is the page's to make, and would make the name the
    // global object's to answer (host_global_kinds.h).
    registerHostGlobal("window", b.get(), ev::GlobalKind::Readonly);  // [LegacyUnforgeable]
    registerHostGlobal("self", b.get());                              // [Replaceable]
    registerHostGlobal("frames", b.get());                            // [Replaceable]
    registerHostGlobal("top", b.get(), ev::GlobalKind::Readonly);     // [LegacyUnforgeable]
    registerHostGlobal("parent", b.get());                            // [Replaceable]
    // A top-level window's frameElement; a frame's own is its host page's
    // element behind the membrane (installParentWindowProxy).
    b.accessor("frameElement", [](Value, std::span<const Value>) { return ev::null(); }, nullptr);
    b.accessor("closed", [](Value, std::span<const Value>) { return ev::fromBool(false); }, nullptr);
    b.accessor("isSecureContext", [](Value, std::span<const Value>) { return ev::fromBool(true); }, nullptr);
    // window.length: the document's child navigables, its frames.
    defineReplaceableAttribute(b, "length", [](Value, std::span<const Value>) {
        dom::Document* doc = currentHostDocument();
        const size_t n = doc ? doc->querySelectorAll("iframe").size() : 0;
        return ev::fromDouble(static_cast<double>(n));
    });
    // The BarProp objects: every bar of a window is visible.
    for (const char* bar : {"locationbar", "menubar", "personalbar", "scrollbars", "statusbar", "toolbar"}) {
        ObjectBuilder prop;
        prop.set("visible", ev::fromBool(true));
        registerHostGlobal(bar, prop.get());
    }
    {
        ObjectBuilder external;  // HTML's obsolete External: both members do nothing
        external.def("AddSearchProvider", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
        external.def("IsSearchProviderInstalled", 0,
                     [](Value, std::span<const Value>) { return ev::undefined(); });
        registerHostGlobal("external", external.get());
    }

    b.accessor("name",
               [](Value, std::span<const Value>) {
                   auto& names = windowNames();
                   auto it = names.find(currentCompartmentId());
                   return ev::fromUtf8(it != names.end() ? it->second : "");
               },
               [](Value, std::span<const Value> a) {
                   std::string s = ev::toUtf8(argAt(a, 0));
                   windowNames()[currentCompartmentId()] = std::move(s);
                   return ev::undefined();
               });
    // window.status: a string that shows nowhere, kept as it was set.
    b.accessor("status",
               [](Value, std::span<const Value>) {
                   auto& st = windowStatuses();
                   auto it = st.find(currentCompartmentId());
                   return ev::fromUtf8(it != st.end() ? it->second : "");
               },
               [](Value, std::span<const Value> a) {
                   windowStatuses()[currentCompartmentId()] = ev::toUtf8(argAt(a, 0));
                   return ev::undefined();
               });

    defineReplaceableAttribute(b, "devicePixelRatio",
               [enginePtr](Value, std::span<const Value>) {
                   // A secondary window reports the display it sits on.
                   dom::Document* curDoc = currentHostDocument();
                   if (curDoc && enginePtr) {
                       if (auto* wh = enginePtr->windowHostForDocument(curDoc))
                           return ev::fromDouble(wh->displayScale);
                   }
                   return ev::fromDouble(enginePtr->displayScale());
               });
    defineReplaceableAttribute(b, "innerWidth",
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
               });
    defineReplaceableAttribute(b, "innerHeight",
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
               });
    defineReplaceableAttribute(b, "outerWidth",
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
               });
    defineReplaceableAttribute(b, "outerHeight",
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
               });

    // opener's setter replaces it as [Replaceable]'s does (HTML 7.2.2.1;
    // there is no opener to disown).
    defineReplaceableAttribute(b, "opener",
                               [](Value, std::span<const Value>) { return hostWindowOpener(); });

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

    // The window's listeners are its realm's document's, whichever realm
    // registers them (`frame.contentWindow.addEventListener` is the frame's).
    const uint64_t home = currentCompartmentId();
    b.def("addEventListener", 3, [home](Value thisValue, std::span<const Value> a) {
        // A bare `addEventListener(...)` arrives with no receiver; the
        // listener's `this` is the window either way.
        ev::Persistent self(ev::isObject(thisValue) ? thisValue
                                                    : ev::globalValue("window").value);
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV) || ev::isUndefined(typeV)) {
            return ev::throwTypeError("window.addEventListener: type must be a string");
        }
        // A null listener is no registration (DOM addEventListener step 2).
        if (ev::isNull(argAt(a, 1)) || ev::isUndefined(argAt(a, 1))) return ev::undefined();
        // A function, or an EventListener object (its handleEvent is called).
        if (!ev::isObject(argAt(a, 1))) {
            return ev::throwTypeError(
                "window.addEventListener: listener must be a function or an object");
        }
        // Rooted before toUtf8/readOptions, which can allocate.
        ev::Persistent fnP(argAt(a, 1));
        std::string type = ev::toUtf8(argAt(a, 0));
        dom::ListenerOptions opts = readOptions(argAt(a, 2));
        dom::Document* targetDoc = compartmentDocument(home);
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
    defineReplaceableAttribute(b, "event", [](Value, std::span<const Value>) { return hostCurrentWindowEvent(); });
    b.def("dispatchEvent", 1, [](Value, std::span<const Value> a) {
        return hostDispatchToWindow(argAt(a, 0));
    });
    installWindowEventHandlers(b);
    b.def("removeEventListener", 3, [home](Value, std::span<const Value> a) {
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV) || ev::isUndefined(typeV)) return ev::undefined();
        std::string type = ev::toUtf8(typeV);
        dom::ListenerOptions opts = readOptions(argAt(a, 2));
        dom::Document* targetDoc = compartmentDocument(home);
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
    registerHostGlobal("postMessage", makeWindowPostMessage());
    registerHostGlobal("getComputedStyle", makeGetComputedStyle());
    registerHostGlobal("close", ev::getProperty(b.get(), "close"));
    registerHostGlobal("localStorage", makeLocalStorageValue(), ev::GlobalKind::Readonly);
    registerHostGlobal("sessionStorage", makeSessionStorageValue(), ev::GlobalKind::Readonly);

    b.def("getSelection", 0, [](Value, std::span<const Value>) {
        dom::Document* doc = currentHostDocument();  // this window's
        if (!doc) return ev::null();
        return wrapSelection(doc->selection());
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

    registerHostGlobal("matchMedia", ev::makeFunction(
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

    defineReplaceableAttribute(b, "scrollX", [](Value, std::span<const Value>) { return ev::fromDouble(0.0); });
    defineReplaceableAttribute(b, "pageXOffset", [](Value, std::span<const Value>) { return ev::fromDouble(0.0); });
    defineReplaceableAttribute(b, "scrollY", [](Value, std::span<const Value>) {
        return ev::fromDouble(rootScrollY());
    });
    defineReplaceableAttribute(b, "pageYOffset", [](Value, std::span<const Value>) {
        return ev::fromDouble(rootScrollY());
    });

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
        registerHostGlobal(name, ev::getProperty(b.get(), name));
    }

    installLocationAndHistory(b);
}

}  // namespace bro::bronze_host
