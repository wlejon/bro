// The minimal DOM a bronze-compiled three.js app touches: document (create a
// canvas, append it), the canvas element itself (size, style, getContext),
// window (sizes, DPR, resize listeners), and requestAnimationFrame driven by
// Engine::onFrame. Everything else the web platform offers is deliberately
// absent — an unknown createElement tag is a named refusal, not a stub that
// fails later.
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
#include "bronze_host/host_internal.h"

#include "engine/engine.h"
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
//  6. The microtask checkpoint. AFTER rAF, not before: an rAF callback is the
//     main producer of promise jobs in a render loop — three.js's own
//     `renderer.setAnimationLoop` body, every `await` an app puts in its frame
//     function — and draining before it would leave every one of those jobs
//     queued until the next frame. That is not merely late: each frame would
//     run the PREVIOUS frame's continuations against this frame's state, and a
//     rejection thrown in the last rAF before shutdown would never be reported
//     at all, because quiescence would never be reached again.
void hostFrame(double dtMs) {
    if (ev::microtasksPending()) ev::drainMicrotasks();  // 1
    g_host->clockMs += dtMs;                             // 2
    drainHostTasks();                                    // 3
    fireHostTimers(g_host->clockMs);                     // 4
    fireAnimationFrames();                               // 5
    ev::drainMicrotasks();                               // 6
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

// The style object three.js's setSize writes through. Only the properties the
// renderer actually assigns are wired to real CSS; a write to anything else
// lands as a plain property on this object — visible to a debugger, inert to
// layout.
Value makeStyleValue(CanvasState* cs) {
    ObjectBuilder b;
    auto defStyleProp = [&](const char* cssName) {
        dom::Element* el = cs->el;
        std::string prop = cssName;
        b.accessor(cssName,
                   [](Value, std::span<const Value>) { return ev::fromUtf8(""); },
                   [el, prop](Value, std::span<const Value> a) {
                       Value v = argAt(a, 0);
                       if (!ev::isObject(v)) el->style().setProperty(prop, ev::toUtf8(v));
                       return ev::undefined();
                   });
    };
    defStyleProp("width");
    defStyleProp("height");
    defStyleProp("display");
    defStyleProp("touchAction");
    return b.get();
}

Value makeCanvasValue(dom::Element* el) {
    auto owned = std::make_unique<CanvasState>();
    CanvasState* cs = owned.get();
    cs->el = el;
    g_host->canvases.push_back(std::move(owned));

    ObjectBuilder b;

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

    {
        Value style = makeStyleValue(cs);
        b.set("style", style);
    }

    // getContext('webgl2'|'webgl') → the B2 context object, built over the
    // SAME Engine path the QuickJS factory takes (Engine::createWebGL2Context
    // — one construction path, no drift), and cached so a second call answers
    // the same object, per spec. '2d' and 'scene' are not bound in this layer
    // and answer null, the factory's own answer for an unknown type.
    b.def("getContext", 1, [cs](Value, std::span<const Value> a) {
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV)) return ev::null();
        std::string type = ev::toUtf8(typeV);
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

    // Real listeners on the engine's own element: a click that hit-tests to
    // this canvas fires them, in registration order beside any the page's
    // interpreted script put on the same element (host_dom_events.cpp).
    // `cs` outlives the program — HostState is never freed — so the element
    // source can read through it safely for the life of the process.
    installElementEventTarget(b, [cs]() { return cs->el; }, "canvas");

    Value built = b.get();
    cs->jsObj.set(built);
    return built;
}

// The canvas registry answers "which host canvas is this Value?" for
// document.body.appendChild — identity by current-address compare against
// each Persistent, valid because nothing allocates during the scan.
CanvasState* canvasFor(Value v) {
    for (auto& cs : g_host->canvases) {
        if (ev::toBits(cs->jsObj.get()) == ev::toBits(v)) return cs.get();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// document
// ---------------------------------------------------------------------------

Value createElementImpl(std::span<const Value> a, size_t tagIndex) {
    Value tagV = argAt(a, tagIndex);
    if (ev::isObject(tagV)) return ev::throwTypeError("createElement: tag must be a string");
    std::string tag = ev::toUtf8(tagV);
    for (char& ch : tag) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    // three.js's ImageLoader builds its element as createElementNS(xhtml,
    // 'img'), so the factory has to answer for it — and what it answers is the
    // same object `new Image()` gives, because there is only one image kind
    // here (host_image.cpp). It is NOT a dom::Element: nothing lays an image out
    // in this layer, and the only thing three.js does with it is read its size
    // and hand it to texImage2D.
    if (tag == "img") return makeImageValue();
    if (tag != "canvas") {
        // The named refusal: this layer models exactly what three.js's
        // renderer touches, and a silent non-canvas stub would fail far from
        // here (bro CLAUDE.md: hard errors over silent fallbacks — bronze
        // agrees).
        return ev::throwTypeError("bronze host document.createElement: only <canvas> "
                                  "and <img> are modelled, got <" + tag + ">");
    }
    dom::Document* doc = g_host->engine->document();
    if (!doc) return ev::throwError("bronze host: engine has no document");
    dom::Element* el = doc->createElement("canvas");
    if (!el) return ev::throwError("bronze host: createElement failed");
    return makeCanvasValue(el);
}

Value makeBodyValue() {
    ObjectBuilder b;
    b.def("appendChild", 1, [](Value, std::span<const Value> a) {
        CanvasState* cs = canvasFor(argAt(a, 0));
        if (!cs) {
            return ev::throwTypeError(
                "bronze host body.appendChild: only host-created canvas elements");
        }
        dom::Document* doc = g_host->engine->document();
        dom::Element* parent = doc->body() ? doc->body() : doc->documentElement();
        if (parent && !cs->el->parentNode()) {
            // Bare dom::Node::appendChild — it invalidates layout itself, so
            // the element enters layout with nothing further (docs/embedding.md).
            parent->appendChild(cs->el);
        }
        return argAt(a, 0);
    });
    b.def("removeChild", 1, [](Value, std::span<const Value> a) {
        CanvasState* cs = canvasFor(argAt(a, 0));
        if (cs && cs->el->parentNode()) {
            cs->el->parentNode()->removeChild(cs->el);
        }
        return argAt(a, 0);
    });
    return b.get();
}

Value makeDocumentValue() {
    ObjectBuilder b;
    b.def("createElement", 1, [](Value, std::span<const Value> a) {
        return createElementImpl(a, 0);
    });
    // three.js spells it createElementNS('http://www.w3.org/1999/xhtml',
    // 'canvas'); the namespace is accepted and ignored, as bro's own DOM does
    // for HTML content.
    b.def("createElementNS", 2, [](Value, std::span<const Value> a) {
        return createElementImpl(a, 1);
    });
    {
        Value body = makeBodyValue();
        b.set("body", body);
    }
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
    installElementEventTarget(b, []() -> dom::Element* {
        dom::Document* doc = g_host->engine->document();
        return doc ? doc->documentElement() : nullptr;
    }, "document");
    return b.get();
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// The two things every file in this layer shares (host_internal.h)
// ---------------------------------------------------------------------------

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

// Identity, not a lookup table built for it: the canvas registry is already
// the list of every element this layer wrapped, and it is short (one canvas in
// every path an app takes). Scanning it costs less than the map that would
// have to be kept in step with it.
Value hostValueForElement(dom::Element* el) {
    if (!g_host || !el) return ev::undefined();
    for (auto& cs : g_host->canvases) {
        if (cs->el == el) return cs->jsObj.get();
    }
    return ev::undefined();
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

void installThreejsHostGlobals(engine::Engine& engine) {
    if (g_host) {
        LOG_WARN("bronze_host: installThreejsHostGlobals called twice; ignoring");
        return;
    }
    // Never freed — see the lifetime note at the top of this file.
    g_host = new HostState();
    g_host->engine = &engine;

    // The frame hook, registered exactly once (Engine::onFrame callbacks are
    // never unregistered). It fires at the point the engine's own rAF fires,
    // with rAF's pause semantics, in every display mode. hostFrame above owns
    // the ordering, drain included, and says why each step sits where it does.
    engine.onFrame([](double dtMs) { hostFrame(dtMs); });

    // Registration order is the manifest's order (threejs_host.globals):
    // document, window, self, requestAnimationFrame, cancelAnimationFrame,
    // performance, WebGL2RenderingContext, setTimeout, clearTimeout,
    // setInterval, clearInterval, Image, XMLHttpRequest. registerGlobal roots
    // each value for the life of the process.
    {
        Value doc = makeDocumentValue();
        ev::registerGlobal("document", doc);
    }
    {
        ev::Persistent win(makeWindowValue());
        // window.self === window and window.window === window, the two
        // self-references three.js and its loaders occasionally take.
        win.set(ev::setProperty(win.get(), "self", win.get()));
        win.set(ev::setProperty(win.get(), "window", win.get()));
        ev::registerGlobal("window", win.get());
        ev::registerGlobal("self", win.get());
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
    // The three families that own their own files, each registering the names
    // the manifest lists for it, in the manifest's order.
    installTimerGlobals();
    installImageGlobal();
    installXhrGlobal();
}

}  // namespace bro::bronze_host
