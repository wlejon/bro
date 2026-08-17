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
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
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
void hostFrame(double dtMs) {
    if (ev::microtasksPending()) ev::drainMicrotasks();  // 1
    g_host->clockMs += dtMs;                             // 2
    drainHostTasks();                                    // 3
    fireHostTimers(g_host->clockMs);                     // 4
    fireAnimationFrames();                               // 5
    deliverHostObservers();                              // 5b
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
// same one for all of them. `img` is the one exception — it is a host object
// with a decoder behind `.src`, not a laid-out element (host_image.cpp).
Value createElementImpl(dom::Document* fixed, std::span<const Value> a,
                        size_t tagIndex) {
    Value tagV = argAt(a, tagIndex);
    if (ev::isObject(tagV)) return ev::throwTypeError("createElement: tag must be a string");
    std::string tag = ev::toUtf8(tagV);
    for (char& ch : tag) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    // three.js's ImageLoader builds its element as createElementNS(xhtml,
    // 'img'), so the factory has to answer for it — and what it answers is the
    // same object `new Image()` gives, because there is only one image kind
    // here. It is NOT a dom::Element: nothing lays an image out in this layer,
    // and the only thing three.js does with it is read its size and hand it to
    // texImage2D.
    if (tag == "img") return makeImageValue();

    dom::Document* doc = documentFor(fixed);
    if (!doc) return ev::throwError("bronze host: engine has no document");
    dom::Element* el = doc->createElement(tag);
    if (!el) return ev::throwError("bronze host: createElement failed");
    // Every other tag — div, span, input, select, textarea, button — is a real
    // dom::Element with the full element surface on it. The `div` special case
    // that used to live here answered with an overlay stub that refused
    // appendChild, which was the right shape only while nothing built a tree.
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
// localStorage & AudioContext
// ---------------------------------------------------------------------------

struct StorageState {
    std::map<std::string, std::string> items;
    std::string path;
    bool loaded = false;
};
static StorageState g_storage;

static void loadStorageFile() {
    if (g_storage.loaded) return;
    g_storage.loaded = true;
    g_storage.path = ".storage.json";
    std::ifstream file(g_storage.path);
    if (!file.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    while (pos < content.size()) {
        size_t kstart = content.find('"', pos);
        if (kstart == std::string::npos) break;
        size_t kend = content.find('"', kstart + 1);
        if (kend == std::string::npos) break;
        std::string key = content.substr(kstart + 1, kend - kstart - 1);
        size_t colon = content.find(':', kend);
        if (colon == std::string::npos) break;
        size_t vstart = content.find('"', colon);
        if (vstart == std::string::npos) break;
        size_t vend = content.find('"', vstart + 1);
        if (vend == std::string::npos) break;
        std::string val = content.substr(vstart + 1, vend - vstart - 1);
        g_storage.items[key] = val;
        pos = vend + 1;
    }
}

static void saveStorageFile() {
    std::ofstream file(g_storage.path);
    if (!file.is_open()) return;
    file << "{\n";
    size_t idx = 0;
    for (const auto& [k, v] : g_storage.items) {
        if (idx > 0) file << ",\n";
        file << "  \"" << k << "\": \"" << v << "\"";
        idx++;
    }
    file << "\n}\n";
}

Value makeLocalStorageValue() {
    loadStorageFile();
    ObjectBuilder b;
    b.def("getItem", 1, [](Value, std::span<const Value> a) {
        Value keyV = argAt(a, 0);
        if (ev::isObject(keyV) || ev::isUndefined(keyV)) return ev::null();
        std::string key = ev::toUtf8(keyV);
        auto it = g_storage.items.find(key);
        if (it == g_storage.items.end()) return ev::null();
        return ev::fromUtf8(it->second);
    });
    b.def("setItem", 2, [](Value, std::span<const Value> a) {
        Value keyV = argAt(a, 0);
        Value valV = argAt(a, 1);
        if (!ev::isObject(keyV) && !ev::isUndefined(keyV)) {
            std::string key = ev::toUtf8(keyV);
            std::string val = (!ev::isObject(valV) && !ev::isUndefined(valV)) ? ev::toUtf8(valV) : "";
            g_storage.items[key] = val;
            saveStorageFile();
        }
        return ev::undefined();
    });
    b.def("removeItem", 1, [](Value, std::span<const Value> a) {
        Value keyV = argAt(a, 0);
        if (!ev::isObject(keyV) && !ev::isUndefined(keyV)) {
            g_storage.items.erase(ev::toUtf8(keyV));
            saveStorageFile();
        }
        return ev::undefined();
    });
    b.def("clear", 0, [](Value, std::span<const Value>) {
        g_storage.items.clear();
        saveStorageFile();
        return ev::undefined();
    });
    b.def("key", 1, [](Value, std::span<const Value> a) {
        int idx = i32At(a, 0);
        if (idx < 0 || static_cast<size_t>(idx) >= g_storage.items.size()) return ev::null();
        auto it = g_storage.items.begin();
        std::advance(it, idx);
        return ev::fromUtf8(it->first);
    });
    b.accessor("length", [](Value, std::span<const Value>) {
        return ev::fromDouble(static_cast<double>(g_storage.items.size()));
    }, nullptr);
    return b.get();
}

Value makeAudioContextConstructor() {
    return ev::makeFunction([](Value, std::span<const Value>) {
        ObjectBuilder ctx;
        ctx.set("state", ev::fromUtf8("running"));
        ctx.set("currentTime", ev::fromDouble(0.0));
        ctx.set("sampleRate", ev::fromDouble(44100.0));
        {
            ObjectBuilder dest;
            ctx.set("destination", dest.get());
        }
        ctx.def("resume", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
        ctx.def("suspend", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
        ctx.def("close", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
        ctx.def("createGain", 0, [](Value, std::span<const Value>) {
            ObjectBuilder g;
            ObjectBuilder param;
            param.set("value", ev::fromDouble(1.0));
            param.def("setValueAtTime", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
            param.def("linearRampToValueAtTime", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
            param.def("exponentialRampToValueAtTime", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
            g.set("gain", param.get());
            g.def("connect", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
            g.def("disconnect", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
            return g.get();
        });
        ctx.def("createOscillator", 0, [](Value, std::span<const Value>) {
            ObjectBuilder osc;
            osc.set("type", ev::fromUtf8("sine"));
            ObjectBuilder freq;
            freq.set("value", ev::fromDouble(440.0));
            freq.def("setValueAtTime", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
            freq.def("exponentialRampToValueAtTime", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
            osc.set("frequency", freq.get());
            osc.def("connect", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
            osc.def("disconnect", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
            osc.def("start", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
            osc.def("stop", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
            return osc.get();
        });
        ctx.def("createBufferSource", 0, [](Value, std::span<const Value>) {
            ObjectBuilder src;
            src.def("connect", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
            src.def("start", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
            src.def("stop", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
            return src.get();
        });
        return ctx.get();
    }, 0);
}

// The compiled app's navigator answers with the identity the interpreted side
// already declares (src/js/window_bindings.cpp): one engine, one name — an app
// must not learn a different browser depending on which compiler ran it, and
// "Bro/1.0" routes every UA sniff (pixi's isSafari, isMobile) to the desktop
// path this layer actually implements. maxTouchPoints=0 is the other half of
// that routing: it keeps pixi's EventSystem on the mouse events
// host_dom_events.cpp wires.
Value makeNavigatorValue() {
    ObjectBuilder b;
    b.set("userAgent", ev::fromUtf8("Bro/1.0"));
    b.set("platform", ev::fromUtf8("Win32"));
    b.set("language", ev::fromUtf8("en-US"));
    b.set("maxTouchPoints", ev::fromDouble(0));
    return b.get();
}

// A DOM constructor this host never constructs, shaped as a callable because
// `instanceof` demands one of its right operand: a bare object there is a
// TypeError, a host function answers false (the documented limit that makes
// `img instanceof Image` false — README.md). pixi only ever brand-tests
// against these (`gl instanceof WebGLRenderingContext` is how it reads the
// context's GL version: false is the truthful answer here, this host's
// context is WebGL2). Calling one is the named error, not a broken instance.
Value makeBrandConstructor(const char* name) {
    std::string msg = std::string("bronze host ") + name +
                      ": an instanceof brand only, not constructible";
    return ev::makeFunction(
        [msg](Value, std::span<const Value>) { return ev::throwTypeError(msg); }, 0);
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
    b.set("AudioContext", makeAudioContextConstructor());
    b.set("webkitAudioContext", makeAudioContextConstructor());

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
// A document wrapper for a document that is not the engine's
// ---------------------------------------------------------------------------

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
    // Never freed — see the lifetime note at the top of this file.
    g_host = new HostState();
    g_host->engine = &engine;

    // The frame hook, registered exactly once (Engine::onFrame callbacks are
    // never unregistered). It fires at the point the engine's own rAF fires,
    // with rAF's pause semantics, in every display mode. hostFrame above owns
    // the ordering, drain included, and says why each step sits where it does.
    engine.onFrame([](double dtMs) { hostFrame(dtMs); });

    // Registration order is the manifest's order (web_host.globals):
    // document, window, self, addEventListener, removeEventListener,
    // dispatchEvent, requestAnimationFrame, cancelAnimationFrame,
    // performance, WebGL2RenderingContext, setTimeout, clearTimeout,
    // setInterval, clearInterval, Image, XMLHttpRequest, fetch, Request,
    // Headers, Response, navigator, HTMLCanvasElement, HTMLImageElement,
    // WebGLRenderingContext, Intl, localStorage, AudioContext, CustomEvent,
    // bro. registerGlobal roots each value for the life of the process.
    {
        // Null: the global follows the engine's current document rather than
        // naming one. documentFor() above has the reason.
        Value doc = makeDocumentValue(nullptr);
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
    installImageGlobal();
    installXhrGlobal();
    installFetchGlobal();
    installPlatformGlobals();
    installFileGlobals();
    installAbortGlobals();
    installObserverGlobals();
    installParserGlobal();

    {
        Value nav = makeNavigatorValue();
        ev::registerGlobal("navigator", nav);
    }
    ev::registerGlobal("HTMLCanvasElement", makeBrandConstructor("HTMLCanvasElement"));
    ev::registerGlobal("HTMLImageElement", makeBrandConstructor("HTMLImageElement"));
    ev::registerGlobal("WebGLRenderingContext", makeBrandConstructor("WebGLRenderingContext"));
    {
        // Intl with no members is a real environment shape — pixi's own
        // comment names Firefox for the missing Segmenter, and its boot
        // EVALUATES the binding (`Intl == null ? ...`), so the name must
        // resolve; the fallback it then takes is the one it documents.
        // The day this host grows a real Intl member, it goes here.
        ObjectBuilder intl;
        ev::registerGlobal("Intl", intl.get());
    }
    {
        Value ls = makeLocalStorageValue();
        ev::registerGlobal("localStorage", ls);
    }
    {
        Value audioCtx = makeAudioContextConstructor();
        ev::registerGlobal("AudioContext", audioCtx);
    }
    {
        Value customEvent = makeBrandConstructor("CustomEvent");
        ev::registerGlobal("CustomEvent", customEvent);
    }
    {
        Value broVal = makeBroValue();
        ev::registerGlobal("bro", broVal);
    }
}

}  // namespace bro::bronze_host
