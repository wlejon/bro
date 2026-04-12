#include "js/server_bindings.h"
#include "engine/engine.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

namespace bro::js {

static engine::Engine* s_engine = nullptr;
static JSContext* s_ctx = nullptr;

// ---------------------------------------------------------------------------
// bro.server.stop() — request graceful shutdown
// ---------------------------------------------------------------------------

static JSValue js_server_stop(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (s_engine) {
        s_engine->requestServerStop();
        LOG_INFO("[server] Stop requested");
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// bro.server.tickrate — get/set ticks per second
// ---------------------------------------------------------------------------

static JSValue js_server_get_tickrate(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!s_engine) return JS_NewFloat64(ctx, 60.0);
    return JS_NewFloat64(ctx, s_engine->serverTickRate());
}

static JSValue js_server_set_tickrate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_engine || argc < 1) return JS_UNDEFINED;
    double hz = 60.0;
    JS_ToFloat64(ctx, &hz, argv[0]);
    if (hz < 1.0) hz = 1.0;
    if (hz > 1000.0) hz = 1000.0;
    s_engine->setServerTickRate(hz);
    LOG_INFO("[server] Tick rate set to %.0f Hz", hz);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// bro.server.uptime — seconds since server started
// ---------------------------------------------------------------------------

static JSValue js_server_get_uptime(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!s_engine) return JS_NewFloat64(ctx, 0.0);
    return JS_NewFloat64(ctx, s_engine->serverUptime());
}

// ---------------------------------------------------------------------------
// Function list
// ---------------------------------------------------------------------------

static const JSCFunctionListEntry js_server_funcs[] = {
    JS_CFUNC_DEF("stop", 0, js_server_stop),
};

// ---------------------------------------------------------------------------
// Install / Cleanup
// ---------------------------------------------------------------------------

void ServerBindings::install(JSContext* ctx, engine::Engine* engine) {
    s_engine = engine;
    s_ctx = ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue serverObj = JS_NewObject(ctx);

    // Methods
    JS_SetPropertyFunctionList(ctx, serverObj, js_server_funcs,
                               sizeof(js_server_funcs) / sizeof(js_server_funcs[0]));

    // tickrate property (getter/setter)
    JSAtom tickrateAtom = JS_NewAtom(ctx, "tickrate");
    JS_DefinePropertyGetSet(ctx, serverObj, tickrateAtom,
        JS_NewCFunction(ctx, js_server_get_tickrate, "get tickrate", 0),
        JS_NewCFunction(ctx, js_server_set_tickrate, "set tickrate", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, tickrateAtom);

    // uptime property (getter only)
    JSAtom uptimeAtom = JS_NewAtom(ctx, "uptime");
    JS_DefinePropertyGetSet(ctx, serverObj, uptimeAtom,
        JS_NewCFunction(ctx, js_server_get_uptime, "get uptime", 0),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, uptimeAtom);

    JS_SetPropertyStr(ctx, broObj, "server", serverObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void ServerBindings::cleanup(JSContext* ctx) {
    s_engine = nullptr;
    s_ctx = nullptr;
}

} // namespace bro::js
