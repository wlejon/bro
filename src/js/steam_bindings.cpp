#include "js/steam_bindings.h"
#include "steam/steam_service.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

#include <string>
#include <vector>

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
    JSValue onFriends = JS_UNDEFINED;
    JSValue onOverlay = JS_UNDEFINED;
    JSValue onJoinRequest = JS_UNDEFINED;

    // JS-thread-owned cache. Updated only during poll() from FriendsUpdated
    // events (ownership transferred from the service thread), read synchronously
    // by getFriends() — so the JS thread never touches Steam state concurrently.
    std::vector<steam::FriendInfo> friends;
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
// Friends (M2)
// ---------------------------------------------------------------------------
static const char* personaStateStr(int s) {
    switch (s) {
        case 0: return "offline";
        case 1: return "online";
        case 2: return "busy";
        case 3: return "away";
        case 4: return "snooze";
        case 5: return "looking-to-trade";
        case 6: return "looking-to-play";
        case 7: return "invisible";
        default: return "unknown";
    }
}

static JSValue friendToObject(JSContext* ctx, const steam::FriendInfo& f) {
    JSValue o = JS_NewObject(ctx);
    // SteamID64 as a decimal string — preserves the full uint64 in JS.
    JS_SetPropertyStr(ctx, o, "steamId", JS_NewString(ctx, std::to_string(f.steamId).c_str()));
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, f.name.c_str()));
    JS_SetPropertyStr(ctx, o, "state", JS_NewString(ctx, personaStateStr(f.personaState)));
    JS_SetPropertyStr(ctx, o, "stateCode", JS_NewInt32(ctx, f.personaState));
    JS_SetPropertyStr(ctx, o, "online", JS_NewBool(ctx, f.personaState != 0));
    JS_SetPropertyStr(ctx, o, "relationship", JS_NewInt32(ctx, f.relationship));
    return o;
}

// getFriends() → array of the cached friends (synchronous; from the last poll).
static JSValue js_steam_getFriends(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    JSValue arr = JS_NewArray(ctx);
    if (!s) return arr;
    uint32_t i = 0;
    for (const auto& f : s->friends)
        JS_SetPropertyUint32(ctx, arr, i++, friendToObject(ctx, f));
    return arr;
}

static JSValue js_steam_setRichPresence(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s || !s->service || argc < 2) return JS_NewBool(ctx, false);
    const char* key = JS_ToCString(ctx, argv[0]);
    const char* val = JS_ToCString(ctx, argv[1]);
    if (key && val) s->service->setRichPresence(key, val);
    if (key) JS_FreeCString(ctx, key);
    if (val) JS_FreeCString(ctx, val);
    return JS_NewBool(ctx, true);
}

static JSValue js_steam_clearRichPresence(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    if (s && s->service) s->service->clearRichPresence();
    return JS_UNDEFINED;
}

static JSValue js_steam_activateOverlay(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s || !s->service) return JS_UNDEFINED;
    const char* dialog = (argc > 0) ? JS_ToCString(ctx, argv[0]) : nullptr;
    s->service->activateOverlay(dialog ? dialog : "");
    if (dialog) JS_FreeCString(ctx, dialog);
    return JS_UNDEFINED;
}

static JSValue js_steam_activateOverlayToUser(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s || !s->service || argc < 2) return JS_UNDEFINED;
    const char* dialog = JS_ToCString(ctx, argv[0]);
    const char* idStr = JS_ToCString(ctx, argv[1]);
    uint64_t id = 0;
    if (idStr) { try { id = std::stoull(idStr); } catch (...) { id = 0; } }
    if (dialog && id) s->service->activateOverlayToUser(dialog, id);
    if (dialog) JS_FreeCString(ctx, dialog);
    if (idStr) JS_FreeCString(ctx, idStr);
    return JS_UNDEFINED;
}

static JSValue js_steam_get_onfriends(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    return s ? JS_DupValue(ctx, s->onFriends) : JS_UNDEFINED;
}

static JSValue js_steam_set_onfriends(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s) return JS_UNDEFINED;
    JS_FreeValue(ctx, s->onFriends);
    s->onFriends = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
}

static JSValue js_steam_get_onoverlay(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    return s ? JS_DupValue(ctx, s->onOverlay) : JS_UNDEFINED;
}

static JSValue js_steam_set_onoverlay(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s) return JS_UNDEFINED;
    JS_FreeValue(ctx, s->onOverlay);
    s->onOverlay = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
}

static JSValue js_steam_get_onjoinrequest(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    return s ? JS_DupValue(ctx, s->onJoinRequest) : JS_UNDEFINED;
}

static JSValue js_steam_set_onjoinrequest(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s) return JS_UNDEFINED;
    JS_FreeValue(ctx, s->onJoinRequest);
    s->onJoinRequest = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
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
        // Snapshot ownership stays on the service side; we copy into the JS-thread
        // cache, then notify. getFriends() reads the cache synchronously.
        state->subscriber->onFriends = [ctx](const std::vector<steam::FriendInfo>& list) {
            auto* s = getState();
            if (!s) return;
            s->friends = list;
            if (JS_IsUndefined(s->onFriends) || JS_IsNull(s->onFriends)) return;
            JSValue func = JS_DupValue(ctx, s->onFriends);
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, func);
        };
        state->subscriber->onOverlay = [ctx](bool active) {
            auto* s = getState();
            if (!s || JS_IsUndefined(s->onOverlay) || JS_IsNull(s->onOverlay)) return;
            JSValue func = JS_DupValue(ctx, s->onOverlay);
            JSValue arg = JS_NewBool(ctx, active);
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, arg);
            JS_FreeValue(ctx, func);
        };
        state->subscriber->onJoinRequest = [ctx](uint64_t friendId, const std::string& connect) {
            auto* s = getState();
            if (!s || JS_IsUndefined(s->onJoinRequest) || JS_IsNull(s->onJoinRequest)) return;
            JSValue func = JS_DupValue(ctx, s->onJoinRequest);
            JSValue argv[2] = {
                JS_NewString(ctx, std::to_string(friendId).c_str()),
                JS_NewString(ctx, connect.c_str()),
            };
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 2, argv);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, argv[0]);
            JS_FreeValue(ctx, argv[1]);
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

    // Friends (M2): list query, rich presence, overlay.
    JS_SetPropertyStr(ctx, steamObj, "getFriends",
        JS_NewCFunction(ctx, js_steam_getFriends, "getFriends", 0));
    JS_SetPropertyStr(ctx, steamObj, "setRichPresence",
        JS_NewCFunction(ctx, js_steam_setRichPresence, "setRichPresence", 2));
    JS_SetPropertyStr(ctx, steamObj, "clearRichPresence",
        JS_NewCFunction(ctx, js_steam_clearRichPresence, "clearRichPresence", 0));
    JS_SetPropertyStr(ctx, steamObj, "activateOverlay",
        JS_NewCFunction(ctx, js_steam_activateOverlay, "activateOverlay", 1));
    JS_SetPropertyStr(ctx, steamObj, "activateOverlayToUser",
        JS_NewCFunction(ctx, js_steam_activateOverlayToUser, "activateOverlayToUser", 2));

    JSAtom aFriends = JS_NewAtom(ctx, "onfriends");
    JS_DefinePropertyGetSet(ctx, steamObj, aFriends,
        JS_NewCFunction(ctx, js_steam_get_onfriends, "get onfriends", 0),
        JS_NewCFunction(ctx, js_steam_set_onfriends, "set onfriends", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, aFriends);

    JSAtom aOverlay = JS_NewAtom(ctx, "onoverlay");
    JS_DefinePropertyGetSet(ctx, steamObj, aOverlay,
        JS_NewCFunction(ctx, js_steam_get_onoverlay, "get onoverlay", 0),
        JS_NewCFunction(ctx, js_steam_set_onoverlay, "set onoverlay", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, aOverlay);

    JSAtom aJoin = JS_NewAtom(ctx, "onjoinrequest");
    JS_DefinePropertyGetSet(ctx, steamObj, aJoin,
        JS_NewCFunction(ctx, js_steam_get_onjoinrequest, "get onjoinrequest", 0),
        JS_NewCFunction(ctx, js_steam_set_onjoinrequest, "set onjoinrequest", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, aJoin);

    JS_SetPropertyStr(ctx, broObj, "steam", steamObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void SteamBindings::cleanup(JSContext* ctx) {
    SteamCtxState* s = s_state;
    if (!s) return;
    s_state = nullptr;

    if (ctx) {
        JS_FreeValue(ctx, s->onPulse);
        JS_FreeValue(ctx, s->onFriends);
        JS_FreeValue(ctx, s->onOverlay);
        JS_FreeValue(ctx, s->onJoinRequest);
    }

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
