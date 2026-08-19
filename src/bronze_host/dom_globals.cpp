// The minimal DOM a bronze-compiled three.js app touches: document (create a
// canvas, append it), the canvas element itself (size, style, getContext),
// window (sizes, DPR, resize listeners), and requestAnimationFrame driven by
// Engine::onFrame.
//
// Main frame seam (hostFrame), document, window, canvas object, installWebHostGlobals.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

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
    bool hasGl = false;
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
    std::vector<std::unique_ptr<CanvasState>> canvases;
    std::vector<RafEntry> rafPending;
    int32_t nextRafId = 1;
    double clockMs = 0.0;
    std::vector<WindowListener> windowListeners;
};

HostState* g_host = nullptr;

// ---------------------------------------------------------------------------
// requestAnimationFrame
// ---------------------------------------------------------------------------

void fireAnimationFrames() {
    if (g_host->rafPending.empty()) return;

    std::vector<RafEntry> current = std::move(g_host->rafPending);
    g_host->rafPending.clear();

    for (RafEntry& entry : current) {
        Value ts = ev::fromDouble(g_host->clockMs);
        ev::CallResult r = ev::call(entry.fn.get(), ev::undefined(),
                                    std::span<const Value>(&ts, 1));
        if (r.thrown) reportBronzeError("requestAnimationFrame", r.value);
    }
}

// ---------------------------------------------------------------------------
// The frame seam
// ---------------------------------------------------------------------------

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
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

int attributeOr(dom::Element* el, const char* name, int fallback) {
    const std::string& v = el->getAttribute(name);
    return v.empty() ? fallback : std::atoi(v.c_str());
}

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

    ObjectBuilder b(makeElementHandleObject(el));
    installElementCore(b, el);

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

dom::Document* documentFor(dom::Document* fixed) {
    if (fixed) return fixed;
    return (g_host && g_host->engine) ? g_host->engine->document() : nullptr;
}

Value createElementImpl(dom::Document* fixed, std::span<const Value> a,
                        size_t tagIndex) {
    Value tagV = argAt(a, tagIndex);
    if (ev::isObject(tagV)) return ev::throwTypeError("createElement: tag must be a string");
    std::string tag = ev::toUtf8(tagV);
    for (char& ch : tag) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (tag == "img") return makeImageValue();

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
    b.def("createElementNS", 2, [fixed](Value, std::span<const Value> a) {
        return createElementImpl(fixed, a, 1);
    });
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

    installElementEventTarget(b, [fixed]() -> dom::Element* {
        dom::Document* doc = documentFor(fixed);
        return doc ? doc->documentElement() : nullptr;
    }, "document");
    return b.get();
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------

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

    for (const char* name : {"AudioContext", "webkitAudioContext"}) {
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
    b.def("now", 0, [](Value, std::span<const Value>) {
        return ev::fromDouble(g_host->clockMs);
    });
    return b.get();
}

}  // namespace

Value hostDocumentValue(dom::Document* doc) {
    return makeDocumentValue(doc);
}

Value makeBrandConstructor(const char* name) {
    std::string msg = std::string("bronze host ") + name +
                      ": an instanceof brand only, not constructible";
    return ev::makeFunction(
        [msg](Value, std::span<const Value>) { return ev::throwTypeError(msg); }, 0);
}

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

double hostClockMs() { return g_host ? g_host->clockMs : 0.0; }

engine::Engine* hostEngine() { return g_host ? g_host->engine : nullptr; }

Value makeCanvasElementValue(dom::Element* el) { return makeCanvasValue(el); }

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
    return b.get();
}

void installWebHostGlobals(engine::Engine& engine) {
    if (g_host) {
        LOG_WARN("bronze_host: installWebHostGlobals called twice; ignoring");
        return;
    }
    g_host = new HostState();
    g_host->engine = &engine;

    engine.onFrame([](double dtMs) { hostFrame(dtMs); });

    {
        Value doc = makeDocumentValue(nullptr);
        ev::registerGlobal("document", doc);
    }
    {
        ev::Persistent win(makeWindowValue());
        win.set(ev::setProperty(win.get(), "self", win.get()));
        win.set(ev::setProperty(win.get(), "window", win.get()));
        ev::registerGlobal("window", win.get());
        ev::registerGlobal("self", win.get());
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
        ObjectBuilder ctor;
        Value name = ev::fromUtf8("WebGL2RenderingContext");
        ctor.set("name", name);
        ev::registerGlobal("WebGL2RenderingContext", ctor.get());
    }

    installTimerGlobals();
    installImageGlobal();
    installXhrGlobal();
    installFetchGlobal();
    installPlatformGlobals();
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
    ev::registerGlobal("WebGLRenderingContext", makeBrandConstructor("WebGLRenderingContext"));
    {
        ObjectBuilder intl;
        ev::registerGlobal("Intl", intl.get());
    }
    {
        Value ls = makeLocalStorageValue();
        ev::registerGlobal("localStorage", ls);
    }
    installAudioGlobals();
    {
        Value customEvent = makeBrandConstructor("CustomEvent");
        ev::registerGlobal("CustomEvent", customEvent);
    }
    {
        Value broVal = makeBroValue();
        ev::registerGlobal("bro", broVal);
    }
    installNetGlobals();
}

}  // namespace bro::bronze_host
