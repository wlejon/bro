#include "js/matchmedia_bindings.h"
#include "js/dom_bindings_internal.h"
#include "js/runtime.h"
#include "dom/document.h"
#include "css/parser.h"
#include <qjsbind/qjsbind.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::js {

namespace {

// ---------------------------------------------------------------------------
// Wrapper state
// ---------------------------------------------------------------------------

struct ChangeListener {
    JSValue fn = JS_UNDEFINED;
    JSValue signal = JS_UNDEFINED;  // AbortSignal, or undefined
    bool capture = false;
    bool once = false;
};

struct MediaQueryListJS {
    JSContext* ctx = nullptr;
    uint64_t id = 0;
    std::string media;         // serialized query (trimmed input; "" → "all")
    bool lastMatches = false;  // last delivered/observed state (flip detection)

    JSValue onchange = JS_UNDEFINED;
    std::vector<ChangeListener> listeners; // 'change' listeners, registration order

    ~MediaQueryListJS();
};

uint64_t s_next_mql_id = 1;

std::unordered_map<uint64_t, JSValue>& mqlMirror() {
    static std::unordered_map<uint64_t, JSValue> m;
    return m;
}

std::unordered_map<uint64_t, JSValue>& mqlPins() {
    static std::unordered_map<uint64_t, JSValue> m;
    return m;
}

std::unordered_map<JSContext*, uint64_t>& lastDeliveredGen() {
    static std::unordered_map<JSContext*, uint64_t> m;
    return m;
}

MediaQueryListJS::~MediaQueryListJS() {
    mqlMirror().erase(id);
    if (ctx) {
        JSRuntime* rt = JS_GetRuntime(ctx);
        if (!JS_IsUndefined(onchange)) JS_FreeValueRT(rt, onchange);
        for (ChangeListener& l : listeners) {
            JS_FreeValueRT(rt, l.fn);
            if (!JS_IsUndefined(l.signal)) JS_FreeValueRT(rt, l.signal);
        }
    }
}

MediaQueryListJS* self(JSValueConst v) {
    return static_cast<MediaQueryListJS*>(
        JS_GetOpaque(v, qjsbind::class_id<MediaQueryListJS>()));
}

// ---------------------------------------------------------------------------
// Pinning (keep listening MQLs alive while their realm lives)
// ---------------------------------------------------------------------------

bool hasAnyListener(const MediaQueryListJS* a) {
    return !a->listeners.empty() || !JS_IsUndefined(a->onchange);
}

void updatePin(JSContext* ctx, MediaQueryListJS* a, JSValueConst obj) {
    auto& pins = mqlPins();
    auto it = pins.find(a->id);
    if (hasAnyListener(a)) {
        if (it == pins.end()) pins[a->id] = JS_DupValue(ctx, obj);
    } else if (it != pins.end()) {
        JS_FreeValue(ctx, it->second);
        pins.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

bool evaluateFor(const MediaQueryListJS* a, bool* out) {
    dom::Document* doc = getDocumentForCtx(a->ctx);
    if (!doc) return false;
    *out = htmlayout::css::evaluateMediaQuery(a->media, doc->mediaContext());
    return true;
}

// ---------------------------------------------------------------------------
// change event dispatch
// ---------------------------------------------------------------------------

void pruneAborted(JSContext* ctx, MediaQueryListJS* a);
void dropListener(JSContext* ctx, ChangeListener& l);

JSValue makeChangeEvent(JSContext* ctx, const MediaQueryListJS* a,
                        JSValueConst mqlObj) {
    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, "change"));
    JS_SetPropertyStr(ctx, ev, "matches", JS_NewBool(ctx, a->lastMatches));
    JS_SetPropertyStr(ctx, ev, "media", JS_NewString(ctx, a->media.c_str()));
    JS_SetPropertyStr(ctx, ev, "target", JS_DupValue(ctx, mqlObj));
    JS_SetPropertyStr(ctx, ev, "currentTarget", JS_DupValue(ctx, mqlObj));
    return ev;
}

void fireChange(JSContext* ctx, MediaQueryListJS* a, JSValueConst mqlObj) {
    pruneAborted(ctx, a);

    std::vector<JSValue> snap;
    snap.reserve(a->listeners.size() + 1);
    for (ChangeListener& l : a->listeners) snap.push_back(JS_DupValue(ctx, l.fn));

    for (auto it = a->listeners.begin(); it != a->listeners.end();) {
        if (it->once) {
            dropListener(ctx, *it);
            it = a->listeners.erase(it);
        } else {
            ++it;
        }
    }
    JSValue onchange = JS_IsUndefined(a->onchange)
                           ? JS_UNDEFINED
                           : JS_DupValue(ctx, a->onchange);

    JSValue ev = makeChangeEvent(ctx, a, mqlObj);
    for (JSValue& fn : snap) {
        JSValue ret = Runtime::callJs(ctx, fn, mqlObj, 1, &ev,
                                      ErrorOrigin::listener("MediaQueryList change"));
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, fn);
    }
    if (!JS_IsUndefined(onchange)) {
        JSValue ret = Runtime::callJs(ctx, onchange, mqlObj, 1, &ev,
                                      ErrorOrigin::listener("MediaQueryList onchange"));
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, onchange);
    }
    JS_FreeValue(ctx, ev);

    updatePin(ctx, a, mqlObj);
}

// ---------------------------------------------------------------------------
// Listener add/remove (shared by the standard and legacy surfaces)
// ---------------------------------------------------------------------------

bool sameFunction(JSValueConst a, JSValueConst b) {
    return JS_VALUE_GET_TAG(a) == JS_VALUE_GET_TAG(b) &&
           JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

struct ListenerOptions {
    bool capture = false;
    bool once = false;
    JSValue signal = JS_UNDEFINED;
};

ListenerOptions parseListenerOptions(JSContext* ctx, JSValueConst opts) {
    ListenerOptions o;
    if (JS_IsUndefined(opts) || JS_IsNull(opts)) return o;
    if (JS_IsBool(opts) || JS_IsNumber(opts) || JS_IsString(opts)) {
        o.capture = JS_ToBool(ctx, opts);
        return o;
    }
    if (!JS_IsObject(opts)) return o;

    JSValue v = JS_GetPropertyStr(ctx, opts, "capture");
    o.capture = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "once");
    o.once = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "signal");
    if (JS_IsObject(v)) o.signal = v;
    else JS_FreeValue(ctx, v);
    return o;
}

bool listenerAborted(JSContext* ctx, const ChangeListener& l) {
    if (JS_IsUndefined(l.signal)) return false;
    JSValue v = JS_GetPropertyStr(ctx, l.signal, "aborted");
    bool aborted = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return aborted;
}

void dropListener(JSContext* ctx, ChangeListener& l) {
    JS_FreeValue(ctx, l.fn);
    if (!JS_IsUndefined(l.signal)) JS_FreeValue(ctx, l.signal);
}

void pruneAborted(JSContext* ctx, MediaQueryListJS* a) {
    for (auto it = a->listeners.begin(); it != a->listeners.end();) {
        if (listenerAborted(ctx, *it)) {
            dropListener(ctx, *it);
            it = a->listeners.erase(it);
        } else {
            ++it;
        }
    }
}

void addChangeListener(JSContext* ctx, JSValueConst this_val, JSValueConst fn,
                       JSValueConst opts) {
    auto* a = self(this_val);
    if (!a || !JS_IsFunction(ctx, fn)) return;
    ListenerOptions o = parseListenerOptions(ctx, opts);

    if (JS_IsObject(o.signal)) {
        ChangeListener probe{ JS_UNDEFINED, o.signal, false, false };
        if (listenerAborted(ctx, probe)) {
            JS_FreeValue(ctx, o.signal);
            return;
        }
    }

    pruneAborted(ctx, a);
    for (ChangeListener& l : a->listeners) {
        if (sameFunction(l.fn, fn) && l.capture == o.capture) {
            if (JS_IsObject(o.signal)) JS_FreeValue(ctx, o.signal);
            return;
        }
    }
    a->listeners.push_back(ChangeListener{
        JS_DupValue(ctx, fn), o.signal, o.capture, o.once });
    updatePin(ctx, a, this_val);
}

void removeChangeListener(JSContext* ctx, JSValueConst this_val, JSValueConst fn,
                          JSValueConst opts) {
    auto* a = self(this_val);
    if (!a) return;
    ListenerOptions o = parseListenerOptions(ctx, opts);
    if (JS_IsObject(o.signal)) JS_FreeValue(ctx, o.signal);

    pruneAborted(ctx, a);
    for (auto it = a->listeners.begin(); it != a->listeners.end(); ++it) {
        if (sameFunction(it->fn, fn) && it->capture == o.capture) {
            dropListener(ctx, *it);
            a->listeners.erase(it);
            break;
        }
    }
    updatePin(ctx, a, this_val);
}

// ---------------------------------------------------------------------------
// MediaQueryList prototype
// ---------------------------------------------------------------------------

JSValue js_mql_get_matches(JSContext* ctx, JSValueConst this_val) {
    (void)ctx;
    auto* a = self(this_val);
    if (!a) return JS_FALSE;
    bool fresh = a->lastMatches;
    if (evaluateFor(a, &fresh)) {
        return JS_NewBool(ctx, fresh);
    }
    return JS_NewBool(ctx, a->lastMatches);
}

JSValue js_mql_get_media(JSContext* ctx, JSValueConst this_val) {
    auto* a = self(this_val);
    return JS_NewString(ctx, a ? a->media.c_str() : "");
}

JSValue js_mql_get_onchange(JSContext* ctx, JSValueConst this_val) {
    auto* a = self(this_val);
    if (!a || JS_IsUndefined(a->onchange)) return JS_NULL;
    return JS_DupValue(ctx, a->onchange);
}

JSValue js_mql_set_onchange(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* a = self(this_val);
    if (!a) return JS_UNDEFINED;
    if (!JS_IsUndefined(a->onchange)) JS_FreeValue(ctx, a->onchange);
    a->onchange = JS_IsFunction(ctx, val) ? JS_DupValue(ctx, val) : JS_UNDEFINED;
    updatePin(ctx, a, this_val);
    return JS_UNDEFINED;
}

JSValue js_mql_addEventListener(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    bool isChange = type && std::string(type) == "change";
    if (type) JS_FreeCString(ctx, type);
    if (isChange)
        addChangeListener(ctx, this_val, argv[1],
                          argc >= 3 ? argv[2] : JS_UNDEFINED);
    return JS_UNDEFINED;
}

JSValue js_mql_removeEventListener(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    bool isChange = type && std::string(type) == "change";
    if (type) JS_FreeCString(ctx, type);
    if (isChange)
        removeChangeListener(ctx, this_val, argv[1],
                             argc >= 3 ? argv[2] : JS_UNDEFINED);
    return JS_UNDEFINED;
}

JSValue js_mql_addListener(JSContext* ctx, JSValueConst this_val,
                           int argc, JSValueConst* argv) {
    if (argc >= 1) addChangeListener(ctx, this_val, argv[0], JS_UNDEFINED);
    return JS_UNDEFINED;
}

JSValue js_mql_removeListener(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    if (argc >= 1) removeChangeListener(ctx, this_val, argv[0], JS_UNDEFINED);
    return JS_UNDEFINED;
}

const JSCFunctionListEntry js_mql_proto_funcs[] = {
    JS_CGETSET_DEF("matches",  js_mql_get_matches,  nullptr),
    JS_CGETSET_DEF("media",    js_mql_get_media,    nullptr),
    JS_CGETSET_DEF("onchange", js_mql_get_onchange, js_mql_set_onchange),
    JS_CFUNC_DEF("addEventListener",    2, js_mql_addEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, js_mql_removeEventListener),
    JS_CFUNC_DEF("addListener",         1, js_mql_addListener),
    JS_CFUNC_DEF("removeListener",      1, js_mql_removeListener),
};

// ---------------------------------------------------------------------------
// window.matchMedia
// ---------------------------------------------------------------------------

JSValue js_matchMedia(JSContext* ctx, JSValueConst /*this_val*/,
                      int argc, JSValueConst* argv) {
    std::string q;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]))
        q = jsToStdString(ctx, argv[0]);
    size_t s = q.find_first_not_of(" \t\r\n");
    size_t e = q.find_last_not_of(" \t\r\n");
    q = (s == std::string::npos) ? std::string() : q.substr(s, e - s + 1);
    if (q.empty()) q = "all";

    auto* a = new MediaQueryListJS();
    a->ctx = ctx;
    a->id = s_next_mql_id++;
    a->media = std::move(q);
    bool m = false;
    if (evaluateFor(a, &m)) a->lastMatches = m;

    JSValue obj = qjsbind::wrap<MediaQueryListJS>(ctx, a);
    if (JS_IsException(obj)) return obj;
    mqlMirror()[a->id] = obj;
    return obj;
}

} // namespace

void cleanupMatchMediaBindings(JSContext* ctx) {
    auto& pins = mqlPins();
    for (auto it = pins.begin(); it != pins.end(); ) {
        auto* a = self(it->second);
        if (a && a->ctx == ctx) {
            JS_FreeValue(ctx, it->second);
            it = pins.erase(it);
        } else {
            ++it;
        }
    }
    lastDeliveredGen().erase(ctx);
}

void deliverMediaQueryChanges(JSContext* ctx) {
    dom::Document* doc = getDocumentForCtx(ctx);
    if (!doc) return;
    if (doc->mediaRestylePending()) return;
    uint64_t gen = doc->mediaGeneration();
    auto& lg = lastDeliveredGen();
    auto it = lg.find(ctx);
    if (it != lg.end() && it->second == gen) return;
    lg[ctx] = gen;

    std::vector<uint64_t> ids;
    ids.reserve(mqlMirror().size());
    for (auto& [id, obj] : mqlMirror()) {
        auto* a = self(obj);
        if (a && a->ctx == ctx) ids.push_back(id);
    }
    for (uint64_t id : ids) {
        auto mIt = mqlMirror().find(id);
        if (mIt == mqlMirror().end()) continue;
        JSValue obj = JS_DupValue(ctx, mIt->second);
        auto* a = self(obj);
        bool fresh = false;
        if (a && a->ctx == ctx && evaluateFor(a, &fresh) &&
            fresh != a->lastMatches) {
            a->lastMatches = fresh;
            fireChange(ctx, a, obj);
        }
        JS_FreeValue(ctx, obj);
    }
}

void installMatchMediaBindings(JSContext* ctx)
{
    qjsbind::Class<MediaQueryListJS>(ctx, "MediaQueryList", qjsbind::NoGlobal)
            .gc_mark([](MediaQueryListJS* a, JSRuntime* rt, JS_MarkFunc* mark) {
                if (!JS_IsUndefined(a->onchange)) JS_MarkValue(rt, a->onchange, mark);
                for (ChangeListener& l : a->listeners) {
                    JS_MarkValue(rt, l.fn, mark);
                    if (!JS_IsUndefined(l.signal)) JS_MarkValue(rt, l.signal, mark);
                }
            })
            .function_list(js_mql_proto_funcs,
                           sizeof(js_mql_proto_funcs) / sizeof(js_mql_proto_funcs[0]));
    
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "matchMedia",
                          JS_NewCFunction(ctx, js_matchMedia, "matchMedia", 1));
        JS_FreeValue(ctx, global);
}

} // namespace bro::js
