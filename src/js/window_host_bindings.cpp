// bro.window.open() — secondary OS windows.
//
// The handle covers the whole window lifecycle — open → real OS window hosting
// an isolated document realm built from `src`, geometry/title/focus control,
// capture() for the window's pixels, postMessage() to the child realm, and the
// 'load' / 'message' / 'resize' / 'close' events (see docs/window-api.js).
//
// This file is BOTH sides of the conversation:
//   • Parent side (app realm) — bro.window.open + the handle class.
//   • Child side (a host's own realm) — bro.window.parent.postMessage and
//     window.close() self-close, installed only when the context being set up
//     IS a window host's realm (the engine answers that per JSContext).
//
// Realm policy: only the MAIN app realm may open windows. The binding is
// also installed into child (iframe) realms so a call there throws a clean,
// deliberate error. The per-context handle registry below pins each handle
// object until its 'close' event has fired, so a handle the app dropped
// still receives the event.

#include "js/window_host_bindings.h"

#include "engine/engine.h"
#include "js/imagebitmap_bindings.h"
#include "js/message_queue.h"
#include "js/message_serializer.h"
#include "js/runtime.h"
#include "platform/sdl_window.h"
#include "util/log.h"

#include <array>
#include <functional>
#include <memory>

#include <qjsbind/qjsbind.h>

#include <string_view>
#include <unordered_map>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

namespace {

// One engine per process (same assumption as the other engine-bound statics
// in this directory).
engine::Engine* s_engine = nullptr;

// The JS-side handle for one window host. Owned by its JS wrapper object
// (default qjsbind finalizer path is replaced with one that also frees the
// stored listener refs).
struct WindowHandle {
    engine::Engine* engine = nullptr;
    uint64_t id = 0;
    // Listeners per event type, JS_DupValue'd. Marked in gc_mark, freed in
    // the finalizer / on removeEventListener.
    std::vector<JSValue> closeListeners;
    std::vector<JSValue> loadListeners;
    std::vector<JSValue> messageListeners;
    std::vector<JSValue> resizeListeners;

    std::vector<JSValue>* listenersFor(std::string_view type) {
        if (type == "close")   return &closeListeners;
        if (type == "load")    return &loadListeners;
        if (type == "message") return &messageListeners;
        if (type == "resize")  return &resizeListeners;
        return nullptr;
    }

    std::array<std::vector<JSValue>*, 4> allListenerLists() {
        return {&closeListeners, &loadListeners, &messageListeners,
                &resizeListeners};
    }
};

// Per-context registry: host id → dup'd handle object. Keeps the handle
// alive until its 'close' delivers; released by windowHostNotifyClosed or
// cleanupWindowHostBindings.
std::unordered_map<JSContext*, std::unordered_map<uint64_t, JSValue>> s_handles;

void handleFinalizer(JSRuntime* rt, JSValue val) {
    auto* h = static_cast<WindowHandle*>(
        JS_GetOpaque(val, qjsbind::class_id<WindowHandle>()));
    if (!h) return;
    for (auto* list : h->allListenerLists())
        for (JSValue& fn : *list) JS_FreeValueRT(rt, fn);
    delete h;
}

// Fire one handle event. `fill` decorates the event object with type-specific
// properties (message data, resize dimensions) and may be null. Listeners are
// snapshotted first — one may add or remove listeners while we iterate.
void dispatchHandleEvent(JSContext* ctx, JSValue handle, const char* type,
                         const std::function<void(JSValue)>& fill) {
    auto* h = qjsbind::unwrap<WindowHandle>(ctx, handle);
    if (!h) return;
    std::vector<JSValue>* list = h->listenersFor(type);
    if (!list || list->empty()) return;
    std::vector<JSValue> listeners;
    listeners.reserve(list->size());
    for (JSValue& fn : *list) listeners.push_back(JS_DupValue(ctx, fn));
    for (JSValue& fn : listeners) {
        JSValue ev = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, type));
        JS_SetPropertyStr(ctx, ev, "target", JS_DupValue(ctx, handle));
        if (fill) fill(ev);
        JSValue ret = JS_Call(ctx, fn, handle, 1, &ev);
        if (JS_IsException(ret)) {
            JSValue ex = JS_GetException(ctx);
            const char* str = JS_ToCString(ctx, ex);
            if (str) {
                LOG_ERROR("window '%s' listener: %s", type, str);
                JS_FreeCString(ctx, str);
            }
            JS_FreeValue(ctx, ex);
        }
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, ev);
        JS_FreeValue(ctx, fn);
    }
}

// The registry entry for one host id on one context, or JS_UNDEFINED.
JSValue handleFor(JSContext* ctx, uint64_t id) {
    auto ctxIt = s_handles.find(ctx);
    if (ctxIt == s_handles.end()) return JS_UNDEFINED;
    auto it = ctxIt->second.find(id);
    if (it == ctxIt->second.end()) return JS_UNDEFINED;
    return it->second;
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
    if (type) {
        if (auto* list = h->listenersFor(type))
            list->push_back(JS_DupValue(ctx, argv[1]));
        JS_FreeCString(ctx, type);
    }
    return JS_UNDEFINED;
}

JSValue js_handle_removeEventListener(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* h = qjsbind::unwrap<WindowHandle>(ctx, this_val);
    if (!h || argc < 2) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    if (type) {
        if (auto* list = h->listenersFor(type)) {
            for (auto it = list->begin(); it != list->end(); ++it) {
                if (JS_VALUE_GET_PTR(*it) == JS_VALUE_GET_PTR(argv[1])) {
                    JS_FreeValue(ctx, *it);
                    list->erase(it);
                    break;
                }
            }
        }
        JS_FreeCString(ctx, type);
    }
    return JS_UNDEFINED;
}

// handle.postMessage(data, transfer?) — structured clone into the child realm.
// Raw so the optional transfer list reaches the serializer untouched.
//
// The value is serialized EAGERLY, even for a window that is already gone: the
// clone is what detaches transferred ArrayBuffers and what rejects a
// non-cloneable value with a TypeError, and neither should depend on whether
// the destination happens to still be open. Delivery itself is the part that
// no-ops (web semantics for posting to a closed window).
JSValue js_handle_postMessage(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    auto* h = qjsbind::unwrap<WindowHandle>(ctx, this_val);
    if (!h || !h->engine) return JS_UNDEFINED;
    JSValue value = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue transferList = argc > 1 ? argv[1] : JS_UNDEFINED;

    auto msg = std::make_unique<Message>();
    if (!serializeMessage(ctx, value, transferList, *msg))
        return JS_EXCEPTION;
    h->engine->postMessageToWindowHost(h->id, std::move(msg));
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
            for (auto* list : h->allListenerLists())
                for (JSValue& fn : *list) JS_MarkValue(rt, fn, mark);
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
            // Engine-tracked size, not SDL's: SDL applies sizes asynchronously
            // on X11 and only commits them on the ConfigureNotify, which an
            // unmapped (hidden/headless) window never receives — so a
            // setSize/getSize pair would read back the creation size forever.
            // host->width/height is seeded at create, written by setSize, and
            // updated on WM-initiated resizes via Engine::handleHostResized.
            if (auto* host = hostFor(h)) return sizeToJS(c, host->width, host->height);
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
        // capture() — the window's pixels as ImageData ({width, height,
        // data:Uint8ClampedArray}, top-down RGBA). Authoritative: the engine
        // re-records the host document at its current size on this thread
        // rather than sampling whatever the raster thread last produced, so a
        // capture right after a DOM edit shows that edit. Null before the
        // document loads, or once the window is closed.
        .method("capture", [](WindowHandle* h, JSContext* c) -> JSValue {
            if (!h->engine || !hostFor(h)) return JS_NULL;
            int w = 0, ht = 0;
            auto pixels = h->engine->captureWindowHost(h->id, w, ht);
            if (pixels.empty() || w <= 0 || ht <= 0) return JS_NULL;
            JSValue abuf = JS_NewArrayBufferCopy(c, pixels.data(), pixels.size());
            JSValue global = JS_GetGlobalObject(c);
            JSValue u8cCtor = JS_GetPropertyStr(c, global, "Uint8ClampedArray");
            JSValue dataArr = JS_CallConstructor(c, u8cCtor, 1, &abuf);
            JS_FreeValue(c, u8cCtor);
            JS_FreeValue(c, global);
            JS_FreeValue(c, abuf);
            return ImageBitmapBindings::makeImageData(c, w, ht, dataArr);
        })
        .method_raw("postMessage", js_handle_postMessage, 2)
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
        // `provided` records which keys the caller actually passed: those win
        // over the child app's own bro.json, everything else defers to it
        // (Engine::applyChildManifestDefaults).
        auto has = [&](const char* key) {
            JSValue v = JS_GetPropertyStr(ctx, o, key);
            bool present = !JS_IsUndefined(v);
            JS_FreeValue(ctx, v);
            return present;
        };
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
        opts.minWidth  = qjsbind::get_prop_int(ctx, o, "minWidth", opts.minWidth);
        opts.minHeight = qjsbind::get_prop_int(ctx, o, "minHeight", opts.minHeight);
        opts.maxWidth  = qjsbind::get_prop_int(ctx, o, "maxWidth", opts.maxWidth);
        opts.maxHeight = qjsbind::get_prop_int(ctx, o, "maxHeight", opts.maxHeight);
        opts.provided.width       = has("width");
        opts.provided.height      = has("height");
        opts.provided.title       = has("title");
        opts.provided.resizable   = has("resizable");
        opts.provided.borderless  = has("borderless");
        opts.provided.alwaysOnTop = has("alwaysOnTop");
        opts.provided.minWidth    = has("minWidth");
        opts.provided.minHeight   = has("minHeight");
        opts.provided.maxWidth    = has("maxWidth");
        opts.provided.maxHeight   = has("maxHeight");
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

// ── Child side: bindings that only exist inside a host window's own realm ──

// bro.window.parent.postMessage(data, transfer?) — child → app realm, arriving
// as a 'message' event on the parent's handle for THIS window.
JSValue js_child_parent_postMessage(JSContext* ctx, JSValueConst /*this_val*/,
                                    int argc, JSValueConst* argv) {
    if (!s_engine) return JS_UNDEFINED;
    uint64_t id = s_engine->windowHostIdForContext(ctx);
    if (id == 0)
        return JS_ThrowTypeError(ctx,
            "bro.window.parent.postMessage is only available in a secondary window");
    JSValue value = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue transferList = argc > 1 ? argv[1] : JS_UNDEFINED;
    auto msg = std::make_unique<Message>();
    if (!serializeMessage(ctx, value, transferList, *msg))
        return JS_EXCEPTION;
    s_engine->postMessageToParent(id, std::move(msg));
    return JS_UNDEFINED;
}

// window.close() inside a host realm closes THAT window — the same queued path
// as handle.close() and the OS close button, so the parent's 'close' fires and
// the document tears down at the next drain. The main app realm keeps its own
// window.close() (installWindowClose: quit the app); this one is installed
// only on host contexts and never shadows it.
JSValue js_child_window_close(JSContext* ctx, JSValueConst /*this_val*/,
                              int /*argc*/, JSValueConst* /*argv*/) {
    if (!s_engine) return JS_UNDEFINED;
    if (uint64_t id = s_engine->windowHostIdForContext(ctx))
        s_engine->closeWindowHost(id);
    return JS_UNDEFINED;
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

    // Is this context a secondary window's own realm? The engine knows: the
    // host's jsCtx is assigned before the realm's bindings are installed, so
    // the lookup already answers here. Only such a realm gets a parent to talk
    // to and a window of its own to close.
    if (s_engine && s_engine->windowHostIdForContext(ctx) != 0) {
        JSValue parent = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, parent, "postMessage",
            JS_NewCFunction(ctx, js_child_parent_postMessage, "postMessage", 2));
        JS_SetPropertyStr(ctx, win, "parent", parent);
        JS_SetPropertyStr(ctx, global, "close",
            JS_NewCFunction(ctx, js_child_window_close, "close", 0));
    }
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

// Fire one handle event without unpinning the handle (used by 'load',
// 'message' and 'resize'; 'close' has its own unpin-first ordering below).
void windowHostNotifyLoaded(JSContext* ctx, uint64_t id) {
    JSValue handle = handleFor(ctx, id);
    if (JS_IsUndefined(handle)) return;
    dispatchHandleEvent(ctx, handle, "load", nullptr);
}

void windowHostNotifyMessage(JSContext* ctx, uint64_t id, JSValue data) {
    JSValue handle = handleFor(ctx, id);
    if (JS_IsUndefined(handle)) {
        // The handle is gone (window closed, realm reloaded): the message has
        // nowhere to land. Dropping it is the whole no-op story.
        JS_FreeValue(ctx, data);
        return;
    }
    dispatchHandleEvent(ctx, handle, "message", [&](JSValue ev) {
        JS_SetPropertyStr(ctx, ev, "data", JS_DupValue(ctx, data));
    });
    JS_FreeValue(ctx, data);
}

void windowHostNotifyResized(JSContext* ctx, uint64_t id, int width, int height) {
    JSValue handle = handleFor(ctx, id);
    if (JS_IsUndefined(handle)) return;
    dispatchHandleEvent(ctx, handle, "resize", [&](JSValue ev) {
        JS_SetPropertyStr(ctx, ev, "width", JS_NewInt32(ctx, width));
        JS_SetPropertyStr(ctx, ev, "height", JS_NewInt32(ctx, height));
    });
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
    dispatchHandleEvent(ctx, handle, "close", nullptr);
    JS_FreeValue(ctx, handle);
}

} // namespace bro::js
