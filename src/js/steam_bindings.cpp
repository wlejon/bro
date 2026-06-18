#include "js/steam_bindings.h"
#include "steam/steam_service.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

#include <string>

namespace bro::js {

// ---------------------------------------------------------------------------
// Per-JSContext state. Thread-local because each JSContext lives on exactly one
// thread — same rationale as net_bindings.
// ---------------------------------------------------------------------------
struct SteamCtxState {
    steam::SteamService* service = nullptr;
    steam::SteamSubscriber* subscriber = nullptr;
    JSContext* ctx = nullptr;

    JSValue onPulse = JS_UNDEFINED;
};

static thread_local SteamCtxState* s_state = nullptr;

static SteamCtxState* getState() { return s_state; }

// ---------------------------------------------------------------------------
// Probe getters (always safe — report the inert stub when unavailable).
// ---------------------------------------------------------------------------
static JSValue js_steam_get_available(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    return JS_NewBool(ctx, s && s->service && s->service->available());
}

static JSValue js_steam_get_reason(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    const char* r = (s && s->service) ? s->service->reason() : "bro.steam not installed";
    return JS_NewString(ctx, r);
}

// SteamID64 is a full uint64 — return it as a decimal STRING so JS doesn't lose
// precision above 2^53. "0" when unavailable.
static JSValue js_steam_get_steamId(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    uint64_t id = (s && s->service && s->service->available()) ? s->service->localSteamId() : 0;
    return JS_NewString(ctx, std::to_string(id).c_str());
}

static JSValue js_steam_get_personaName(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    if (s && s->service && s->service->available())
        return JS_NewString(ctx, s->service->personaName().c_str());
    return JS_NewString(ctx, "");
}

static JSValue js_steam_get_appId(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    uint32_t appId = (s && s->service && s->service->available()) ? s->service->appId() : 0;
    return JS_NewUint32(ctx, appId);
}

// ---------------------------------------------------------------------------
// onpulse callback accessor (heartbeat from the RunCallbacks pump — M1).
// ---------------------------------------------------------------------------
static JSValue js_steam_get_onpulse(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    return s ? JS_DupValue(ctx, s->onPulse) : JS_UNDEFINED;
}

static JSValue js_steam_set_onpulse(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s) return JS_UNDEFINED;
    JS_FreeValue(ctx, s->onPulse);
    s->onPulse = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Install / Cleanup / Poll
// ---------------------------------------------------------------------------
static void defineGetter(JSContext* ctx, JSValue obj, const char* name, JSCFunction* getter) {
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, obj, atom,
        JS_NewCFunction(ctx, getter, name, 0),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
}

void SteamBindings::install(JSContext* ctx, steam::SteamService* service) {
    if (s_state) {
        LOG_WARN("[steam] SteamBindings::install called twice on the same thread");
        return;
    }
    auto* state = new SteamCtxState();
    state->service = service;
    state->ctx = ctx;
    state->subscriber = service ? service->createSubscriber() : nullptr;
    s_state = state;

    if (state->subscriber) {
        // Fires synchronously during poll() on this context's thread.
        state->subscriber->onPulse = [ctx](uint64_t tick) {
            auto* s = getState();
            if (!s || JS_IsUndefined(s->onPulse) || JS_IsNull(s->onPulse)) return;
            JSValue func = JS_DupValue(ctx, s->onPulse);
            JSValue arg = JS_NewInt64(ctx, static_cast<int64_t>(tick));
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, arg);
            JS_FreeValue(ctx, func);
        };
    }

    // Build bro.steam namespace.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue steamObj = JS_NewObject(ctx);
    defineGetter(ctx, steamObj, "available",    js_steam_get_available);
    defineGetter(ctx, steamObj, "reason",       js_steam_get_reason);
    defineGetter(ctx, steamObj, "steamId",      js_steam_get_steamId);
    defineGetter(ctx, steamObj, "personaName",  js_steam_get_personaName);
    defineGetter(ctx, steamObj, "appId",        js_steam_get_appId);

    JSAtom aPulse = JS_NewAtom(ctx, "onpulse");
    JS_DefinePropertyGetSet(ctx, steamObj, aPulse,
        JS_NewCFunction(ctx, js_steam_get_onpulse, "get onpulse", 0),
        JS_NewCFunction(ctx, js_steam_set_onpulse, "set onpulse", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, aPulse);

    JS_SetPropertyStr(ctx, broObj, "steam", steamObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void SteamBindings::cleanup(JSContext* ctx) {
    SteamCtxState* s = s_state;
    if (!s) return;
    s_state = nullptr;

    if (ctx) JS_FreeValue(ctx, s->onPulse);

    if (s->service && s->subscriber) {
        s->service->destroySubscriber(s->subscriber);
    }
    delete s;
}

void SteamBindings::poll(JSContext* ctx) {
    (void)ctx;
    auto* s = getState();
    if (!s || !s->subscriber) return;
    s->subscriber->poll();
}

} // namespace bro::js
