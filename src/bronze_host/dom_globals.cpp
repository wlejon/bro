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
#include "bronze_host/host_interp.h"

#include "engine/engine.h"
#include "platform/sdl_window.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_target.h"
#include "util/log.h"

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

struct CanvasState {
    dom::Element* el = nullptr;  // owned by the Document
    webgl::WebGL2RenderingContext* glCtx = nullptr;  // owned by the Engine
    ev::Persistent jsObj;  // the host canvas object handed to the program
    ev::Persistent glObj;  // the B2 context object, once getContext ran
    ev::Persistent ctx2dObj;
    bool hasGl = false;
    // No listener list here: canvas listeners live in the ENGINE's native
    // listener list on `el`, which is what makes them fire from a real click
    // (host_dom_events.cpp).
};

struct RafEntry {
    int32_t id;
    ev::Persistent fn;
};

struct WindowListener {
    dom::ListenerHandle handle;
    std::string type;
    ev::Persistent fn;
};

struct HostState {
    engine::Engine* engine = nullptr;
    // unique_ptr entries so CanvasState addresses stay stable while the
    // vector grows — the accessors' lambdas capture raw CanvasState*.
    std::vector<std::unique_ptr<CanvasState>> canvases;
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

// REENTRANCY: the pending list is MOVED OUT before any callback runs —
// mirroring js::Timers::fireAnimationFrames — so a callback that registers
// another rAF appends to the fresh list (fires next frame), and a
// cancelAnimationFrame from inside a callback affects only not-yet-moved future
// entries; cancelling a sibling of the currently-firing batch is a no-op,
// exactly as it is in bro's own rAF.
void fireAnimationFrames() {
    if (g_host->rafPending.empty()) return;

    std::vector<RafEntry> current = std::move(g_host->rafPending);
    g_host->rafPending.clear();

    for (RafEntry& entry : current) {
        Value ts = ev::fromDouble(g_host->clockMs);
        ev::CallResult r = ev::call(entry.fn.get(), ev::undefined(),
                                    std::span<const Value>(&ts, 1));
        // Report once, keep going: one broken callback must not silence its
        // siblings or tear the loop down (web semantics).
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
//  1. A leftover drain. bro's loop drains QuickJS TWICE — once right after rAF
//     (step 3b) and again after the late pumps that can resolve a promise
//     (framePumps_ + tickAsync). This layer is fired at the FIRST of those two
//     seams and there is no host hook at the second, so anything that could
//     enqueue a bronze job after we return this frame — any listener the
//     engine dispatches during the next frame's event poll, on the window or
//     on an element — would otherwise wait a whole frame to be seen. That is
//     no longer hypothetical: a compiled click handler runs inside the input
//     pipeline (host_dom_events.cpp), well outside this seam. Draining here
//     costs a queue check when there is nothing to do and bounds that wait at
//     one frame instead of forever. It matters because an unhandled rejection
//     is only REPORTED at quiescence: a drain that never runs is a rejection
//     nobody ever hears about.
//
//  2. The clock. Advanced before anything reads it, so a timer deadline, an
//     rAF timestamp and performance.now() inside one frame all agree.
//
//  3. Host tasks — image loads, XHR completions. Before rAF, because that is
//     where the web runs a load event relative to the rendering steps, and
//     because it lets a texture that finished decoding be uploaded by the very
//     frame that learns about it rather than the next one.
//
//  4. Timers, then 5. rAF. The order bro's own loop uses (timers_->tick at
//     step 2, fireAnimationFrames at step 3a).
//
//  5b. Mutation records. After rAF because an rAF callback is where a compiled
//     app does most of its DOM work, and an observer told about it in the same
//     frame is an observer that can still act before the frame is drawn.
//     Before the checkpoint for the same reason step 6 is where it is: whatever
//     the callback starts should settle with the rest of this frame's jobs.
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
//  7. The bridge sweep, after the checkpoint: the drain runs bronze's deferred
//     finalizers, which is when a crossing's own-side wrapper can die, and the
//     sweep is what turns those deaths into freed rows (host_interp.h).
void hostFrame(double dtMs) {
    if (ev::microtasksPending()) ev::drainMicrotasks();  // 1
    g_host->clockMs += dtMs;                             // 2
    drainHostTasks();                                    // 3
    drainNetEvents();                                    // 3b
    fireHostTimers(g_host->clockMs);                     // 4
    drainPhysicsContactEvents();                         // 4b
    fireAnimationFrames();                               // 5
    deliverHostObservers();                              // 5b
    ev::drainMicrotasks();                               // 6
    sweepInterpBridge();                                 // 7
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

int attributeOr(dom::Element* el, const char* name, int fallback) {
    const std::string& v = el->getAttribute(name);
    return v.empty() ? fallback : std::atoi(v.c_str());
}

// Per HTML the width/height ATTRIBUTES are the drawing-buffer size; 300x150
// is the spec default for a canvas that never set them.
int canvasWidthOf(CanvasState* cs) {
    if (cs->glCtx) return cs->glCtx->canvasWidth();
    return attributeOr(cs->el, "width", 300);
}

int canvasHeightOf(CanvasState* cs) {
    if (cs->glCtx) return cs->glCtx->canvasHeight();
    return attributeOr(cs->el, "height", 150);
}

Value makeCanvasValue(dom::Element* el) {
    auto owned = std::make_unique<CanvasState>();
    CanvasState* cs = owned.get();
    cs->el = el;
    g_host->canvases.push_back(std::move(owned));

    // A canvas IS an element — it lives in the tree, carries classes, is styled
    // and is measured — and then it also owns a drawing buffer. The element half
    // is the shared core (host_element.cpp); what follows overrides only the
    // handful of members the drawing buffer changes the answer to.
    ObjectBuilder b(makeElementHandleObject(el));
    installElementCore(b, el);

    // width/height: reads answer the live drawing-buffer size; a write is
    // both the attribute (the HTML source of truth the engine's
    // syncWebGLCanvasSizes respects — an app that sets them owns the size)
    // and, once a GL context exists, an immediate FBO resize.
    b.accessor("width",
               [cs](Value, std::span<const Value>) {
                   return ev::fromDouble(canvasWidthOf(cs));
               },
               [cs](Value, std::span<const Value> a) {
                   int w = i32At(a, 0);
                   cs->el->setAttribute("width", std::to_string(w));
                   if (cs->glCtx) cs->glCtx->resize(w, cs->glCtx->canvasHeight());
                   return ev::undefined();
               });
    b.accessor("height",
               [cs](Value, std::span<const Value>) {
                   return ev::fromDouble(canvasHeightOf(cs));
               },
               [cs](Value, std::span<const Value> a) {
                   int h = i32At(a, 0);
                   cs->el->setAttribute("height", std::to_string(h));
                   if (cs->glCtx) cs->glCtx->resize(cs->glCtx->canvasWidth(), h);
                   return ev::undefined();
               });

    // clientWidth/clientHeight: an honest layout read, flushed first so an
    // element appended and measured in one turn measures correctly (the
    // Engine::flushLayoutForRead contract). A canvas with no box yet answers
    // its drawing-buffer size, which is what a just-created offscreen canvas
    // is on the web too.
    b.accessor("clientWidth",
               [cs](Value, std::span<const Value>) {
                   g_host->engine->flushLayoutForRead(cs->el->document());
                   auto& box = cs->el->layoutBox();
                   double w = box.contentRect.width;
                   return ev::fromDouble(w > 0 ? w : canvasWidthOf(cs));
               },
               nullptr);
    b.accessor("clientHeight",
               [cs](Value, std::span<const Value>) {
                   g_host->engine->flushLayoutForRead(cs->el->document());
                   auto& box = cs->el->layoutBox();
                   double h = box.contentRect.height;
                   return ev::fromDouble(h > 0 ? h : canvasHeightOf(cs));
               },
               nullptr);

    b.def("getBoundingClientRect", 0, [cs](Value, std::span<const Value>) {
        g_host->engine->flushLayoutForRead(cs->el->document());
        auto& box = cs->el->layoutBox();
        double w = box.contentRect.width > 0 ? box.contentRect.width : canvasWidthOf(cs);
        double h = box.contentRect.height > 0 ? box.contentRect.height : canvasHeightOf(cs);
        double x = box.contentRect.x;
        double y = box.contentRect.y;
        ObjectBuilder r;
        r.set("left", ev::fromDouble(x));
        r.set("top", ev::fromDouble(y));
        r.set("right", ev::fromDouble(x + w));
        r.set("bottom", ev::fromDouble(y + h));
        r.set("width", ev::fromDouble(w));
        r.set("height", ev::fromDouble(h));
        r.set("x", ev::fromDouble(x));
        r.set("y", ev::fromDouble(y));
        return r.get();
    });
    b.def("setAttribute", 2, [cs](Value, std::span<const Value> a) {
        Value nameV = argAt(a, 0);
        Value valV = argAt(a, 1);
        if (!ev::isObject(nameV) && !ev::isUndefined(nameV)) {
            std::string name = ev::toUtf8(nameV);
            std::string val = (!ev::isObject(valV) && !ev::isUndefined(valV)) ? ev::toUtf8(valV) : "";
            cs->el->setAttribute(name, val);
            if (name == "width") {
                int w = std::atoi(val.c_str());
                if (cs->glCtx) cs->glCtx->resize(w, cs->glCtx->canvasHeight());
            } else if (name == "height") {
                int h = std::atoi(val.c_str());
                if (cs->glCtx) cs->glCtx->resize(cs->glCtx->canvasWidth(), h);
            }
        }
        return ev::undefined();
    });
    // getContext('webgl2'|'webgl') → the B2 context object, built over the
    // SAME Engine path the QuickJS factory takes (Engine::createWebGL2Context
    // — one construction path, no drift), and cached so a second call answers
    // the same object, per spec. '2d' answers a CanvasRenderingContext2D wrapper.
    b.def("getContext", 1, [cs](Value, std::span<const Value> a) {
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV)) return ev::null();
        std::string type = ev::toUtf8(typeV);
        if (type == "2d") {
            if (ev::isObject(cs->ctx2dObj.get())) return cs->ctx2dObj.get();
            Value ctx2d = makeCanvas2DContextValue(cs->jsObj.get());
            cs->ctx2dObj.set(ctx2d);
            return ctx2d;
        }
        if (type != "webgl2" && type != "webgl") return ev::null();
        if (cs->hasGl) return cs->glObj.get();
        webgl::WebGL2RenderingContext* ctx = g_host->engine->createWebGL2Context(cs->el);
        if (!ctx) return ev::null();
        cs->glCtx = ctx;
        Value glValue = createGlContextValue(ctx, cs->jsObj.get());
        cs->glObj.set(glValue);
        cs->hasGl = true;
        return cs->glObj.get();
    });

    Value built = b.get();
    cs->jsObj.set(built);
    noteHostElementValue(el, built);
    return built;
}

// "Which host canvas is this Value?" — through the element handle every
// wrapper now carries, rather than by comparing raw Value addresses. The old
// compare was correct only while nothing allocated during the scan; this asks
// the value what element it is and looks that up, which has no such condition.
CanvasState* canvasFor(Value v) {
    dom::Element* el = hostElementOf(v);
    if (!el) return nullptr;
    for (auto& cs : g_host->canvases) {
        if (cs->el == el) return cs.get();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// generic elements
// ---------------------------------------------------------------------------

Value makeGenericElementValue(dom::Element* el) {
    return makePlainElementValue(el);
}

Value wrapElement(dom::Element* el) {
    return hostElementValue(el);
}

// Which document a wrapper speaks for.
//
// `fixed` is null for the `document` global and only for it. That global is
// registered before the engine has parsed anything, so it cannot capture a
// document — it has to ask for the current one every time, and asking is also
// what lets it survive a reparse that swaps the whole tree out. Every OTHER
// document wrapper — the ones DOMParser hands back — names one document for
// good, and naming it is the point: `parsed.getElementById('x')` must not
// quietly answer from the live page.
dom::Document* documentFor(dom::Document* fixed) {
    if (fixed) return fixed;
    return (g_host && g_host->engine) ? g_host->engine->document() : nullptr;
}

// document.createElement / createElementNS. An unknown tag is not a refusal:
// every HTML tag is a real dom::Element here, and the element surface is the
// same one for all of them. `img` gets MORE than that surface rather than a
// different one — a decoder behind `.src`, added by host_element_image.cpp on
// top of the element, because an image built this way is still a node the
// program may append.
Value createElementImpl(dom::Document* fixed, std::span<const Value> a,
                        size_t tagIndex) {
    Value tagV = argAt(a, tagIndex);
    if (ev::isObject(tagV)) return ev::throwTypeError("createElement: tag must be a string");
    std::string tag = ev::toUtf8(tagV);
    for (char& ch : tag) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    dom::Document* doc = documentFor(fixed);
    if (!doc) return ev::throwError("bronze host: engine has no document");
    dom::Element* el = doc->createElement(tag);
    if (!el) return ev::throwError("bronze host: createElement failed");
    return hostElementValue(el);
}

Value makeDocumentValue(dom::Document* fixed) {
    ObjectBuilder b;
    b.def("createElement", 1, [fixed](Value, std::span<const Value> a) {
        return createElementImpl(fixed, a, 0);
    });
    // three.js spells it createElementNS('http://www.w3.org/1999/xhtml',
    // 'canvas'); the namespace is accepted and ignored, as bro's own DOM does
    // for HTML content.
    b.def("createElementNS", 2, [fixed](Value, std::span<const Value> a) {
        return createElementImpl(fixed, a, 1);
    });
    // The other three node factories. All go through the DOCUMENT so the node
    // lands in Document::ownedNodes_ — which is what makes it freed on teardown
    // and what makes the freed-node observer the registry depends on fire for
    // it (host_element.cpp). A node allocated any other way would outlive its
    // wrapper's ability to notice it died.
    b.def("createTextNode", 1, [fixed](Value, std::span<const Value> a) {
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::throwError("bronze host: engine has no document");
        Value v = argAt(a, 0);
        std::string text =
            (ev::isObject(v) || ev::isUndefined(v)) ? "" : ev::toUtf8(v);
        return hostNodeValue(doc->createTextNode(text));
    });
    b.def("createComment", 1, [fixed](Value, std::span<const Value> a) {
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::throwError("bronze host: engine has no document");
        Value v = argAt(a, 0);
        std::string text =
            (ev::isObject(v) || ev::isUndefined(v)) ? "" : ev::toUtf8(v);
        return hostNodeValue(doc->createComment(text));
    });
    b.def("createDocumentFragment", 0, [fixed](Value, std::span<const Value>) {
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::throwError("bronze host: engine has no document");
        return hostNodeValue(doc->createDocumentFragment());
    });
    b.def("getElementById", 1, [fixed](Value, std::span<const Value> a) {
        Value idV = argAt(a, 0);
        if (ev::isObject(idV) || ev::isUndefined(idV)) return ev::null();
        std::string id = ev::toUtf8(idV);
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::null();
        dom::Element* el = doc->getElementById(id);
        return wrapElement(el);
    });
    b.def("querySelector", 1, [fixed](Value, std::span<const Value> a) {
        Value selV = argAt(a, 0);
        if (ev::isObject(selV) || ev::isUndefined(selV)) return ev::null();
        std::string sel = ev::toUtf8(selV);
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::null();
        dom::Element* el = doc->querySelector(sel);
        return wrapElement(el);
    });
    b.def("querySelectorAll", 1, [fixed](Value, std::span<const Value> a) {
        auto empty = []() {
            return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        };
        Value selV = argAt(a, 0);
        if (ev::isObject(selV) || ev::isUndefined(selV)) return empty();
        dom::Document* doc = documentFor(fixed);
        if (!doc) return empty();
        std::vector<dom::Element*> list = doc->querySelectorAll(ev::toUtf8(selV));
        return hostArrayOf(list.size(),
                           [&list](size_t i) { return hostElementValue(list[i]); });
    });

    // body / documentElement are ACCESSORS, not values captured at install
    // time: the `document` global is registered before the engine has parsed a
    // document, so there is nothing to capture yet, and a reparse would replace
    // whatever had been. (A parsed document could capture safely — its tree is
    // final the moment parseFromString returns — but one builder serves both
    // and the accessor is correct for each.) Each answers the real element
    // through the registry, which is what makes `document.body.appendChild
    // (panel)` an ordinary tree insert rather than the canvas-only special case
    // it used to be.
    auto defDocElement = [&b, fixed](const char* name,
                                     dom::Element* (dom::Document::*get)() const) {
        b.accessor(name,
                   [get, fixed](Value, std::span<const Value>) {
                       dom::Document* doc = documentFor(fixed);
                       if (!doc) return ev::null();
                       return hostElementValue((doc->*get)());
                   },
                   nullptr);
    };
    defDocElement("body", &dom::Document::body);
    defDocElement("documentElement", &dom::Document::documentElement);
    b.accessor("activeElement",
               [fixed](Value, std::span<const Value>) {
                   dom::Document* doc = documentFor(fixed);
                   if (!doc) return ev::null();
                   return hostElementValue(doc->activeElement());
               },
               nullptr);
    b.accessor("pointerLockElement",
               [fixed](Value, std::span<const Value>) {
                   if (fixed) return ev::null();
                   auto* e = hostEngine();
                   if (!e) return ev::null();
                   return hostElementValue(e->pointerLockElement());
               },
               nullptr);
    b.def("exitPointerLock", 0, [fixed](Value, std::span<const Value>) {
        if (!fixed) {
            if (auto* e = hostEngine()) {
                e->exitPointerLock();
            }
        }
        return ev::undefined();
    });
    b.accessor("fullscreenElement",
               [fixed](Value, std::span<const Value>) {
                   if (fixed) return ev::null();
                   return hostElementValue(hostFullscreenElement());
               },
               nullptr);
    b.accessor("fullscreenEnabled",
               [](Value, std::span<const Value>) {
                   return ev::fromBool(true);
               },
               nullptr);
    b.def("exitFullscreen", 0, [fixed](Value, std::span<const Value>) {
        if (!fixed) {
            setHostFullscreenElement(nullptr);
            if (auto* e = hostEngine()) {
                e->setFullscreenState(false);
                if (auto* win = e->window()) {
                    win->setFullscreen(false);
                }
            }
        }
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    // Document listeners are documentElement's listeners — the exact
    // delegation js_document_addEventListener performs for the interpreted
    // side (src/js/document_bindings.cpp), and for its reason: a document is
    // not an Element, the event path is built from Elements, so an event aimed
    // at the document has to be registered and dispatched where it will
    // actually be walked. The visible consequence is the same one the
    // interpreted side already lives with — `currentTarget` inside such a
    // handler is <html>, not the document.
    //
    // Resolved per call rather than captured: the globals are registered
    // before anyone has asked the engine for its document element.
    installElementEventTarget(b, [fixed]() -> dom::Element* {
        dom::Document* doc = documentFor(fixed);
        return doc ? doc->documentElement() : nullptr;
    }, "document");
    return b.get();
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------

// One function value, installed BOTH on window and as a bare global. Both
// spellings are how it is reached on the web — `getComputedStyle(el)` and
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

Value makeWindowValue() {
    ObjectBuilder b;
    engine::Engine* engine = g_host->engine;

    b.accessor("devicePixelRatio",
               [engine](Value, std::span<const Value>) {
                   return ev::fromDouble(engine->displayScale());
               },
               nullptr);
    // innerWidth/innerHeight report the app content area, which is what the
    // engine reports to its own JS realms as window.innerWidth.
    b.accessor("innerWidth",
               [engine](Value, std::span<const Value>) {
                   return ev::fromDouble(engine->contentWidth());
               },
               nullptr);
    b.accessor("innerHeight",
               [engine](Value, std::span<const Value>) {
                   return ev::fromDouble(engine->contentHeight());
               },
               nullptr);

    // Real listeners, through the same dispatch the engine's JS window
    // listeners ride (Engine::addWindowEventListener) — a compiled app's
    // resize handler fires when a JS app's would, in shared registration
    // order. The event object is the same plain-data copy an element listener
    // gets (host_dom_events.cpp), which is what carries a CustomEvent's string
    // detail across from an interpreted `window.dispatchEvent`. A window event
    // has no target and no dimensions anywhere in this DOM, so a resize
    // handler still reads the new size from window.innerWidth — exactly what
    // the C++ listener docs tell native listeners to do.
    b.def("addEventListener", 3, [engine](Value thisValue,
                                          std::span<const Value> a) {
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
        ev::Persistent fnP(fn);
        std::string origin = "window " + type + " listener";
        dom::ListenerHandle handle = engine->addWindowEventListener(
            type, [fnP, self, origin](dom::Event& evt) {
                callBronzeListener(fnP, self, evt, origin.c_str());
            });
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
    b.def("removeEventListener", 2, [engine](Value, std::span<const Value> a) {
        Value typeV = argAt(a, 0);
        Value fn = argAt(a, 1);
        if (ev::isObject(typeV)) return ev::undefined();
        std::string type = ev::toUtf8(typeV);
        auto& list = g_host->windowListeners;
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (it->type == type && ev::toBits(it->fn.get()) == ev::toBits(fn)) {
                engine->removeWindowEventListener(it->handle);
                list.erase(it);
                break;
            }
        }
        return ev::undefined();
    });

    b.set("getComputedStyle", makeGetComputedStyle());
    b.set("localStorage", makeLocalStorageValue());
    b.def("matchMedia", 1, [](Value, std::span<const Value> a) {
        std::string query = a.empty() || ev::isUndefined(a[0]) ? "" : ev::toUtf8(a[0]);
        bool matches = (query.find("dark") != std::string::npos);
        ObjectBuilder m;
        m.set("matches", ev::fromBool(matches));
        m.set("media", ev::fromUtf8(query));
        auto noop = [](Value, std::span<const Value>) { return ev::undefined(); };
        m.def("addEventListener", 2, noop);
        m.def("removeEventListener", 2, noop);
        m.def("addListener", 1, noop);
        m.def("removeListener", 1, noop);
        return m.get();
    });

    {
        ObjectBuilder loc;
        loc.set("hash", ev::fromUtf8(""));
        loc.set("href", ev::fromUtf8("app://localhost/"));
        loc.set("origin", ev::fromUtf8("app://localhost"));
        loc.set("protocol", ev::fromUtf8("app:"));
        loc.set("host", ev::fromUtf8("localhost"));
        loc.set("hostname", ev::fromUtf8("localhost"));
        loc.set("port", ev::fromUtf8(""));
        loc.set("pathname", ev::fromUtf8("/"));
        loc.set("search", ev::fromUtf8(""));
        loc.def("reload", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
        loc.def("replace", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
        loc.def("assign", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
        b.set("location", loc.get());
    }

    // Read from the global registry rather than captured: the window is built
    // before installAudioGlobals runs, and AudioContext is a real class now
    // whose constructor does not exist yet at this point. An accessor is also
    // the truthful shape — `window.AudioContext` and the bare `AudioContext`
    // are the same object on the web, not two.
    for (const char* name : {
             "AudioContext", "webkitAudioContext", "signals", "CodeMirror",
             "acorn", "tern", "esprima", "jsonlint", "draco_encoder",
             "setTimeout", "clearTimeout", "setInterval", "clearInterval",
             "requestAnimationFrame", "cancelAnimationFrame", "performance",
         }) {
        std::string n(name);
        b.accessor(name,
                   [n](Value, std::span<const Value>) {
                       ev::GlobalValue g = ev::globalValue(n.c_str());
                       return g.found ? g.value : ev::undefined();
                   },
                   nullptr);
    }

    return b.get();
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
            g_host->rafPending.push_back({id, ev::Persistent(fn)});
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

// The same surface the `document` global has — createElement, the queries, the
// element accessors, the event-target delegation — bound to `doc` instead of to
// whatever the engine is showing. host_parser.cpp hands DOMParser results
// through here, which is why it exists at all: the builder itself is private to
// this file, and duplicating it would produce a second document surface that
// drifted from this one the first time either gained a method.
//
// Null `doc` is not defended against here; the only caller has just parsed one.
Value hostDocumentValue(dom::Document* doc) {
    return makeDocumentValue(doc);
}

Value makeBrandConstructor(const char* name) {
    std::string msg = std::string("bronze host ") + name +
                      ": an instanceof brand only, not constructible";
    return ev::makeFunction(
        [msg](Value, std::span<const Value>) { return ev::throwTypeError(msg); }, 0);
}

// Where an exception out of compiled code ends up. bro's JS funnel
// (js::Runtime::callJs) is QuickJS-shaped end to end, so a bronze throw cannot
// ride it; this is the same report-and-continue behaviour aimed at the same log
// stream. The Error's own fields are read through embed property reads — the
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

// The canvas wrapper, reached from host_element.cpp's factory: an element that
// also owns a drawing buffer and a GL context. It lives here because the GL
// context does.
Value makeCanvasElementValue(dom::Element* el) { return makeCanvasValue(el); }

// Identity — the value the program already holds for `el`, so
// `event.target === canvas` is true inside a compiled listener. It asks the
// element registry (host_element.cpp), which is every element this layer ever
// wrapped, not just the canvases: a UI tests target identity on every element
// it built, not only on the one it draws into.
Value hostValueForElement(dom::Element* el) {
    if (!g_host || !el) return ev::undefined();
    return hostElementValue(el);
}

Value makeBroValue() {
    ObjectBuilder b;
    {
        ObjectBuilder menu;
        menu.def("set", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
        menu.def("on", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
        b.set("menu", menu.get());
    }
    {
        ObjectBuilder time;
        time.def("now", 0, [](Value, std::span<const Value>) { return ev::fromDouble(hostClockMs()); });
        b.set("time", time.get());
    }
    b.set("net", makeBroNetValue());
    b.set("mesh", makeBroMeshValue());
    b.set("image", makeBroImageValue());
    // bro.ai.game (host_ai_game.cpp): after installAIGlobals, which installs
    // the classes its factories birth instances on.
    b.set("ai", makeBroAiValue());
    return b.get();
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------
void installWebHostGlobals(engine::Engine& engine) {
    if (g_host) {
        LOG_WARN("bronze_host: installWebHostGlobals called twice; ignoring");
        return;
    }
    // Registration order is the manifest's order (web_host.globals):
    // document, window, self, addEventListener, removeEventListener,
    // dispatchEvent, requestAnimationFrame, cancelAnimationFrame,
    // performance, WebGL2RenderingContext, setTimeout, clearTimeout,
    // setInterval, clearInterval, Image, XMLHttpRequest, fetch, Request,
    // Headers, Response, navigator, HTMLCanvasElement, HTMLImageElement,
    // WebGLRenderingContext, Intl, localStorage, AudioContext, CustomEvent,
    // bro. registerGlobal roots each value for the life of the process, and a
    // manifest name with no registerGlobal behind it is a fatal() at startup,
    // not a catchable miss — so the two lists move together.
    //
    // Never freed — see the lifetime note at the top of this file.
    g_host = new HostState();
    g_host->engine = &engine;

    // The interpreter bridge, before anything the compiled program can run:
    // `new Function` is a BUILTIN read, not a host global, so it is not on the
    // manifest and nothing below registers it — the hook is installed into
    // bronze itself (host_interp.h). Early, because a module's top level may
    // build a function from a string on its first line.
    installInterpBridge(engine);

    // The frame hook, registered exactly once (Engine::onFrame callbacks are
    // never unregistered). It fires at the point the engine's own rAF fires,
    // with rAF's pause semantics, in every display mode. hostFrame above owns
    // the ordering, drain included, and says why each step sits where it does.
    engine.onFrame([](double dtMs) { hostFrame(dtMs); });

    {
        // Null: the global follows the engine's current document rather than
        // naming one. documentFor() above has the reason.
        Value doc = makeDocumentValue(nullptr);
        ev::registerGlobal("document", doc);
    }
    {
        // On the web the window IS the global object: `window.THREE = THREE`
        // and a later bare `THREE` are one binding (the editor does exactly
        // this to hand THREE to its scene scripts). Here the window cannot BE
        // globalThis — its fixed surface is accessors and host functions —
        // so it is a proxy in front of that surface whose expando reads,
        // writes, membership and enumeration forward to globalThis. The
        // read side resolves through ev::globalValue, the same ladder a
        // compiled bare read walks, so the two spellings cannot drift.
        ev::Persistent inner(makeWindowValue());

        HostProxyTraps traps;
        traps.methods = inner.get();
        traps.get = [](const std::string& key, Value& out) {
            ev::GlobalValue g = ev::globalValue(key);
            if (!g.found) return false;
            out = g.value;
            return true;
        };
        traps.set = [](const std::string& key, Value v) {
            // v arrives current, but globalValue may allocate (the builtin
            // ladder builds lazily) — root it first or store stale bits.
            ev::Persistent vP(v);
            ev::GlobalValue gt = ev::globalValue("globalThis");
            if (gt.found) ev::setProperty(gt.value, key.c_str(), vP.get());
        };
        traps.has = [](const std::string& key) { return ev::globalValue(key).found; };
        traps.ownKeys = []() {
            // Reflect.ownKeys(globalThis), string keys only — the same answer
            // the interp bridge's wrapper enumeration gives (host_interp.cpp).
            std::vector<std::string> keys;
            ev::GlobalValue reflect = ev::globalValue("Reflect");
            if (!reflect.found) return keys;
            // getProperty/getElement may allocate: every value read more than
            // once rides in a Persistent (the embed GC contract).
            ev::Persistent reflectP(reflect.value);
            ev::GlobalValue gt = ev::globalValue("globalThis");
            if (!gt.found) return keys;
            ev::Persistent gtP(gt.value);
            Value fn = ev::getProperty(reflectP.get(), "ownKeys");
            if (!ev::isFunction(fn)) return keys;
            Value self = gtP.get();
            ev::CallResult r =
                ev::call(fn, reflectP.get(), std::span<const Value>(&self, 1));
            if (r.thrown || !ev::isObject(r.value)) return keys;
            ev::Persistent arr(r.value);
            const auto n =
                static_cast<uint32_t>(ev::toDouble(ev::getProperty(arr.get(), "length")));
            for (uint32_t i = 0; i < n; ++i) {
                Value k = ev::getElement(arr.get(), i);
                if (!ev::isSymbol(k)) keys.push_back(ev::toUtf8(k));
            }
            return keys;
        };

        ev::Persistent win(makeHostProxy(std::move(traps)));
        // window.self === window and window.window === window, the two
        // self-references three.js and its loaders occasionally take. On the
        // fixed surface, so the reads beat the globalThis fallback.
        inner.set(ev::setProperty(inner.get(), "self", win.get()));
        inner.set(ev::setProperty(inner.get(), "window", win.get()));
        ev::registerGlobal("window", win.get());
        ev::registerGlobal("self", win.get());
        // On the web the global object IS the window, so its listener
        // functions are also global bindings — `globalThis.addEventListener`
        // is how pixi's EventSystem registers pointerup/mouseup. The same
        // three values window carries, registered under their own names so
        // identity holds across both spellings.
        ev::registerGlobal("addEventListener", ev::getProperty(win.get(), "addEventListener"));
        ev::registerGlobal("removeEventListener",
                           ev::getProperty(win.get(), "removeEventListener"));
        ev::registerGlobal("dispatchEvent", ev::getProperty(win.get(), "dispatchEvent"));
        ev::registerGlobal("getComputedStyle",
                           ev::getProperty(win.get(), "getComputedStyle"));
    }
    {
        Value raf = makeRequestAnimationFrame();
        ev::registerGlobal("requestAnimationFrame", raf);
    }
    {
        Value caf = makeCancelAnimationFrame();
        ev::registerGlobal("cancelAnimationFrame", caf);
    }
    {
        Value perf = makePerformanceValue();
        ev::registerGlobal("performance", perf);
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
    installXhrGlobal();
    installFetchGlobal();
    installPlatformGlobals();
    // AFTER installPlatformGlobals, which is where installElementGlobals runs:
    // `Image` is an element class and chains its prototype onto Element's, so
    // Element's has to exist first (host_element_image.cpp).
    installImageGlobal();
    installFileGlobals();
    installAbortGlobals();
    installObserverGlobals();
    installParserGlobal();
    installVideoGlobals();
    installPhysicsGlobals();
    installAIGlobals();

    {
        Value nav = makeNavigatorValue();
        ev::registerGlobal("navigator", nav);
    }
    ev::registerGlobal("HTMLCanvasElement", makeBrandConstructor("HTMLCanvasElement"));
    // HTMLImageElement is NOT here: installImageGlobal above registers it as
    // the same object as `Image`, the way the web does, and that one is a real
    // class whose instances answer `instanceof`. These two remain brands.
    ev::registerGlobal("WebGLRenderingContext", makeBrandConstructor("WebGLRenderingContext"));
    {
        Value pluralRules = ev::makeFunction(
            [](Value, std::span<const Value> /*args*/) -> Value {
                ObjectBuilder b;
                b.def("select", 1, [](Value, std::span<const Value> a) -> Value {
                    double n = a.empty() ? 0.0 : ev::toDouble(a[0]);
                    if (n == 1.0) {
                        return ev::fromUtf8("one");
                    }
                    return ev::fromUtf8("other");
                });
                b.def("resolvedOptions", 0, [](Value, std::span<const Value>) -> Value {
                    ObjectBuilder opts;
                    opts.set("locale", ev::fromUtf8("en-US"));
                    opts.set("type", ev::fromUtf8("cardinal"));
                    return opts.get();
                });
                return b.get();
            },
            0);

        Value numberFormat = ev::makeFunction(
            [](Value, std::span<const Value> /*args*/) -> Value {
                ObjectBuilder b;
                b.def("format", 1, [](Value, std::span<const Value> a) -> Value {
                    if (a.empty()) return ev::fromUtf8("NaN");
                    double n = ev::toDouble(a[0]);
                    char buf[64];
                    if (std::floor(n) == n) {
                        snprintf(buf, sizeof(buf), "%.0f", n);
                    } else {
                        snprintf(buf, sizeof(buf), "%g", n);
                    }
                    return ev::fromUtf8(buf);
                });
                b.def("resolvedOptions", 0, [](Value, std::span<const Value>) -> Value {
                    ObjectBuilder opts;
                    opts.set("locale", ev::fromUtf8("en-US"));
                    return opts.get();
                });
                return b.get();
            },
            0);

        Value dateTimeFormat = ev::makeFunction(
            [](Value, std::span<const Value> /*args*/) -> Value {
                ObjectBuilder b;
                b.def("format", 1, [](Value, std::span<const Value>) -> Value {
                    return ev::fromUtf8("");
                });
                b.def("resolvedOptions", 0, [](Value, std::span<const Value>) -> Value {
                    ObjectBuilder opts;
                    opts.set("locale", ev::fromUtf8("en-US"));
                    return opts.get();
                });
                return b.get();
            },
            0);

        Value collator = ev::makeFunction(
            [](Value, std::span<const Value> /*args*/) -> Value {
                ObjectBuilder b;
                b.def("compare", 2, [](Value, std::span<const Value> a) -> Value {
                    std::string s1 = a.size() > 0 ? ev::toUtf8(a[0]) : "";
                    std::string s2 = a.size() > 1 ? ev::toUtf8(a[1]) : "";
                    if (s1 < s2) return ev::fromDouble(-1);
                    if (s1 > s2) return ev::fromDouble(1);
                    return ev::fromDouble(0);
                });
                return b.get();
            },
            0);

        Value displayNames = ev::makeFunction(
            [](Value, std::span<const Value> /*args*/) -> Value {
                ObjectBuilder b;
                b.def("of", 1, [](Value, std::span<const Value> a) -> Value {
                    if (a.empty()) return ev::fromUtf8("");
                    std::string code = ev::toUtf8(a[0]);
                    if (code == "en") return ev::fromUtf8("English");
                    if (code == "fr") return ev::fromUtf8("Français");
                    if (code == "zh") return ev::fromUtf8("中文");
                    if (code == "ja") return ev::fromUtf8("日本語");
                    if (code == "ko") return ev::fromUtf8("한국어");
                    if (code == "fa") return ev::fromUtf8("فارسی");
                    return a[0];
                });
                return b.get();
            },
            2);

        ObjectBuilder intl;
        intl.set("PluralRules", pluralRules);
        intl.set("NumberFormat", numberFormat);
        intl.set("DateTimeFormat", dateTimeFormat);
        intl.set("Collator", collator);
        intl.set("DisplayNames", displayNames);
        ev::registerGlobal("Intl", intl.get());
    }
    {
        Value ls = makeLocalStorageValue();
        ev::registerGlobal("localStorage", ls);
    }
    installAudioGlobals();
    {
        Value customEvent = makeEventConstructor("CustomEvent");
        ev::registerGlobal("CustomEvent", customEvent);
    }
    {
        Value broVal = makeBroValue();
        ev::registerGlobal("bro", broVal);
    }
    installNetGlobals();
    installVendorGlobals();
}

}  // namespace bro::bronze_host
