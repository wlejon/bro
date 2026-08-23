#include "js/time_bindings.h"
#include "engine/engine.h"

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ---------------------------------------------------------------------------
// Engine pointer stash (no pinned JSValues, no finalizer-order hazard).
// ---------------------------------------------------------------------------

static const char* kTimeEngineKey = "__bro_time_engine_ptr";

static engine::Engine* getEngine(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kTimeEngineKey);
    engine::Engine* e = nullptr;
    if (JS_IsNumber(val)) {
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        e = reinterpret_cast<engine::Engine*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return e;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

static JSValue js_time_get_scale(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* eng = getEngine(ctx);
    return JS_NewFloat64(ctx, static_cast<double>(eng ? eng->timeScale() : 1.0));
}

static JSValue js_time_set_scale(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = getEngine(ctx);
    if (!eng || argc < 1) return JS_UNDEFINED;
    double v = 1.0;
    if (JS_ToFloat64(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    eng->setTimeScale(v);  // clamps to [0, 100]; ignores non-finite
    return JS_UNDEFINED;
}

static JSValue js_time_get_paused(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* eng = getEngine(ctx);
    return JS_NewBool(ctx, eng ? eng->timePaused() : false);
}

static JSValue js_time_set_paused(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = getEngine(ctx);
    if (!eng || argc < 1) return JS_UNDEFINED;
    eng->setTimePaused(JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_time_get_now(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* eng = getEngine(ctx);
    return JS_NewFloat64(ctx, static_cast<double>(eng ? eng->timeNowMs() : 0.0));
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void TimeBindings::install(JSContext* ctx, engine::Engine* engine) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, kTimeEngineKey,
                      JS_NewInt64(ctx, static_cast<int64_t>(
                          reinterpret_cast<intptr_t>(engine))));

    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue timeObj = JS_NewObject(ctx);

    auto defineGetSet = [&](const char* name, JSCFunction* getter,
                            JSCFunction* setter) {
        JSAtom atom = JS_NewAtom(ctx, name);
        JS_DefinePropertyGetSet(ctx, timeObj, atom,
            JS_NewCFunction(ctx, getter, name, 0),
            setter ? JS_NewCFunction(ctx, setter, name, 1) : JS_UNDEFINED,
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    };
    defineGetSet("scale",  js_time_get_scale,  js_time_set_scale);
    defineGetSet("paused",  js_time_get_paused,  js_time_set_paused);
    defineGetSet("now",  js_time_get_now,  nullptr);

    JS_SetPropertyStr(ctx, broObj, "time", timeObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}


} // namespace bro::js
