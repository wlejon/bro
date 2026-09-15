// The minimal DOM a bronze-compiled three.js app touches: document (create a
// canvas, append it), the canvas element itself (size, style, getContext),
// window (sizes, DPR, resize listeners), and requestAnimationFrame driven by
// Engine::onFrame. Everything else the web platform offers is deliberately
// absent — an unknown createElement tag is a named refusal, not a stub that
// fails later.
//
// What is HERE: the frame seam (hostFrame), document, window, the canvas
// object, and installWebHostGlobals. What moved out, and where to look for it:
// localStorage to dom_storage.cpp, the gamepad surface and navigator to
// dom_gamepad.cpp, the element surface to host_element*.cpp.
//
// bronze already provides `console` from its own runtime (rt_print.cpp), so
// no console is registered here — duplicating it would shadow the builtin
// ladder for nothing (host globals lose to builtins anyway; see
// runtime/host_globals.h).
//
// LIFETIME MODEL: one process-lived HostState (never freed — the
// Engine::onFrame convention: callbacks register once and are never
// unregistered, so their captures must outlive the loop). Bronze Values held
// across frames (canvas objects, rAF callbacks, listeners) live in
// embed::Persistent slots; dom::Element pointers are owned by the Document
// and outlive the compiled program's run.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_canvas2d.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/eval.h"
#include "bronze_host/host_headless.h"
#include "bronze_host/host_gc.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_html_interfaces.h"
#include "bronze_host/host_range.h"
#include "bronze_host/host_selection.h"
#include "bronze_host/host_matchmedia.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_realm_scope.h"
#include "bronze_host/host_window_open.h"
#include "bronze_host/host_intl.h"
#include "bronze_host/host_web_animations.h"

#include "engine/engine.h"
#include "platform/sdl_window.h"
#include "platform/clipboard.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_target.h"
#include "css/parser.h"
#include "util/log.h"
#include "util/interrupt.h"
#include <unordered_set>

#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct RafEntry {
    int32_t id;
    dom::Document* doc = nullptr;
    ev::Persistent fn;
};

struct WindowListener {
    dom::ListenerHandle handle;
    std::string type;
    ev::Persistent fn;
};

struct HostState {
    engine::Engine* engine = nullptr;
    std::vector<RafEntry> rafPending;
    int32_t nextRafId = 1;
    // The rAF/performance clock: accumulated scaled-frame deltas, so
    // timestamps here advance exactly as the engine's own rAF timestamps do
    // (paused while bro.time is paused, virtual under headless advanceTime).
    double clockMs = 0.0;
    std::vector<WindowListener> windowListeners;
};

HostState* g_host = nullptr;

// ---------------------------------------------------------------------------
// requestAnimationFrame
// ---------------------------------------------------------------------------

// REENTRANCY: the pending list is MOVED OUT before any callback runs,
// so a callback that registers another rAF appends to the fresh list (fires next frame),
// and a cancelAnimationFrame from inside a callback affects only not-yet-moved future
// entries; cancelling a sibling of the currently-firing batch is a no-op.
void fireAnimationFrames() {
    if (g_host->rafPending.empty()) return;

    std::vector<RafEntry> current = std::move(g_host->rafPending);
    g_host->rafPending.clear();

    for (RafEntry& entry : current) {
        dom::Document* targetDoc = entry.doc;
        dom::Document* prevDoc = currentHostDocument();
        bool swapDoc = (targetDoc && targetDoc != prevDoc);

        ev::GlobalValue docG = ev::globalValue("document");
        // A Persistent, not a raw Value: the callback may allocate enough to
        // move the heap, and the restore must name the document's CURRENT
        // address; `globalThis` is re-read after the call for the same reason.
        ev::Persistent prevDocVal(docG.found ? docG.value : ev::null());

        if (swapDoc) {
            enterRealmScope(scopeIdForDocument(targetDoc));
            setCurrentHostDocument(targetDoc);
            ev::Persistent subDocVal(hostDocumentValue(targetDoc));
            ev::registerGlobal("document", subDocVal.get());
            ev::GlobalValue gt = ev::globalValue("globalThis");
            if (gt.found && ev::isObject(gt.value)) {
                ev::setProperty(gt.value, "document", subDocVal.get());
            }
        }

        Value ts = ev::fromDouble(g_host->clockMs);
        ev::CallResult r = ev::call(entry.fn.get(), ev::undefined(),
                                    std::span<const Value>(&ts, 1));

        if (swapDoc) {
            if (!ev::isNull(prevDocVal.get())) {
                ev::registerGlobal("document", prevDocVal.get());
                ev::GlobalValue gt = ev::globalValue("globalThis");
                if (gt.found && ev::isObject(gt.value)) {
                    ev::setProperty(gt.value, "document", prevDocVal.get());
                }
            }
            setCurrentHostDocument(prevDoc);
            exitRealmScope();
        }

        if (r.thrown) reportBronzeError("requestAnimationFrame", r.value);
    }
}

// ---------------------------------------------------------------------------
// The frame seam
// ---------------------------------------------------------------------------

// Everything this layer does per frame, in one place, fired from
// Engine::onFrame — which the engine calls at the point its own rAF fires
// (engine_frame.cpp step 3a, headless_api.cpp's advanceTime step, and the
// server tick), under rAF's pause gate, with the delta the scaled clock
// actually advanced by.
//
// THE ORDER, and what each position is answering to:
//
//  1. A leftover drain. Any task that enqueued a bronze job after we returned
//     last frame — e.g. a listener dispatched during input processing — is
//     drained here before advancing time and firing frame callbacks.
//     Draining here costs a queue check when there is nothing to do and ensures
//     jobs are processed without waiting an extra frame.
//     It matters because an unhandled rejection is only REPORTED at quiescence:
//     nobody ever hears about.
//
//  2. The clock. Advanced before anything reads it, so a timer deadline, an
//     rAF timestamp and performance.now() inside one frame all agree.
//
//  3. Host tasks — image loads. Before rAF, because that is where the web
//     runs a load event relative to the rendering steps, and because it lets
//     a texture that finished decoding be uploaded by the very frame that
//     learns about it rather than the next one. brokit's polled completions
//     (fetch, WebSocket, sockets, fs.watch; host_brokit.cpp) are host tasks
//     of the same kind and are pumped in the same position.
//
//  4. Timers, then 5. rAF. The order bro's own loop uses (timers_->tick at
//     step 2, fireAnimationFrames at step 3a).
//
//  6. The microtask checkpoint. AFTER rAF, not before: an rAF callback is the
//     main producer of promise jobs in a render loop — three.js's own
//     `renderer.setAnimationLoop` body, every `await` an app puts in its frame
//     function — and draining before it would leave every one of those jobs
//     queued until the next frame. That is not merely late: each frame would
//     run the PREVIOUS frame's continuations against this frame's state, and a
//     rejection thrown in the last rAF before shutdown would never be reported
//     at all, because quiescence would never be reached again.
//
//  6b. The observer pass — ResizeObserver, then IntersectionObserver
//     (js/observers.js, over host_observer_hooks.cpp). After the checkpoint,
//     which is where the web's "update the rendering" puts them: a mutation
//     made in an rAF callback has been reported (MutationObserver delivers
//     from a microtask) before the box it changed is measured, so a resize
//     entry describes the box as it ended up rather than mid-edit. The pass
//     reads geometry, and a geometry read lays the document out first, so
//     "after layout" is what it measures. Its own callbacks' promise jobs
//     drain at 6c.
void hostFrame(double dtMs) {
    if (ev::microtasksPending()) ev::drainMicrotasks();  // 1
    g_host->clockMs += dtMs;                             // 2
    drainHostTasks();                                    // 3
    pumpBrokitTicks();                                   // 3b
    drainWorkerMessages();                               // 3c
    pollNet();                                           // 3d
    drainSteamEvents();                                  // 3e
    drainHostWindowMessages();                           // 3f
    fireHostTimers(g_host->clockMs);                     // 4
    fireAnimationFrames();                               // 5
    ev::drainMicrotasks();                               // 6
    fireHostObserverFrame();                             // 6b
    deliverWebAnimationFinishEvents();
    ev::drainMicrotasks();                               // 6c
    hostNotifyIdleFrame(dtMs);                           // 7
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

// ---------------------------------------------------------------------------
// Free-function globals
// ---------------------------------------------------------------------------

Value makeRequestAnimationFrame() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            Value fn = argAt(a, 0);
            if (!ev::isFunction(fn)) {
                return ev::throwTypeError(
                    "requestAnimationFrame: first argument must be a function");
            }
            int32_t id = g_host->nextRafId++;
            dom::Document* curDoc = currentHostDocument() ? currentHostDocument() : (g_host->engine ? g_host->engine->document() : nullptr);
            g_host->rafPending.push_back({id, curDoc, ev::Persistent(fn)});
            return ev::fromDouble(id);
        },
        1);
}

Value makeCancelAnimationFrame() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            int32_t id = i32At(a, 0);
            auto& pending = g_host->rafPending;
            for (auto it = pending.begin(); it != pending.end(); ++it) {
                if (it->id == id) {
                    pending.erase(it);
                    break;
                }
            }
            return ev::undefined();
        },
        1);
}

Value makePerformanceValue() {
    ObjectBuilder b;
    // The rAF clock, so performance.now() and rAF timestamps agree — the
    // invariant three.js's Clock leans on. Advances only with frames, which
    // is also what keeps it honest under bro.time pause and headless virtual
    // time.
    b.def("now", 0, [](Value, std::span<const Value>) {
        return ev::fromDouble(g_host->clockMs);
    });
    return b.get();
}

}  // namespace

void clearHostAnimationFramesForDocument(dom::Document* doc) {
    if (!g_host || !doc) return;
    auto it = std::remove_if(g_host->rafPending.begin(), g_host->rafPending.end(),
                             [doc](const RafEntry& e) { return e.doc == doc; });
    g_host->rafPending.erase(it, g_host->rafPending.end());
}

// The same surface the `document` global has — createElement, the queries, the
// element accessors, the event-target delegation — bound to `doc` instead of to
// whatever the engine is showing. host_parser.cpp hands DOMParser results
// through here, which is why it exists at all: the builder itself is private to


Value makeBrandConstructor(const char* name) {
    std::string msg = std::string("bronze host ") + name +
                      ": an instanceof brand only, not constructible";
    return ev::makeFunction(
        [msg](Value, std::span<const Value>) { return ev::throwTypeError(msg); }, 0);
}

// Where an exception out of compiled code ends up. Reports to the log stream.
// The Error's own fields are read through embed property reads — the
// throw was already caught, so running a getter here is safe.
void reportBronzeError(const char* origin, Value thrown) {
    if (!ev::isObject(thrown)) {
        LOG_ERROR("[bronze:%s] uncaught: %s", origin, ev::toUtf8(thrown).c_str());
        return;
    }
    ev::Persistent root(thrown);
    Value nameV = ev::getProperty(root.get(), "name");
    Value msgV = ev::getProperty(root.get(), "message");
    std::string name = ev::isObject(nameV) || ev::isUndefined(nameV)
                           ? std::string("Error")
                           : ev::toUtf8(nameV);
    std::string msg =
        ev::isObject(msgV) || ev::isUndefined(msgV) ? std::string() : ev::toUtf8(msgV);
    LOG_ERROR("[bronze:%s] uncaught %s: %s", origin, name.c_str(), msg.c_str());
}

// Zero before the first frame, which is what a program's top level sees. It is
// an ELAPSED clock, not a wall clock: three.js's Clock only ever subtracts two
// readings, and an origin of zero keeps a headless run's output free of the one
// number that would differ every time it ran.
double hostClockMs() { return g_host ? g_host->clockMs : 0.0; }

engine::Engine* hostEngine() { return g_host ? g_host->engine : nullptr; }



// Identity — the value the program already holds for `el`, so
// `event.target === canvas` is true inside a compiled listener. It asks the
// element registry (host_element.cpp), which is every element this layer ever
// wrapped, not just the canvases: a UI tests target identity on every element
// it built, not only on the one it draws into.
Value hostValueForElement(dom::Element* el) {
    if (!g_host || !el) return ev::undefined();
    return hostElementValue(el);
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------
static void snapshotBaselineGlobalProps();

void installWebHostGlobals(engine::Engine& engine) {
    if (g_host) {
        LOG_WARN("bronze_host: installWebHostGlobals called twice; ignoring");
        return;
    }
    // Every registerGlobal below, and in the install* calls at the foot, is
    // the whole contract: registeredHostGlobals() reads the names back off
    // bronze's registry after this returns, and that is the list every
    // compile is given (eval_jit.cpp, host_worker.cpp, and `bro-headless
    // --print-host-globals` for an ahead-of-time `bronze build`). There is
    // no manifest to keep in step. registerGlobal roots each value for the
    // life of the process.
    //
    // Never freed — see the lifetime note at the top of this file.
    g_host = new HostState();
    g_host->engine = &engine;
    if (engine.document()) {
        engine.document()->setElementClonedCallback(&fireElementCloned);
    }

    // Install dynamic evaluation and function hooks (eval, new Function)
    // into bronze before anything the compiled program can run.
    installDynamicHooks(engine);

    // The frame hook, registered exactly once (Engine::onFrame callbacks are
    // never unregistered). It fires at the point the engine's own rAF fires,
    // with rAF's pause semantics, in every display mode. hostFrame above owns
    // the ordering, drain included, and says why each step sits where it does.
    engine.onFrame([](double dtMs) { hostFrame(dtMs); });

    // Install HTML interfaces BEFORE document is created:
    installHtmlInterfaces();

    {
        // Null: the global follows the engine's current document rather than
        // naming one. documentFor() above has the reason.
        Value doc = makeDocumentValue(nullptr);
        ev::registerGlobal("document", doc);
        ev::registerGlobal("Document", documentHostClass().constructor());
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) {
            ev::setProperty(gt.value, "document", doc);
            ev::setProperty(gt.value, "Document", documentHostClass().constructor());
        }
    }
    {
        ev::GlobalValue gt = ev::globalValue("globalThis");
        Value gObj = gt.value;

        ev::registerGlobal("window", gObj);
        ev::registerGlobal("self", gObj);
        ev::registerGlobal("top", gObj);
        ev::registerGlobal("parent", gObj);
        ev::setProperty(gObj, "window", gObj);
        ev::setProperty(gObj, "self", gObj);
        ev::setProperty(gObj, "top", gObj);
        ev::setProperty(gObj, "parent", gObj);

        ObjectBuilder b(gObj);
        engine::Engine* enginePtr = g_host->engine;

        b.accessor("devicePixelRatio",
                   [enginePtr](Value, std::span<const Value>) {
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
            ev::Persistent self(thisValue);
            Value typeV = argAt(a, 0);
            Value fn = argAt(a, 1);
            if (ev::isObject(typeV) || ev::isUndefined(typeV)) {
                return ev::throwTypeError("window.addEventListener: type must be a string");
            }
            if (!ev::isFunction(fn)) {
                return ev::throwTypeError(
                    "window.addEventListener: listener must be a function");
            }
            std::string type = ev::toUtf8(typeV);
            dom::ListenerOptions opts = readOptions(argAt(a, 2));
            ev::Persistent fnP(fn);
            dom::Document* targetDoc = currentHostDocument() ? currentHostDocument() : (enginePtr ? enginePtr->document() : nullptr);
            if (type == "message") {
                if (targetDoc && enginePtr) {
                    if (auto* wh = enginePtr->windowHostForDocument(targetDoc)) {
                        addWindowHostChildMessageListener(wh->id, fn);
                    }
                }
            }
            if (!targetDoc) {
                return ev::throwError(
                    "window.addEventListener: the engine refused the registration");
            }
            std::string origin = "window " + type + " listener";
            dom::ListenerHandle handle = targetDoc->windowListeners().add(
                type, [fnP, self, origin, targetDoc](dom::Event& evt) {
                    callBronzeListener(fnP, self, evt, origin.c_str(), targetDoc);
                }, opts);
            if (!handle) {
                return ev::throwError(
                    "window.addEventListener: the engine refused the registration");
            }
            g_host->windowListeners.push_back({handle, std::move(type), std::move(fnP)});
            return ev::undefined();
        });
        b.def("dispatchEvent", 1, [](Value, std::span<const Value> a) {
            return hostDispatchToWindow(argAt(a, 0));
        });
        b.def("removeEventListener", 3, [enginePtr](Value, std::span<const Value> a) {
            Value typeV = argAt(a, 0);
            Value fn = argAt(a, 1);
            if (ev::isObject(typeV)) return ev::undefined();
            std::string type = ev::toUtf8(typeV);
            dom::Document* targetDoc = currentHostDocument() ? currentHostDocument() : (enginePtr ? enginePtr->document() : nullptr);
            if (type == "message") {
                if (targetDoc && enginePtr) {
                    if (auto* wh = enginePtr->windowHostForDocument(targetDoc)) {
                        removeWindowHostChildMessageListener(wh->id, fn);
                    }
                }
            }
            auto& list = g_host->windowListeners;
            for (auto it = list.begin(); it != list.end(); ++it) {
                if (it->type == type && ev::toBits(it->fn.get()) == ev::toBits(fn)) {
                    if (targetDoc) targetDoc->windowListeners().remove(it->handle);
                    list.erase(it);
                    break;
                }
            }
            return ev::undefined();
        });

        Value getComputedStyleFn = makeGetComputedStyle();
        b.set("getComputedStyle", getComputedStyleFn);
        ev::registerGlobal("getComputedStyle", getComputedStyleFn);
        ev::registerGlobal("close", ev::getProperty(gObj, "close"));

        Value ls = makeLocalStorageValue();
        b.set("localStorage", ls);
        ev::registerGlobal("localStorage", ls);

        Value ss = makeSessionStorageValue();
        b.set("sessionStorage", ss);
        ev::registerGlobal("sessionStorage", ss);

        Value screenVal = makeScreenValue();
        b.set("screen", screenVal);
        ev::registerGlobal("screen", screenVal);

        Value getSelectionFn = ev::makeFunction([](Value, std::span<const Value>) {
            auto* e = hostEngine();
            if (!e || !e->document()) return ev::null();
            return wrapSelection(e->document()->selection());
        }, 0, "getSelection");
        b.set("getSelection", getSelectionFn);
        {
            ev::GlobalValue gt = ev::globalValue("globalThis");
            if (gt.found && ev::isObject(gt.value)) ev::setProperty(gt.value, "getSelection", getSelectionFn);
        }

        b.def("open", 1, [](Value, std::span<const Value> a) {
            if (a.empty() || ev::isUndefined(a[0]) || ev::isNull(a[0])) return ev::null();
            std::string url = ev::toUtf8(a[0]);
            if (!url.empty()) {
                auto* e = hostEngine();
                if (e && e->displayMode() == engine::DisplayMode::Headless) {
                    LOG_INFO("window.open('%s'): suppressed in headless mode", url.c_str());
                }
            }
            return ev::null();
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

        Value matchMediaFn = ev::makeFunction(
            [](Value, std::span<const Value> a) -> Value {
                std::string query = a.empty() || ev::isUndefined(a[0]) ? "" : ev::toUtf8(a[0]);
                return makeHostMatchMediaObject(query);
            },
            1);
        b.set("matchMedia", matchMediaFn);
        ev::registerGlobal("matchMedia", matchMediaFn);

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
        auto doWindowScrollTo = [](double, double y) {
            auto* e = hostEngine();
            if (e && e->document() && e->document()->documentElement()) {
                e->document()->documentElement()->setScrollTopValue(static_cast<float>(y));
            }
        };

        b.accessor("scrollX", [](Value, std::span<const Value>) { return ev::fromDouble(0.0); }, nullptr);
        b.accessor("pageXOffset", [](Value, std::span<const Value>) { return ev::fromDouble(0.0); }, nullptr);
        b.accessor("scrollY", [](Value, std::span<const Value>) {
            auto* e = hostEngine();
            float y = e ? e->viewportScrollY() : 0.0f;
            if (y == 0.0f && e && e->document() && e->document()->documentElement()) {
                y = e->document()->documentElement()->scrollTopValue();
            }
            return ev::fromDouble(y);
        }, nullptr);
        b.accessor("pageYOffset", [](Value, std::span<const Value>) {
            auto* e = hostEngine();
            float y = e ? e->viewportScrollY() : 0.0f;
            if (y == 0.0f && e && e->document() && e->document()->documentElement()) {
                y = e->document()->documentElement()->scrollTopValue();
            }
            return ev::fromDouble(y);
        }, nullptr);

        b.def("scrollTo", 2, [parseScrollArgs, doWindowScrollTo](Value, std::span<const Value> a) {
            double x = 0, y = 0;
            parseScrollArgs(a, x, y);
            doWindowScrollTo(x, y);
            return ev::undefined();
        });
        b.def("scroll", 2, [parseScrollArgs, doWindowScrollTo](Value, std::span<const Value> a) {
            double x = 0, y = 0;
            parseScrollArgs(a, x, y);
            doWindowScrollTo(x, y);
            return ev::undefined();
        });
        b.def("scrollBy", 2, [parseScrollArgs, doWindowScrollTo](Value, std::span<const Value> a) {
            double dx = 0, dy = 0;
            parseScrollArgs(a, dx, dy);
            double curY = 0;
            auto* e = hostEngine();
            if (e && e->document() && e->document()->documentElement()) {
                curY = e->document()->documentElement()->scrollTopValue();
            }
            doWindowScrollTo(0, curY + dy);
            return ev::undefined();
        });

        ev::registerGlobal("addEventListener", ev::getProperty(gObj, "addEventListener"));
        ev::registerGlobal("removeEventListener", ev::getProperty(gObj, "removeEventListener"));
        ev::registerGlobal("dispatchEvent", ev::getProperty(gObj, "dispatchEvent"));
        ev::registerGlobal("scrollTo", ev::getProperty(gObj, "scrollTo"));
        ev::registerGlobal("scroll", ev::getProperty(gObj, "scroll"));
        ev::registerGlobal("scrollBy", ev::getProperty(gObj, "scrollBy"));
        ev::registerGlobal("open", ev::getProperty(gObj, "open"));
        ev::registerGlobal("focus", ev::getProperty(gObj, "focus"));
        ev::registerGlobal("blur", ev::getProperty(gObj, "blur"));

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
            Value locVal = loc.get();
            b.set("location", locVal);
            ev::registerGlobal("location", locVal);
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
            Value histVal = hist.get();
            b.set("history", histVal);
            ev::registerGlobal("history", histVal);
        }
    }
    {
        Value raf = makeRequestAnimationFrame();
        ev::registerGlobal("requestAnimationFrame", raf);
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) ev::setProperty(gt.value, "requestAnimationFrame", raf);
    }
    {
        Value caf = makeCancelAnimationFrame();
        ev::registerGlobal("cancelAnimationFrame", caf);
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) ev::setProperty(gt.value, "cancelAnimationFrame", caf);
    }
    {
        Value perf = makePerformanceValue();
        ev::registerGlobal("performance", perf);
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) ev::setProperty(gt.value, "performance", perf);
    }
    {
        // `typeof WebGL2RenderingContext !== 'undefined'` must hold, and
        // gl.constructor.name (gl_context.cpp) carries the instance half of
        // three.js's sniff. A bare named object is all the sniff reads.
        ObjectBuilder ctor;
        Value name = ev::fromUtf8("WebGL2RenderingContext");
        ctor.set("name", name);
        ev::registerGlobal("WebGL2RenderingContext", ctor.get());
    }

    // The families that own their own files, each registering the names
    // the manifest lists for it, in the manifest's order.
    installTimerGlobals();
    installPlatformGlobals();
    // AFTER installPlatformGlobals, which is where installElementGlobals runs:
    // `Image` is an element class and chains its prototype onto Element's, so
    // Element's has to exist first (host_element_image.cpp).
    installImageGlobal();
    installParserGlobal();

    {
        Value nav = makeNavigatorValue();
        ObjectBuilder bNav(nav);
        ObjectBuilder clip;
        clip.def("__read", 0, [](Value, std::span<const Value>) {
            return ev::fromUtf8(bro::platform::getClipboardText());
        });
        clip.def("__write", 1, [](Value, std::span<const Value> a) {
            Value textV = argAt(a, 0);
            std::string text = (!ev::isObject(textV) && !ev::isUndefined(textV)) ? ev::toUtf8(textV) : "";
            bool ok = bro::platform::setClipboardText(text);
            return ev::fromBool(ok);
        });
        clip.def("readText", 0, [](Value, std::span<const Value>) {
            Value p = ev::createPromise();
            ev::resolvePromise(p, ev::fromUtf8(bro::platform::getClipboardText()));
            return p;
        });
        clip.def("writeText", 1, [](Value, std::span<const Value> a) {
            Value textV = argAt(a, 0);
            std::string text = (!ev::isObject(textV) && !ev::isUndefined(textV)) ? ev::toUtf8(textV) : "";
            bool ok = bro::platform::setClipboardText(text);
            Value p = ev::createPromise();
            if (ok) {
                ev::resolvePromise(p, ev::undefined());
            } else {
                Value msg = ev::fromUtf8("clipboard write failed");
                ev::CallResult err = ev::construct(ev::globalValue("Error").value,
                                                   std::span<const Value>(&msg, 1));
                ev::rejectPromise(p, err.value);
            }
            return p;
        });
        bNav.set("clipboard", clip.get());

        ev::registerGlobal("navigator", nav);
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) ev::setProperty(gt.value, "navigator", nav);
    }
    // HTMLCanvasElement and HTMLImageElement are installed as real classes
    // via installHtmlInterfaces() / installImageGlobal().
    ev::registerGlobal("WebGLRenderingContext", makeBrandConstructor("WebGLRenderingContext"));
    installTouchGlobals();
    installVendorGlobals();
    installBrokitGlobals(engine);
    // The `bro` / `__bro` roots, the natives under `__bro_native`, and
    // js/bro_core.js over them (host_bro_root.cpp). Nothing before this
    // point registers `bro`; a later `bro.*` namespace mounts onto the
    // object this creates.
    installBroRoots(engine);
#if BRO_WITH_3D
    // bro.mesh, Mesh and MeshBVH (js/mesh.js over native_mesh.cpp): after
    // the roots, whose `bro.mesh` and `__bro_native.mesh` it fills.
    installMeshModule();
    installRiggingModule();
    installPhysicsModule();
    installTerrainModule();
    installClipmapModule();
    installTileWorldModule();
    installLightingModule();
    installGizmoModule();
    installAnimationModule();
    installSceneModule();
#endif
    installNetModule();
    installLmModule();
    installRaveModule();
    installMotionModule();
    installMicModule();
    installSenseModule();
    installGestureModule();
    installWakeModule();
    installKwsModule();
    installListenModule();
    installTriposplatModule();
    installDiffusionModule();
    installVisionModule();
    installDiarModule();
    installSttModule();
    installTtsModule();
    installFloraModule();
    installTensorModule();
    installImpostorModule();
    // bro's own compiled JavaScript (host_js_modules.cpp), after brokit:
    // observers.js reads queueMicrotask, performance and getComputedStyle
    // off globalThis at the point of use, and every name a module lists in
    // js/module.globals must already be registered when its entry runs.
    installObserversModule();
    installNetSyncModule();
    installImageGpuModule();
    installHeadlessGlobals(engine);
    installPlatformExtensions(engine);
    installRangeGlobals();
    installSelectionGlobals();
    installIntlGlobals();
    installWebAnimationGlobals();
    installAudioGlobals();
    installAIGlobals();
    installMathGlobals();
    installVideoGlobals();
    initHostCalleeNamer();

    snapshotBaselineGlobalProps();
}

static std::vector<std::string> s_baselineGlobalProps;

static void snapshotBaselineGlobalProps() {
    initRealmScopeBaseline();
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return;
    Value objCtor = ev::globalValue("Object").value;
    Value getOwnPropertyNamesFn = ev::getProperty(objCtor, "getOwnPropertyNames");
    if (ev::isFunction(getOwnPropertyNamesFn)) {
        ev::CallResult res = ev::call(getOwnPropertyNamesFn, objCtor, std::span<const Value>(&gt.value, 1));
        if (!res.thrown) {
            Value namesArr = res.value;
            Value lenVal = ev::getProperty(namesArr, "length");
            int len = static_cast<int>(ev::toDouble(lenVal));
            s_baselineGlobalProps.clear();
            for (int i = 0; i < len; ++i) {
                Value k = ev::getElement(namesArr, i);
                s_baselineGlobalProps.push_back(ev::toUtf8(k));
            }
        }
    }
}

void resetGlobalExpandos() {
    resetAllRealmScopes();
    setCurrentHostDocument(nullptr);
    if (g_host) {
        g_host->rafPending.clear();
        g_host->windowListeners.clear();
    }
    resetWindowHostOpenState();
    cleanupSteamBindings();
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        Value objCtor = ev::globalValue("Object").value;
        Value getOwnPropertyNamesFn = ev::getProperty(objCtor, "getOwnPropertyNames");
        Value reflect = ev::globalValue("Reflect").value;
        Value deletePropFn = ev::isObject(reflect) ? ev::getProperty(reflect, "deleteProperty") : ev::undefined();
        if (ev::isFunction(getOwnPropertyNamesFn)) {
            ev::CallResult res = ev::call(getOwnPropertyNamesFn, objCtor, std::span<const Value>(&gt.value, 1));
            if (!res.thrown) {
                Value namesArr = res.value;
                Value lenVal = ev::getProperty(namesArr, "length");
                int len = static_cast<int>(ev::toDouble(lenVal));
                std::unordered_set<std::string> baseline(s_baselineGlobalProps.begin(), s_baselineGlobalProps.end());
                for (int i = 0; i < len; ++i) {
                    Value k = ev::getElement(namesArr, i);
                    std::string key = ev::toUtf8(k);
                    if (baseline.find(key) == baseline.end()) {
                        if (ev::isFunction(deletePropFn)) {
                            Value args[2] = { gt.value, k };
                            ev::call(deletePropFn, reflect, args);
                        }
                        ev::setProperty(gt.value, key, ev::undefined());
                    }
                }
            }
        }
        if (ev::isFunction(deletePropFn)) {
            Value c1 = ev::fromUtf8("__reloadCanary");
            Value c2 = ev::fromUtf8("__afterReloadCall");
            Value a1[2] = { gt.value, c1 };
            Value a2[2] = { gt.value, c2 };
            ev::call(deletePropFn, reflect, a1);
            ev::call(deletePropFn, reflect, a2);
        }
        ev::setProperty(gt.value, "__reloadCanary", ev::undefined());
        ev::setProperty(gt.value, "__afterReloadCall", ev::undefined());
    }
}

bool isWebHostGlobalsInstalled() {
    return g_host != nullptr;
}

std::vector<std::string> registeredHostGlobals() {
    return ev::hostGlobalNames();
}

bool hasPendingAnimationFrames() {
    return g_host && !g_host->rafPending.empty();
}

}  // namespace bro::bronze_host
