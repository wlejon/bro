#include "js/server_bindings.h"
#include "engine/engine.h"
#include "js/worker.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

namespace bro::js {

// Main-context binding (engine-backed). One engine per process, so a
// static pointer is fine here — set by install() on the main thread.
static engine::Engine* s_engine = nullptr;
static JSContext* s_ctx = nullptr;

// Worker-context binding. Each worker thread has its own JSContext; we
// resolve the Worker pointer via a thread-local since workers can be
// running on different threads concurrently and a static map would race.
static thread_local Worker* s_worker = nullptr;

// ---------------------------------------------------------------------------
// bro.server.stop() — request graceful shutdown
// ---------------------------------------------------------------------------

static JSValue js_server_stop(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (s_worker) {
        s_worker->requestClose();
        LOG_INFO("[server] Stop requested (worker)");
        return JS_UNDEFINED;
    }
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
    if (s_worker) return JS_NewFloat64(ctx, s_worker->tickRate());
    if (!s_engine) return JS_NewFloat64(ctx, 60.0);
    return JS_NewFloat64(ctx, s_engine->serverTickRate());
}

static JSValue js_server_set_tickrate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    double hz = 60.0;
    JS_ToFloat64(ctx, &hz, argv[0]);
    if (hz < 1.0) hz = 1.0;
    if (hz > 1000.0) hz = 1000.0;
    if (s_worker) {
        s_worker->setTickRate(hz);
        LOG_INFO("[server] Tick rate set to %.0f Hz (worker)", hz);
        return JS_UNDEFINED;
    }
    if (s_engine) {
        s_engine->setServerTickRate(hz);
        LOG_INFO("[server] Tick rate set to %.0f Hz", hz);
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// bro.server.uptime — seconds since server started
// ---------------------------------------------------------------------------

static JSValue js_server_get_uptime(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (s_worker) return JS_NewFloat64(ctx, s_worker->uptimeSec());
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

// Build the bro.server object on `ctx` and attach the shared getters/
// setters. Which backend (engine vs worker) those functions talk to is
// determined by s_engine / s_worker at call time.
static void buildServerObject(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue serverObj = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, serverObj, js_server_funcs,
                               sizeof(js_server_funcs) / sizeof(js_server_funcs[0]));

    JSAtom tickrateAtom = JS_NewAtom(ctx, "tickrate");
    JS_DefinePropertyGetSet(ctx, serverObj, tickrateAtom,
        JS_NewCFunction(ctx, js_server_get_tickrate, "get tickrate", 0),
        JS_NewCFunction(ctx, js_server_set_tickrate, "set tickrate", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, tickrateAtom);

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

void ServerBindings::install(JSContext* ctx, engine::Engine* engine) {
    s_engine = engine;
    s_ctx = ctx;
    buildServerObject(ctx);
}

void ServerBindings::installWorker(JSContext* ctx, Worker* worker) {
    s_worker = worker;
    buildServerObject(ctx);
}

void ServerBindings::cleanup(JSContext* ctx) {
    // Drop bro.server so the getter/setter JSValues and the "tickrate" /
    // "uptime" atoms held by its property shape are released before the
    // runtime is freed.
    if (ctx) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
        if (JS_IsObject(broObj)) {
            JSAtom serverAtom = JS_NewAtom(ctx, "server");
            JS_DeleteProperty(ctx, broObj, serverAtom, 0);
            JS_FreeAtom(ctx, serverAtom);
        }
        JS_FreeValue(ctx, broObj);
        JS_FreeValue(ctx, global);
    }

    // Clear whichever backend is bound on this thread. On the main thread
    // this clears s_engine; on a worker thread it clears its own s_worker.
    if (s_worker) {
        s_worker = nullptr;
        return;
    }
    s_engine = nullptr;
    s_ctx = nullptr;
}

} // namespace bro::js
