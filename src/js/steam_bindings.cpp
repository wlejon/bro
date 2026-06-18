#include "js/steam_bindings.h"
#include "steam/steam_service.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

#include <array>
#include <cstring>
#include <string>
#include <unordered_map>
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
    JSValue onLobbyEntered = JS_UNDEFINED;
    JSValue onLobbyUpdated = JS_UNDEFINED;
    JSValue onLobbyLeft = JS_UNDEFINED;
    JSValue onLobbyInvite = JS_UNDEFINED;
    JSValue onLobbyJoinRequest = JS_UNDEFINED;
    JSValue onVoiceCaptured = JS_UNDEFINED;

    // JS-thread-owned cache. Updated only during poll() from FriendsUpdated
    // events (ownership transferred from the service thread), read synchronously
    // by getFriends() — so the JS thread never touches Steam state concurrently.
    std::vector<steam::FriendInfo> friends;

    // Same pattern for lobbies: LobbyUpdated events refresh this cache, and
    // getLobbyMembers/getLobbyOwner/getLobbyData read it synchronously.
    std::unordered_map<uint64_t, steam::LobbyState> lobbies;

    // In-flight getAvatar() promises, keyed by request id. The two stored values
    // are the promise's [resolve, reject] functions (owned; freed on settle).
    std::unordered_map<uint32_t, std::array<JSValue, 2>> pendingAvatars;
    uint32_t nextAvatarReq = 1;

    // In-flight createLobby()/joinLobby() promises, keyed by request id. reqIds
    // are unique across both (one shared counter), and each id is consumed by
    // exactly one of onLobbyCreated/onLobbyEntered, so a single map is safe.
    std::unordered_map<uint32_t, std::array<JSValue, 2>> pendingLobby;
    uint32_t nextLobbyReq = 1;

    // In-flight decodeVoice() promises, keyed by request id.
    std::unordered_map<uint32_t, std::array<JSValue, 2>> pendingVoice;
    uint32_t nextVoiceReq = 1;
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

// activateInviteDialog(lobbyId) — opens the Steam overlay's invite-to-lobby panel.
static JSValue js_steam_activateInviteDialog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s || !s->service || argc < 1) return JS_UNDEFINED;
    const char* idStr = JS_ToCString(ctx, argv[0]);
    uint64_t id = 0;
    if (idStr) { try { id = std::stoull(idStr); } catch (...) { id = 0; } JS_FreeCString(ctx, idStr); }
    if (id) s->service->activateInviteDialog(id);
    return JS_UNDEFINED;
}

// getAvatar(steamId, size="medium") -> Promise<{width,height,data:Uint8ClampedArray}|null>
// Resolves null when the avatar isn't loaded yet (re-request after onfriends).
static JSValue js_steam_getAvatar(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;

    auto settleNull = [&]() {
        JSValue n = JS_NULL;
        JSValue r = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &n);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
    };

    auto* s = getState();
    if (!s || !s->service || !s->subscriber || argc < 1) { settleNull(); return promise; }

    const char* idStr = JS_ToCString(ctx, argv[0]);
    uint64_t id = 0;
    if (idStr) { try { id = std::stoull(idStr); } catch (...) { id = 0; } JS_FreeCString(ctx, idStr); }
    if (!id) { settleNull(); return promise; }

    int size = 1; // medium
    if (argc > 1) {
        const char* sz = JS_ToCString(ctx, argv[1]);
        if (sz) {
            if (!std::strcmp(sz, "small")) size = 0;
            else if (!std::strcmp(sz, "large")) size = 2;
            else size = 1;
            JS_FreeCString(ctx, sz);
        }
    }

    uint32_t reqId = s->nextAvatarReq++;
    s->pendingAvatars[reqId] = { resolving[0], resolving[1] }; // take ownership
    s->service->requestAvatar(s->subscriber->id(), reqId, id, size);
    return promise;
}

// ---------------------------------------------------------------------------
// Lobbies (M3)
// ---------------------------------------------------------------------------
static uint64_t parseSteamId(JSContext* ctx, JSValueConst v) {
    const char* s = JS_ToCString(ctx, v);
    uint64_t id = 0;
    if (s) {
        try { id = std::stoull(s); } catch (...) { id = 0; }
        JS_FreeCString(ctx, s);
    }
    return id;
}

// ELobbyType: 0 private, 1 friends-only, 2 public, 3 invisible.
static int lobbyTypeFromString(const char* s) {
    if (!s) return 2;
    if (!std::strcmp(s, "private")) return 0;
    if (!std::strcmp(s, "friendsonly") || !std::strcmp(s, "friends")) return 1;
    if (!std::strcmp(s, "invisible")) return 3;
    return 2; // public (default)
}

// createLobby(type="public", maxMembers=8) -> Promise<lobbyId:string | null>
static JSValue js_steam_createLobby(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;

    auto* s = getState();
    if (!s || !s->service || !s->subscriber) {
        JSValue n = JS_NULL;
        JSValue r = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &n);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        return promise;
    }

    int type = 2;
    if (argc > 0) {
        const char* t = JS_ToCString(ctx, argv[0]);
        type = lobbyTypeFromString(t);
        if (t) JS_FreeCString(ctx, t);
    }
    int maxMembers = 8;
    if (argc > 1) { int32_t m = 8; JS_ToInt32(ctx, &m, argv[1]); if (m > 0) maxMembers = m; }

    uint32_t reqId = s->nextLobbyReq++;
    s->pendingLobby[reqId] = { resolving[0], resolving[1] };
    s->service->createLobby(s->subscriber->id(), reqId, type, maxMembers);
    return promise;
}

// joinLobby(lobbyId) -> Promise<{ success:bool, lobbyId:string, response:int }>
static JSValue js_steam_joinLobby(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;

    auto* s = getState();
    uint64_t id = (argc > 0) ? parseSteamId(ctx, argv[0]) : 0;
    if (!s || !s->service || !s->subscriber || !id) {
        JSValue res = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, res, "success", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, res, "lobbyId", JS_NewString(ctx, "0"));
        JS_SetPropertyStr(ctx, res, "response", JS_NewInt32(ctx, 0));
        JSValue r = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &res);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        return promise;
    }

    uint32_t reqId = s->nextLobbyReq++;
    s->pendingLobby[reqId] = { resolving[0], resolving[1] };
    s->service->joinLobby(s->subscriber->id(), reqId, id);
    return promise;
}

static JSValue js_steam_leaveLobby(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s || !s->service || argc < 1) return JS_UNDEFINED;
    uint64_t id = parseSteamId(ctx, argv[0]);
    if (id) {
        s->service->leaveLobby(id);
        s->lobbies.erase(id); // drop the cache entry immediately
    }
    return JS_UNDEFINED;
}

static JSValue js_steam_setLobbyData(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s || !s->service || argc < 3) return JS_NewBool(ctx, false);
    uint64_t id = parseSteamId(ctx, argv[0]);
    const char* key = JS_ToCString(ctx, argv[1]);
    const char* val = JS_ToCString(ctx, argv[2]);
    if (id && key && val) s->service->setLobbyData(id, key, val);
    if (key) JS_FreeCString(ctx, key);
    if (val) JS_FreeCString(ctx, val);
    return JS_NewBool(ctx, id != 0);
}

static JSValue js_steam_setLobbyMemberData(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s || !s->service || argc < 3) return JS_UNDEFINED;
    uint64_t id = parseSteamId(ctx, argv[0]);
    const char* key = JS_ToCString(ctx, argv[1]);
    const char* val = JS_ToCString(ctx, argv[2]);
    if (id && key && val) s->service->setLobbyMemberData(id, key, val);
    if (key) JS_FreeCString(ctx, key);
    if (val) JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

static JSValue js_steam_setLobbyJoinable(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s || !s->service || argc < 2) return JS_UNDEFINED;
    uint64_t id = parseSteamId(ctx, argv[0]);
    if (id) s->service->setLobbyJoinable(id, JS_ToBool(ctx, argv[1]) != 0);
    return JS_UNDEFINED;
}

static JSValue js_steam_setLobbyType(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s || !s->service || argc < 2) return JS_UNDEFINED;
    uint64_t id = parseSteamId(ctx, argv[0]);
    const char* t = JS_ToCString(ctx, argv[1]);
    if (id) s->service->setLobbyType(id, lobbyTypeFromString(t));
    if (t) JS_FreeCString(ctx, t);
    return JS_UNDEFINED;
}

static JSValue js_steam_setLobbyMemberLimit(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s || !s->service || argc < 2) return JS_UNDEFINED;
    uint64_t id = parseSteamId(ctx, argv[0]);
    int32_t limit = 0; JS_ToInt32(ctx, &limit, argv[1]);
    if (id && limit > 0) s->service->setLobbyMemberLimit(id, limit);
    return JS_UNDEFINED;
}

// getLobbyMembers(lobbyId) -> [{ steamId:string, name:string }] (from cache)
static JSValue js_steam_getLobbyMembers(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValue arr = JS_NewArray(ctx);
    auto* s = getState();
    if (!s || argc < 1) return arr;
    uint64_t id = parseSteamId(ctx, argv[0]);
    auto it = s->lobbies.find(id);
    if (it == s->lobbies.end()) return arr;
    uint32_t i = 0;
    for (const auto& m : it->second.members) {
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "steamId", JS_NewString(ctx, std::to_string(m.steamId).c_str()));
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, m.name.c_str()));
        JS_SetPropertyUint32(ctx, arr, i++, o);
    }
    return arr;
}

// getLobbyOwner(lobbyId) -> steamId string ("0" if unknown)
static JSValue js_steam_getLobbyOwner(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    uint64_t owner = 0;
    if (s && argc >= 1) {
        uint64_t id = parseSteamId(ctx, argv[0]);
        auto it = s->lobbies.find(id);
        if (it != s->lobbies.end()) owner = it->second.owner;
    }
    return JS_NewString(ctx, std::to_string(owner).c_str());
}

// getLobbyData(lobbyId, key) -> value string ("" if absent)
static JSValue js_steam_getLobbyData(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s || argc < 2) return JS_NewString(ctx, "");
    uint64_t id = parseSteamId(ctx, argv[0]);
    const char* key = JS_ToCString(ctx, argv[1]);
    std::string out;
    if (key) {
        auto it = s->lobbies.find(id);
        if (it != s->lobbies.end()) {
            for (const auto& [k, v] : it->second.data)
                if (k == key) { out = v; break; }
        }
        JS_FreeCString(ctx, key);
    }
    return JS_NewString(ctx, out.c_str());
}

static JSValue js_steam_get_onlobbyentered(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    return s ? JS_DupValue(ctx, s->onLobbyEntered) : JS_UNDEFINED;
}
static JSValue js_steam_set_onlobbyentered(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s) return JS_UNDEFINED;
    JS_FreeValue(ctx, s->onLobbyEntered);
    s->onLobbyEntered = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
}
static JSValue js_steam_get_onlobbyupdated(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    return s ? JS_DupValue(ctx, s->onLobbyUpdated) : JS_UNDEFINED;
}
static JSValue js_steam_set_onlobbyupdated(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s) return JS_UNDEFINED;
    JS_FreeValue(ctx, s->onLobbyUpdated);
    s->onLobbyUpdated = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
}
static JSValue js_steam_get_onlobbyleft(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    return s ? JS_DupValue(ctx, s->onLobbyLeft) : JS_UNDEFINED;
}
static JSValue js_steam_set_onlobbyleft(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s) return JS_UNDEFINED;
    JS_FreeValue(ctx, s->onLobbyLeft);
    s->onLobbyLeft = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
}

// Convert a LobbyState snapshot to a JS object. `members` is only populated for
// joined lobbies; for requestLobbyList results it's empty (memberCount stands in).
static JSValue lobbyStateToObject(JSContext* ctx, const steam::LobbyState& st) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "lobbyId", JS_NewString(ctx, std::to_string(st.lobbyId).c_str()));
    JS_SetPropertyStr(ctx, o, "owner", JS_NewString(ctx, std::to_string(st.owner).c_str()));
    JS_SetPropertyStr(ctx, o, "memberCount", JS_NewInt32(ctx, st.memberCount));
    JS_SetPropertyStr(ctx, o, "memberLimit", JS_NewInt32(ctx, st.memberLimit));
    JSValue data = JS_NewObject(ctx);
    for (const auto& [k, v] : st.data)
        JS_SetPropertyStr(ctx, data, k.c_str(), JS_NewString(ctx, v.c_str()));
    JS_SetPropertyStr(ctx, o, "data", data);
    JSValue members = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const auto& m : st.members) {
        JSValue mo = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, mo, "steamId", JS_NewString(ctx, std::to_string(m.steamId).c_str()));
        JS_SetPropertyStr(ctx, mo, "name", JS_NewString(ctx, m.name.c_str()));
        JS_SetPropertyUint32(ctx, members, i++, mo);
    }
    JS_SetPropertyStr(ctx, o, "members", members);
    return o;
}

static int distanceFromString(const char* s) {
    if (!s) return 1;
    if (!std::strcmp(s, "close")) return 0;
    if (!std::strcmp(s, "far")) return 2;
    if (!std::strcmp(s, "worldwide")) return 3;
    return 1; // default (same region)
}

// requestLobbyList(opts) -> Promise<[{lobbyId, owner, memberCount, memberLimit, data}]>
// opts: { stringFilters:{k:v}, numberFilters:{k:n}, maxResults:int, distance:'worldwide' }
static JSValue js_steam_requestLobbyList(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;

    auto* s = getState();
    if (!s || !s->service || !s->subscriber) {
        JSValue empty = JS_NewArray(ctx);
        JSValue r = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &empty);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, empty);
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        return promise;
    }

    std::vector<steam::LobbyListFilter> filters;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];
        // String equality filters.
        JSValue sf = JS_GetPropertyStr(ctx, opts, "stringFilters");
        if (JS_IsObject(sf)) {
            JSPropertyEnum* tab = nullptr;
            uint32_t len = 0;
            if (JS_GetOwnPropertyNames(ctx, &tab, &len, sf, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < len; ++i) {
                    const char* key = JS_AtomToCString(ctx, tab[i].atom);
                    JSValue v = JS_GetProperty(ctx, sf, tab[i].atom);
                    const char* val = JS_ToCString(ctx, v);
                    if (key && val) {
                        steam::LobbyListFilter f;
                        f.kind = steam::LobbyListFilter::String;
                        f.key = key; f.sval = val; f.comparison = 0;
                        filters.push_back(std::move(f));
                    }
                    if (val) JS_FreeCString(ctx, val);
                    if (key) JS_FreeCString(ctx, key);
                    JS_FreeValue(ctx, v);
                    JS_FreeAtom(ctx, tab[i].atom);
                }
                js_free(ctx, tab);
            }
        }
        JS_FreeValue(ctx, sf);
        // Numeric equality filters.
        JSValue nf = JS_GetPropertyStr(ctx, opts, "numberFilters");
        if (JS_IsObject(nf)) {
            JSPropertyEnum* tab = nullptr;
            uint32_t len = 0;
            if (JS_GetOwnPropertyNames(ctx, &tab, &len, nf, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < len; ++i) {
                    const char* key = JS_AtomToCString(ctx, tab[i].atom);
                    JSValue v = JS_GetProperty(ctx, nf, tab[i].atom);
                    int32_t num = 0; JS_ToInt32(ctx, &num, v);
                    if (key) {
                        steam::LobbyListFilter f;
                        f.kind = steam::LobbyListFilter::Numeric;
                        f.key = key; f.ival = num; f.comparison = 0;
                        filters.push_back(std::move(f));
                        JS_FreeCString(ctx, key);
                    }
                    JS_FreeValue(ctx, v);
                    JS_FreeAtom(ctx, tab[i].atom);
                }
                js_free(ctx, tab);
            }
        }
        JS_FreeValue(ctx, nf);
        // Distance filter.
        JSValue dist = JS_GetPropertyStr(ctx, opts, "distance");
        if (JS_IsString(dist)) {
            const char* d = JS_ToCString(ctx, dist);
            steam::LobbyListFilter f;
            f.kind = steam::LobbyListFilter::Distance;
            f.ival = distanceFromString(d);
            filters.push_back(std::move(f));
            if (d) JS_FreeCString(ctx, d);
        }
        JS_FreeValue(ctx, dist);
        // Result-count cap.
        JSValue mr = JS_GetPropertyStr(ctx, opts, "maxResults");
        if (JS_IsNumber(mr)) {
            int32_t n = 0; JS_ToInt32(ctx, &n, mr);
            if (n > 0) {
                steam::LobbyListFilter f;
                f.kind = steam::LobbyListFilter::ResultCount;
                f.ival = n;
                filters.push_back(std::move(f));
            }
        }
        JS_FreeValue(ctx, mr);
    }

    uint32_t reqId = s->nextLobbyReq++;
    s->pendingLobby[reqId] = { resolving[0], resolving[1] };
    s->service->requestLobbyList(s->subscriber->id(), reqId, std::move(filters));
    return promise;
}

static JSValue js_steam_inviteUserToLobby(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s || !s->service || argc < 2) return JS_NewBool(ctx, false);
    uint64_t lobby = parseSteamId(ctx, argv[0]);
    uint64_t invitee = parseSteamId(ctx, argv[1]);
    if (lobby && invitee) s->service->inviteUserToLobby(lobby, invitee);
    return JS_NewBool(ctx, lobby != 0 && invitee != 0);
}

static JSValue js_steam_get_onlobbyinvite(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    return s ? JS_DupValue(ctx, s->onLobbyInvite) : JS_UNDEFINED;
}
static JSValue js_steam_set_onlobbyinvite(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s) return JS_UNDEFINED;
    JS_FreeValue(ctx, s->onLobbyInvite);
    s->onLobbyInvite = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
}
static JSValue js_steam_get_onlobbyjoinrequest(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    return s ? JS_DupValue(ctx, s->onLobbyJoinRequest) : JS_UNDEFINED;
}
static JSValue js_steam_set_onlobbyjoinrequest(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s) return JS_UNDEFINED;
    JS_FreeValue(ctx, s->onLobbyJoinRequest);
    s->onLobbyJoinRequest = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Voice (M4)
// ---------------------------------------------------------------------------
static JSValue js_steam_startVoiceRecording(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    if (s && s->service) s->service->startVoiceRecording();
    return JS_UNDEFINED;
}
static JSValue js_steam_stopVoiceRecording(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    if (s && s->service) s->service->stopVoiceRecording();
    return JS_UNDEFINED;
}
static JSValue js_steam_get_isVoiceRecording(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    return JS_NewBool(ctx, s && s->service && s->service->voiceRecording());
}
static JSValue js_steam_get_voiceSampleRate(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    return JS_NewUint32(ctx, (s && s->service) ? s->service->voiceSampleRate() : 0);
}

// decodeVoice(compressed, desiredSampleRate=0) ->
//   Promise<{ pcm:Float32Array, sampleRate:int }>  (empty pcm if no/garbage data)
static JSValue js_steam_decodeVoice(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;

    auto settleEmpty = [&]() {
        JSValue res = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, res, "pcm", qjsbind::make_float32_array(ctx, nullptr, 0));
        JS_SetPropertyStr(ctx, res, "sampleRate", JS_NewInt32(ctx, 0));
        JSValue r = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &res);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
    };

    auto* s = getState();
    if (!s || !s->service || !s->subscriber || argc < 1) { settleEmpty(); return promise; }

    // Extract compressed bytes from a typed array / ArrayBuffer.
    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &viewLen, nullptr);
    if (JS_IsException(abuf)) { JS_FreeValue(ctx, JS_GetException(ctx)); settleEmpty(); return promise; }
    size_t abufLen = 0;
    uint8_t* base = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    if (!base || viewLen == 0) { JS_FreeValue(ctx, abuf); settleEmpty(); return promise; }

    int rate = 0;
    if (argc > 1) { int32_t r = 0; JS_ToInt32(ctx, &r, argv[1]); if (r > 0) rate = r; }

    uint32_t reqId = s->nextVoiceReq++;
    s->pendingVoice[reqId] = { resolving[0], resolving[1] };
    s->service->decodeVoice(s->subscriber->id(), reqId, base + byteOff, viewLen, rate);
    JS_FreeValue(ctx, abuf);
    return promise;
}

static JSValue js_steam_get_onvoicecaptured(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = getState();
    return s ? JS_DupValue(ctx, s->onVoiceCaptured) : JS_UNDEFINED;
}
static JSValue js_steam_set_onvoicecaptured(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* s = getState();
    if (!s) return JS_UNDEFINED;
    JS_FreeValue(ctx, s->onVoiceCaptured);
    s->onVoiceCaptured = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
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
        state->subscriber->onAvatar = [ctx](uint32_t reqId, int w, int h,
                                            const uint8_t* rgba, size_t len) {
            auto* s = getState();
            if (!s) return;
            auto it = s->pendingAvatars.find(reqId);
            if (it == s->pendingAvatars.end()) return;
            JSValue resolve = it->second[0];
            JSValue reject  = it->second[1];
            s->pendingAvatars.erase(it);

            JSValue result;
            if (w > 0 && h > 0 && len > 0) {
                result = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, result, "width", JS_NewInt32(ctx, w));
                JS_SetPropertyStr(ctx, result, "height", JS_NewInt32(ctx, h));
                // RGBA as a Uint8ClampedArray, so it drops straight into
                // `new ImageData(data, w, h)` / createImageBitmap.
                JSValue abuf = JS_NewArrayBufferCopy(ctx, rgba, len);
                JSValue global = JS_GetGlobalObject(ctx);
                JSValue ctor = JS_GetPropertyStr(ctx, global, "Uint8ClampedArray");
                JSValue arr = JS_CallConstructor(ctx, ctor, 1, &abuf);
                JS_SetPropertyStr(ctx, result, "data", arr);
                JS_FreeValue(ctx, ctor);
                JS_FreeValue(ctx, global);
                JS_FreeValue(ctx, abuf);
            } else {
                result = JS_NULL;
            }
            JSValue r = JS_Call(ctx, resolve, JS_UNDEFINED, 1, &result);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, result);
            JS_FreeValue(ctx, resolve);
            JS_FreeValue(ctx, reject);
        };

        // createLobby() result → resolve the pending promise with the lobby id
        // (decimal string) or null on failure.
        state->subscriber->onLobbyCreated = [ctx](uint32_t reqId, uint64_t lobbyId, bool success) {
            auto* s = getState();
            if (!s) return;
            auto it = s->pendingLobby.find(reqId);
            if (it == s->pendingLobby.end()) return;
            JSValue resolve = it->second[0];
            JSValue reject  = it->second[1];
            s->pendingLobby.erase(it);
            JSValue val = (success && lobbyId)
                ? JS_NewString(ctx, std::to_string(lobbyId).c_str()) : JS_NULL;
            JSValue r = JS_Call(ctx, resolve, JS_UNDEFINED, 1, &val);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, val);
            JS_FreeValue(ctx, resolve);
            JS_FreeValue(ctx, reject);
        };
        // Entered a lobby. Resolve a pending joinLobby() promise (reqId set), then
        // always fire the onlobbyentered event.
        state->subscriber->onLobbyEntered = [ctx](uint32_t reqId, uint64_t lobbyId, int response, bool fireEvent) {
            auto* s = getState();
            if (!s) return;
            bool success = (response == 1); // k_EChatRoomEnterResponseSuccess
            if (reqId) {
                auto it = s->pendingLobby.find(reqId);
                if (it != s->pendingLobby.end()) {
                    JSValue resolve = it->second[0];
                    JSValue reject  = it->second[1];
                    s->pendingLobby.erase(it);
                    JSValue res = JS_NewObject(ctx);
                    JS_SetPropertyStr(ctx, res, "success", JS_NewBool(ctx, success));
                    JS_SetPropertyStr(ctx, res, "lobbyId", JS_NewString(ctx, std::to_string(lobbyId).c_str()));
                    JS_SetPropertyStr(ctx, res, "response", JS_NewInt32(ctx, response));
                    JSValue r = JS_Call(ctx, resolve, JS_UNDEFINED, 1, &res);
                    JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, res);
                    JS_FreeValue(ctx, resolve);
                    JS_FreeValue(ctx, reject);
                }
            }
            if (!fireEvent) return; // promise-only refire; entry already announced
            if (JS_IsUndefined(s->onLobbyEntered) || JS_IsNull(s->onLobbyEntered)) return;
            JSValue func = JS_DupValue(ctx, s->onLobbyEntered);
            JSValue argv[2] = {
                JS_NewString(ctx, std::to_string(lobbyId).c_str()),
                JS_NewBool(ctx, success),
            };
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 2, argv);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, argv[0]);
            JS_FreeValue(ctx, argv[1]);
            JS_FreeValue(ctx, func);
        };
        // Lobby membership/data changed — refresh the JS-thread cache, then fire
        // onlobbyupdated so the app re-reads getLobbyMembers/Data.
        state->subscriber->onLobbyUpdated = [ctx](const steam::LobbyState& st) {
            auto* s = getState();
            if (!s) return;
            s->lobbies[st.lobbyId] = st;
            if (JS_IsUndefined(s->onLobbyUpdated) || JS_IsNull(s->onLobbyUpdated)) return;
            JSValue func = JS_DupValue(ctx, s->onLobbyUpdated);
            JSValue arg = JS_NewString(ctx, std::to_string(st.lobbyId).c_str());
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, arg);
            JS_FreeValue(ctx, func);
        };
        state->subscriber->onLobbyLeft = [ctx](uint64_t lobbyId) {
            auto* s = getState();
            if (!s) return;
            s->lobbies.erase(lobbyId);
            if (JS_IsUndefined(s->onLobbyLeft) || JS_IsNull(s->onLobbyLeft)) return;
            JSValue func = JS_DupValue(ctx, s->onLobbyLeft);
            JSValue arg = JS_NewString(ctx, std::to_string(lobbyId).c_str());
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, arg);
            JS_FreeValue(ctx, func);
        };
        // requestLobbyList() result → resolve the pending promise with an array
        // of lobby snapshots.
        state->subscriber->onLobbyList = [ctx](uint32_t reqId, const std::vector<steam::LobbyState>& list) {
            auto* s = getState();
            if (!s) return;
            auto it = s->pendingLobby.find(reqId);
            if (it == s->pendingLobby.end()) return;
            JSValue resolve = it->second[0];
            JSValue reject  = it->second[1];
            s->pendingLobby.erase(it);
            JSValue arr = JS_NewArray(ctx);
            uint32_t i = 0;
            for (const auto& st : list)
                JS_SetPropertyUint32(ctx, arr, i++, lobbyStateToObject(ctx, st));
            JSValue r = JS_Call(ctx, resolve, JS_UNDEFINED, 1, &arr);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, arr);
            JS_FreeValue(ctx, resolve);
            JS_FreeValue(ctx, reject);
        };
        state->subscriber->onLobbyInvite = [ctx](uint64_t friendId, uint64_t lobbyId) {
            auto* s = getState();
            if (!s || JS_IsUndefined(s->onLobbyInvite) || JS_IsNull(s->onLobbyInvite)) return;
            JSValue func = JS_DupValue(ctx, s->onLobbyInvite);
            JSValue argv[2] = {
                JS_NewString(ctx, std::to_string(friendId).c_str()),
                JS_NewString(ctx, std::to_string(lobbyId).c_str()),
            };
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 2, argv);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, argv[0]);
            JS_FreeValue(ctx, argv[1]);
            JS_FreeValue(ctx, func);
        };
        state->subscriber->onLobbyJoinRequested = [ctx](uint64_t lobbyId, uint64_t friendId) {
            auto* s = getState();
            if (!s || JS_IsUndefined(s->onLobbyJoinRequest) || JS_IsNull(s->onLobbyJoinRequest)) return;
            JSValue func = JS_DupValue(ctx, s->onLobbyJoinRequest);
            JSValue argv[2] = {
                JS_NewString(ctx, std::to_string(lobbyId).c_str()),
                JS_NewString(ctx, std::to_string(friendId).c_str()),
            };
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 2, argv);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, argv[0]);
            JS_FreeValue(ctx, argv[1]);
            JS_FreeValue(ctx, func);
        };
        // Local compressed voice frame → onvoicecaptured(Uint8Array). The app
        // forwards these bytes to peers (e.g. over bro.net).
        state->subscriber->onVoiceCaptured = [ctx](const uint8_t* data, size_t len) {
            auto* s = getState();
            if (!s || JS_IsUndefined(s->onVoiceCaptured) || JS_IsNull(s->onVoiceCaptured)) return;
            JSValue func = JS_DupValue(ctx, s->onVoiceCaptured);
            JSValue abuf = JS_NewArrayBufferCopy(ctx, data, len);
            JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
            JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_UINT8);
            JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, &arr);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, arr);
            JS_FreeValue(ctx, abuf);
            JS_FreeValue(ctx, func);
        };
        // decodeVoice() result → resolve the pending promise with normalized PCM.
        state->subscriber->onVoiceDecoded = [ctx](uint32_t reqId, int sampleRate,
                                                  const uint8_t* pcm, size_t len) {
            auto* s = getState();
            if (!s) return;
            auto it = s->pendingVoice.find(reqId);
            if (it == s->pendingVoice.end()) return;
            JSValue resolve = it->second[0];
            JSValue reject  = it->second[1];
            s->pendingVoice.erase(it);
            // Steam PCM is signed 16-bit; hand JS a Float32Array in [-1,1].
            size_t n = len / sizeof(int16_t);
            std::vector<float> f(n);
            const int16_t* src = reinterpret_cast<const int16_t*>(pcm);
            for (size_t i = 0; i < n; ++i) f[i] = src[i] / 32768.0f;
            JSValue res = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, res, "pcm", qjsbind::make_float32_array(ctx, f.data(), f.size()));
            JS_SetPropertyStr(ctx, res, "sampleRate", JS_NewInt32(ctx, sampleRate));
            JSValue r = JS_Call(ctx, resolve, JS_UNDEFINED, 1, &res);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, res);
            JS_FreeValue(ctx, resolve);
            JS_FreeValue(ctx, reject);
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
    JS_SetPropertyStr(ctx, steamObj, "getAvatar",
        JS_NewCFunction(ctx, js_steam_getAvatar, "getAvatar", 2));
    JS_SetPropertyStr(ctx, steamObj, "setRichPresence",
        JS_NewCFunction(ctx, js_steam_setRichPresence, "setRichPresence", 2));
    JS_SetPropertyStr(ctx, steamObj, "clearRichPresence",
        JS_NewCFunction(ctx, js_steam_clearRichPresence, "clearRichPresence", 0));
    JS_SetPropertyStr(ctx, steamObj, "activateOverlay",
        JS_NewCFunction(ctx, js_steam_activateOverlay, "activateOverlay", 1));
    JS_SetPropertyStr(ctx, steamObj, "activateOverlayToUser",
        JS_NewCFunction(ctx, js_steam_activateOverlayToUser, "activateOverlayToUser", 2));
    JS_SetPropertyStr(ctx, steamObj, "activateInviteDialog",
        JS_NewCFunction(ctx, js_steam_activateInviteDialog, "activateInviteDialog", 1));

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

    // Lobbies (M3): lifecycle, data, membership reads.
    JS_SetPropertyStr(ctx, steamObj, "createLobby",
        JS_NewCFunction(ctx, js_steam_createLobby, "createLobby", 2));
    JS_SetPropertyStr(ctx, steamObj, "joinLobby",
        JS_NewCFunction(ctx, js_steam_joinLobby, "joinLobby", 1));
    JS_SetPropertyStr(ctx, steamObj, "leaveLobby",
        JS_NewCFunction(ctx, js_steam_leaveLobby, "leaveLobby", 1));
    JS_SetPropertyStr(ctx, steamObj, "setLobbyData",
        JS_NewCFunction(ctx, js_steam_setLobbyData, "setLobbyData", 3));
    JS_SetPropertyStr(ctx, steamObj, "setLobbyMemberData",
        JS_NewCFunction(ctx, js_steam_setLobbyMemberData, "setLobbyMemberData", 3));
    JS_SetPropertyStr(ctx, steamObj, "setLobbyJoinable",
        JS_NewCFunction(ctx, js_steam_setLobbyJoinable, "setLobbyJoinable", 2));
    JS_SetPropertyStr(ctx, steamObj, "setLobbyType",
        JS_NewCFunction(ctx, js_steam_setLobbyType, "setLobbyType", 2));
    JS_SetPropertyStr(ctx, steamObj, "setLobbyMemberLimit",
        JS_NewCFunction(ctx, js_steam_setLobbyMemberLimit, "setLobbyMemberLimit", 2));
    JS_SetPropertyStr(ctx, steamObj, "getLobbyMembers",
        JS_NewCFunction(ctx, js_steam_getLobbyMembers, "getLobbyMembers", 1));
    JS_SetPropertyStr(ctx, steamObj, "getLobbyOwner",
        JS_NewCFunction(ctx, js_steam_getLobbyOwner, "getLobbyOwner", 1));
    JS_SetPropertyStr(ctx, steamObj, "getLobbyData",
        JS_NewCFunction(ctx, js_steam_getLobbyData, "getLobbyData", 2));
    JS_SetPropertyStr(ctx, steamObj, "requestLobbyList",
        JS_NewCFunction(ctx, js_steam_requestLobbyList, "requestLobbyList", 1));
    JS_SetPropertyStr(ctx, steamObj, "inviteUserToLobby",
        JS_NewCFunction(ctx, js_steam_inviteUserToLobby, "inviteUserToLobby", 2));

    JSAtom aLobbyEnter = JS_NewAtom(ctx, "onlobbyentered");
    JS_DefinePropertyGetSet(ctx, steamObj, aLobbyEnter,
        JS_NewCFunction(ctx, js_steam_get_onlobbyentered, "get onlobbyentered", 0),
        JS_NewCFunction(ctx, js_steam_set_onlobbyentered, "set onlobbyentered", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, aLobbyEnter);

    JSAtom aLobbyUpd = JS_NewAtom(ctx, "onlobbyupdated");
    JS_DefinePropertyGetSet(ctx, steamObj, aLobbyUpd,
        JS_NewCFunction(ctx, js_steam_get_onlobbyupdated, "get onlobbyupdated", 0),
        JS_NewCFunction(ctx, js_steam_set_onlobbyupdated, "set onlobbyupdated", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, aLobbyUpd);

    JSAtom aLobbyLeft = JS_NewAtom(ctx, "onlobbyleft");
    JS_DefinePropertyGetSet(ctx, steamObj, aLobbyLeft,
        JS_NewCFunction(ctx, js_steam_get_onlobbyleft, "get onlobbyleft", 0),
        JS_NewCFunction(ctx, js_steam_set_onlobbyleft, "set onlobbyleft", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, aLobbyLeft);

    JSAtom aLobbyInvite = JS_NewAtom(ctx, "onlobbyinvite");
    JS_DefinePropertyGetSet(ctx, steamObj, aLobbyInvite,
        JS_NewCFunction(ctx, js_steam_get_onlobbyinvite, "get onlobbyinvite", 0),
        JS_NewCFunction(ctx, js_steam_set_onlobbyinvite, "set onlobbyinvite", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, aLobbyInvite);

    JSAtom aLobbyJoinReq = JS_NewAtom(ctx, "onlobbyjoinrequest");
    JS_DefinePropertyGetSet(ctx, steamObj, aLobbyJoinReq,
        JS_NewCFunction(ctx, js_steam_get_onlobbyjoinrequest, "get onlobbyjoinrequest", 0),
        JS_NewCFunction(ctx, js_steam_set_onlobbyjoinrequest, "set onlobbyjoinrequest", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, aLobbyJoinReq);

    // Voice (M4): capture toggle, decode, metering.
    JS_SetPropertyStr(ctx, steamObj, "startVoiceRecording",
        JS_NewCFunction(ctx, js_steam_startVoiceRecording, "startVoiceRecording", 0));
    JS_SetPropertyStr(ctx, steamObj, "stopVoiceRecording",
        JS_NewCFunction(ctx, js_steam_stopVoiceRecording, "stopVoiceRecording", 0));
    JS_SetPropertyStr(ctx, steamObj, "decodeVoice",
        JS_NewCFunction(ctx, js_steam_decodeVoice, "decodeVoice", 2));
    defineGetter(ctx, steamObj, "isVoiceRecording", js_steam_get_isVoiceRecording);
    defineGetter(ctx, steamObj, "voiceSampleRate", js_steam_get_voiceSampleRate);

    JSAtom aVoiceCap = JS_NewAtom(ctx, "onvoicecaptured");
    JS_DefinePropertyGetSet(ctx, steamObj, aVoiceCap,
        JS_NewCFunction(ctx, js_steam_get_onvoicecaptured, "get onvoicecaptured", 0),
        JS_NewCFunction(ctx, js_steam_set_onvoicecaptured, "set onvoicecaptured", 1),
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, aVoiceCap);

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
        JS_FreeValue(ctx, s->onLobbyEntered);
        JS_FreeValue(ctx, s->onLobbyUpdated);
        JS_FreeValue(ctx, s->onLobbyLeft);
        JS_FreeValue(ctx, s->onLobbyInvite);
        JS_FreeValue(ctx, s->onLobbyJoinRequest);
        JS_FreeValue(ctx, s->onVoiceCaptured);
        // Drop any unresolved getAvatar()/lobby/voice promises (context going away).
        for (auto& [reqId, fns] : s->pendingAvatars) {
            JS_FreeValue(ctx, fns[0]);
            JS_FreeValue(ctx, fns[1]);
        }
        for (auto& [reqId, fns] : s->pendingLobby) {
            JS_FreeValue(ctx, fns[0]);
            JS_FreeValue(ctx, fns[1]);
        }
        for (auto& [reqId, fns] : s->pendingVoice) {
            JS_FreeValue(ctx, fns[0]);
            JS_FreeValue(ctx, fns[1]);
        }
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
