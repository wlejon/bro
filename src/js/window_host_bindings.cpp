// bro.window.open() — secondary OS windows (multiwindow v1 IN PROGRESS).
//
// This chunk ships the window lifecycle: open → real (blank) OS window,
// handle with geometry/title/focus control, close() / OS close button →
// 'close' event. The src argument is validated and stored by the engine but
// no document is created for it yet — that lands with the next chunk
// (see docs/window-api.js).
//
// Realm policy: only the MAIN app realm may open windows. The binding is
// also installed into child (iframe) realms so a call there throws a clean,
// deliberate error. The per-context handle registry below pins each handle
// object until its 'close' event has fired, so a handle the app dropped
// still receives the event.

#include "js/window_host_bindings.h"

#include "engine/engine.h"
#include "js/runtime.h"
#include "platform/sdl_window.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

#include <unordered_map>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

namespace {

// One engine per process (same assumption as the other engine-bound statics
// in this directory; per-realm scoping is the next chunk's work).
engine::Engine* s_engine = nullptr;

// The JS-side handle for one window host. Owned by its JS wrapper object
// (default qjsbind finalizer path is replaced with one that also frees the
// stored listener refs).
struct WindowHandle {
    engine::Engine* engine = nullptr;
    uint64_t id = 0;
    // 'close' listeners, JS_DupValue'd. Marked in gc_mark, freed in the
    // finalizer / on removeEventListener.
    std::vector<JSValue> closeListeners;
};

// Per-context registry: host id → dup'd handle object. Keeps the handle
// alive until its 'close' delivers; released by windowHostNotifyClosed or
// cleanupWindowHostBindings.
std::unordered_map<JSContext*, std::unordered_map<uint64_t, JSValue>> s_handles;

void handleFinalizer(JSRuntime* rt, JSValue val) {
    auto* h = static_cast<WindowHandle*>(
        JS_GetOpaque(val, qjsbind::class_id<WindowHandle>()));
    if (!h) return;
    for (JSValue& fn : h->closeListeners) JS_FreeValueRT(rt, fn);
    delete h;
}

engine::Engine::WindowHost* hostFor(const WindowHandle* h) {
    if (!h || !h->engine) return nullptr;
    return h->engine->windowHostById(h->id);
}

JSValue sizeToJS(JSContext* ctx, int w, int h) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));
    return obj;
}

JSValue posToJS(JSContext* ctx, int x, int y) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewInt32(ctx, x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewInt32(ctx, y));
    return obj;
}

// addEventListener/removeEventListener — raw methods ('close' only for now;
// unknown types are accepted and ignored, DOM-style).
JSValue js_handle_addEventListener(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* h = qjsbind::unwrap<WindowHandle>(ctx, this_val);
    if (!h || argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    if (type && std::string_view(type) == "close")
        h->closeListeners.push_back(JS_DupValue(ctx, argv[1]));
    if (type) JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

JSValue js_handle_removeEventListener(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* h = qjsbind::unwrap<WindowHandle>(ctx, this_val);
    if (!h || argc < 2) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    if (type && std::string_view(type) == "close") {
        for (auto it = h->closeListeners.begin(); it != h->closeListeners.end(); ++it) {
            if (JS_VALUE_GET_PTR(*it) == JS_VALUE_GET_PTR(argv[1])) {
                JS_FreeValue(ctx, *it);
                h->closeListeners.erase(it);
                break;
            }
        }
    }
    if (type) JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

// Register the handle class on this context. The class id/def are runtime-
// wide; the Class builder re-run on a fresh context (app reload) just
// installs a fresh prototype for it — the same pattern the other qjsbind
// classes use.
void registerHandleClass(JSContext* ctx) {
    using engine::Engine;
    qjsbind::Class<WindowHandle>(ctx, "BroWindowHandle",
                                 qjsbind::NoGlobal, handleFinalizer)
        .gc_mark([](WindowHandle* h, JSRuntime* rt, JS_MarkFunc* mark) {
            for (JSValue& fn : h->closeListeners) JS_MarkValue(rt, fn, mark);
        })
        .get("id", [](WindowHandle* h) { return static_cast<double>(h->id); })
        .get("closed", [](WindowHandle* h) { return hostFor(h) == nullptr; })
        .method("close", [](WindowHandle* h) {
            if (h->engine) h->engine->closeWindowHost(h->id);
        })
        .method("setTitle", [](WindowHandle* h, std::string title) {
            if (auto* host = hostFor(h)) {
                host->opts.title = title;
                if (host->window) host->window->setTitle(title);
            }
        })
        .method("getSize", [](WindowHandle* h, JSContext* c) -> JSValue {
            if (auto* host = hostFor(h)) {
                if (host->window) {
                    int w = 0, ht = 0;
                    host->window->getSize(w, ht);
                    return sizeToJS(c, w, ht);
                }
                return sizeToJS(c, host->opts.width, host->opts.height);
            }
            return sizeToJS(c, 0, 0);
        })
        .method("setSize", [](WindowHandle* h, int w, int ht) {
            auto* host = hostFor(h);
            if (!host || w < 1 || ht < 1) return;
            host->opts.width = w;
            host->opts.height = ht;
            host->width = w;
            host->height = ht;
            // Applies to hidden (incl. headless) windows too — pure window
            // state that round-trips, same policy as bro.window's flag/limit
            // setters.
            if (host->window)
                host->window->setWindowSize(static_cast<uint32_t>(w),
                                            static_cast<uint32_t>(ht));
        })
        .method("getPosition", [](WindowHandle* h, JSContext* c) -> JSValue {
            if (auto* host = hostFor(h)) {
                if (host->window) {
                    int x = 0, y = 0;
                    host->window->getPosition(x, y);
                    return posToJS(c, x, y);
                }
                int x = host->opts.x == engine::kWindowPosUnset ? 0 : host->opts.x;
                int y = host->opts.y == engine::kWindowPosUnset ? 0 : host->opts.y;
                return posToJS(c, x, y);
            }
            return posToJS(c, 0, 0);
        })
        .method("setPosition", [](WindowHandle* h, int x, int y) {
            auto* host = hostFor(h);
            if (!host) return;
            // Hidden windows (all headless hosts) keep the bro.window policy:
            // positioning depends on the desktop the process runs on, so it
            // no-ops rather than making test output desk-dependent.
            if (host->opts.hidden) return;
            host->opts.x = x;
            host->opts.y = y;
            if (host->window) host->window->setPosition(x, y);
        })
        .method("focus", [](WindowHandle* h) {
            if (auto* host = hostFor(h)) {
                if (host->window && !host->opts.hidden) host->window->raise();
            }
        })
        .method_raw("addEventListener", js_handle_addEventListener, 2)
        .method_raw("removeEventListener", js_handle_removeEventListener, 2);
}

// bro.window.open(src, opts) — raw for flexible option parsing.
JSValue js_bro_window_open(JSContext* ctx, JSValueConst /*this_val*/,
                           int argc, JSValueConst* argv) {
    if (!s_engine)
        return JS_ThrowTypeError(ctx, "bro.window.open: engine unavailable");

    // Realm gate: the main app realm only. Child/iframe/panel realms see the
    // same function and land here.
    js::Runtime* rt = s_engine->jsRuntime();
    if (!rt || ctx != rt->getContext())
        return JS_ThrowTypeError(ctx,
            "bro.window.open is only available from the main app realm");

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "bro.window.open(src, opts): src must be a string");
    const char* src = JS_ToCString(ctx, argv[0]);
    if (!src) return JS_EXCEPTION;

    engine::Engine::WindowHostOptions opts;
    opts.src = src;
    JS_FreeCString(ctx, src);
    if (opts.src.empty())
        return JS_ThrowTypeError(ctx, "bro.window.open(src, opts): src must be non-empty");

    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValueConst o = argv[1];
        opts.width  = qjsbind::get_prop_int(ctx, o, "width", opts.width);
        opts.height = qjsbind::get_prop_int(ctx, o, "height", opts.height);
        opts.title  = qjsbind::get_prop_string(ctx, o, "title", opts.title.c_str());
        opts.x = qjsbind::get_prop_int(ctx, o, "x", opts.x);
        opts.y = qjsbind::get_prop_int(ctx, o, "y", opts.y);
        opts.display = qjsbind::get_prop_int(ctx, o, "display", opts.display);
        opts.resizable   = qjsbind::get_prop_bool(ctx, o, "resizable", opts.resizable);
        opts.borderless  = qjsbind::get_prop_bool(ctx, o, "borderless", opts.borderless);
        opts.alwaysOnTop = qjsbind::get_prop_bool(ctx, o, "alwaysOnTop", opts.alwaysOnTop);
        opts.hidden      = qjsbind::get_prop_bool(ctx, o, "hidden", opts.hidden);
    }
    if (opts.width < 1) opts.width = 1;
    if (opts.height < 1) opts.height = 1;

    uint64_t id = s_engine->openWindowHost(opts);
    if (id == 0)
        return JS_ThrowTypeError(ctx,
            "bro.window.open requires a GPU window session (unavailable under --no-gpu)");

    auto* h = new WindowHandle();
    h->engine = s_engine;
    h->id = id;
    JSValue handle = qjsbind::wrap(ctx, h);
    if (JS_IsException(handle)) return handle;

    // Pin the handle until its 'close' fires (or the realm is cleaned up).
    s_handles[ctx][id] = JS_DupValue(ctx, handle);
    return handle;
}

} // namespace

void installWindowHostBindings(JSContext* ctx, engine::Engine* engine) {
    if (engine) s_engine = engine;

    registerHandleClass(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue bro = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(bro) || JS_IsNull(bro)) {
        JS_FreeValue(ctx, bro);
        bro = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, bro));
    }
    JSValue win = JS_GetPropertyStr(ctx, bro, "window");
    if (JS_IsUndefined(win) || JS_IsNull(win)) {
        // Child realms have no bro.window (installBroWindowBindings is main-
        // realm-only); give them a minimal object so open() can throw its
        // deliberate realm error instead of a property lookup TypeError.
        JS_FreeValue(ctx, win);
        win = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, bro, "window", JS_DupValue(ctx, win));
    }
    JS_SetPropertyStr(ctx, win, "open",
                      JS_NewCFunction(ctx, js_bro_window_open, "open", 2));
    JS_FreeValue(ctx, win);
    JS_FreeValue(ctx, bro);
    JS_FreeValue(ctx, global);
}

void cleanupWindowHostBindings(JSContext* ctx) {
    auto it = s_handles.find(ctx);
    if (it == s_handles.end()) return;
    for (auto& [id, val] : it->second) JS_FreeValue(ctx, val);
    s_handles.erase(it);
}

void windowHostNotifyClosed(JSContext* ctx, uint64_t id) {
    auto ctxIt = s_handles.find(ctx);
    if (ctxIt == s_handles.end()) return;
    auto it = ctxIt->second.find(id);
    if (it == ctxIt->second.end()) return;
    JSValue handle = it->second;
    // Unpin before dispatch so a listener that drops its own reference lets
    // the handle GC afterwards; `handle` keeps this call's ref until the end.
    ctxIt->second.erase(it);

    if (auto* h = qjsbind::unwrap<WindowHandle>(ctx, handle)) {
        // Copy — a listener may add/remove listeners while we iterate.
        std::vector<JSValue> listeners;
        listeners.reserve(h->closeListeners.size());
        for (JSValue& fn : h->closeListeners)
            listeners.push_back(JS_DupValue(ctx, fn));
        for (JSValue& fn : listeners) {
            JSValue ev = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, "close"));
            JS_SetPropertyStr(ctx, ev, "target", JS_DupValue(ctx, handle));
            JSValue ret = JS_Call(ctx, fn, handle, 1, &ev);
            if (JS_IsException(ret)) {
                JSValue ex = JS_GetException(ctx);
                const char* s = JS_ToCString(ctx, ex);
                if (s) {
                    LOG_ERROR("window 'close' listener: %s", s);
                    JS_FreeCString(ctx, s);
                }
                JS_FreeValue(ctx, ex);
            }
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, ev);
            JS_FreeValue(ctx, fn);
        }
    }
    JS_FreeValue(ctx, handle);
}

} // namespace bro::js
