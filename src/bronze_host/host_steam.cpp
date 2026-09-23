#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "steam/steam_service.h"
#include "util/log.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

struct SteamCtxState {
    steam::SteamService* service = nullptr;
    steam::SteamSubscriber* subscriber = nullptr;

    ev::Persistent onPulse;
    ev::Persistent onFriends;
    ev::Persistent onOverlay;
    ev::Persistent onJoinRequest;
    ev::Persistent onLobbyEntered;
    ev::Persistent onLobbyUpdated;
    ev::Persistent onLobbyLeft;
    ev::Persistent onLobbyInvite;
    ev::Persistent onLobbyJoinRequest;
    ev::Persistent onVoiceCaptured;

    std::vector<steam::FriendInfo> friends;
    std::unordered_map<uint64_t, steam::LobbyState> lobbies;

    std::unordered_map<uint32_t, ev::Persistent> pendingAvatars;
    uint32_t nextAvatarReq = 1;

    std::unordered_map<uint32_t, ev::Persistent> pendingLobby;
    uint32_t nextLobbyReq = 1;

    std::unordered_map<uint32_t, ev::Persistent> pendingVoice;
    uint32_t nextVoiceReq = 1;
};

static thread_local SteamCtxState* s_steamState = nullptr;

static SteamCtxState* getSteamState() {
    return s_steamState;
}

static uint64_t parseSteamId(Value v) {
    if (ev::isString(v)) {
        std::string s = ev::toUtf8(v);
        try {
            return std::stoull(s);
        } catch (...) {
            return 0;
        }
    } else if (ev::isNumber(v)) {
        return satCast<uint64_t>(ev::toDouble(v));
    }
    return 0;
}

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

static Value friendToObject(const steam::FriendInfo& f) {
    ObjectBuilder o;
    o.set("steamId", ev::fromUtf8(std::to_string(f.steamId)));
    o.set("name", ev::fromUtf8(f.name));
    o.set("state", ev::fromUtf8(personaStateStr(f.personaState)));
    o.set("stateCode", ev::fromDouble(f.personaState));
    o.set("online", ev::fromBool(f.personaState != 0));
    o.set("relationship", ev::fromDouble(f.relationship));
    return o.get();
}

static Value lobbyStateToObject(const steam::LobbyState& st) {
    ObjectBuilder o;
    o.set("lobbyId", ev::fromUtf8(std::to_string(st.lobbyId)));
    o.set("owner", ev::fromUtf8(std::to_string(st.owner)));
    o.set("memberCount", ev::fromDouble(st.memberCount));
    o.set("memberLimit", ev::fromDouble(st.memberLimit));

    ObjectBuilder data;
    for (const auto& [k, v] : st.data) {
        data.set(k.c_str(), ev::fromUtf8(v));
    }
    o.set("data", data.get());

    Value members = hostArrayOf(st.members.size(), [&](size_t i) -> Value {
        ObjectBuilder m;
        m.set("steamId", ev::fromUtf8(std::to_string(st.members[i].steamId)));
        m.set("name", ev::fromUtf8(st.members[i].name));
        return m.get();
    });
    o.set("members", members);
    return o.get();
}

static int lobbyTypeFromString(const char* s) {
    if (!s) return 2;
    if (!std::strcmp(s, "private")) return 0;
    if (!std::strcmp(s, "friendsonly") || !std::strcmp(s, "friends")) return 1;
    if (!std::strcmp(s, "invisible")) return 3;
    return 2; // public
}

static int distanceFromString(const char* s) {
    if (!s) return 1;
    if (!std::strcmp(s, "close")) return 0;
    if (!std::strcmp(s, "far")) return 2;
    if (!std::strcmp(s, "worldwide")) return 3;
    return 1;
}

static void initSubscriberCallbacks(SteamCtxState* state) {
    if (!state || !state->subscriber) return;

    state->subscriber->onPulse = [](uint64_t tick) {
        auto* s = getSteamState();
        if (!s || !ev::isFunction(s->onPulse.get())) return;
        Value arg = ev::fromDouble(static_cast<double>(tick));
        ev::call(s->onPulse.get(), ev::undefined(), std::span<const Value>(&arg, 1));
    };

    state->subscriber->onFriends = [](const std::vector<steam::FriendInfo>& list) {
        auto* s = getSteamState();
        if (!s) return;
        s->friends = list;
        if (!ev::isFunction(s->onFriends.get())) return;
        ev::call(s->onFriends.get(), ev::undefined(), {});
    };

    state->subscriber->onOverlay = [](bool active) {
        auto* s = getSteamState();
        if (!s || !ev::isFunction(s->onOverlay.get())) return;
        Value arg = ev::fromBool(active);
        ev::call(s->onOverlay.get(), ev::undefined(), std::span<const Value>(&arg, 1));
    };

    state->subscriber->onJoinRequest = [](uint64_t friendId, const std::string& connect) {
        auto* s = getSteamState();
        if (!s || !ev::isFunction(s->onJoinRequest.get())) return;
        // Two allocations: the first string is rooted across the second.
        ev::Persistent a0(ev::fromUtf8(std::to_string(friendId)));
        ev::Persistent a1(ev::fromUtf8(connect));
        Value args[2] = {a0.get(), a1.get()};
        ev::call(s->onJoinRequest.get(), ev::undefined(), std::span<const Value>(args, 2));
    };

    state->subscriber->onAvatar = [](uint32_t reqId, int w, int h,
                                     const uint8_t* rgba, size_t len) {
        auto* s = getSteamState();
        if (!s) return;
        auto it = s->pendingAvatars.find(reqId);
        if (it == s->pendingAvatars.end()) return;
        ev::Persistent p = std::move(it->second);
        s->pendingAvatars.erase(it);

        if (w > 0 && h > 0 && len > 0) {
            ObjectBuilder res;
            res.set("width", ev::fromDouble(w));
            res.set("height", ev::fromDouble(h));
            Value dataArr = ev::createTypedArray(bronze::embed::elements::Uint8Clamped, static_cast<uint32_t>(len));
            if (rgba && len > 0) {
                ev::fillTypedArray(dataArr, std::span<const uint8_t>(rgba, len));
            }
            res.set("data", dataArr);
            ev::resolvePromise(p.get(), res.get());
        } else {
            ev::resolvePromise(p.get(), ev::null());
        }
    };

    state->subscriber->onLobbyCreated = [](uint32_t reqId, uint64_t lobbyId, bool success) {
        auto* s = getSteamState();
        if (!s) return;
        auto it = s->pendingLobby.find(reqId);
        if (it != s->pendingLobby.end()) {
            ev::Persistent p = std::move(it->second);
            s->pendingLobby.erase(it);
            Value val = (success && lobbyId) ? ev::fromUtf8(std::to_string(lobbyId)) : ev::null();
            ev::resolvePromise(p.get(), val);
        }
    };

    state->subscriber->onLobbyEntered = [](uint32_t reqId, uint64_t lobbyId, int response, bool fireEvent) {
        auto* s = getSteamState();
        if (!s) return;
        bool success = (response == 1);
        if (reqId) {
            auto it = s->pendingLobby.find(reqId);
            if (it != s->pendingLobby.end()) {
                ev::Persistent p = std::move(it->second);
                s->pendingLobby.erase(it);
                ObjectBuilder res;
                res.set("success", ev::fromBool(success));
                res.set("lobbyId", ev::fromUtf8(std::to_string(lobbyId)));
                res.set("response", ev::fromDouble(response));
                ev::resolvePromise(p.get(), res.get());
            }
        }
        if (!fireEvent) return;
        if (!ev::isFunction(s->onLobbyEntered.get())) return;
        Value args[2] = {
            ev::fromUtf8(std::to_string(lobbyId)),
            ev::fromBool(success),
        };
        ev::call(s->onLobbyEntered.get(), ev::undefined(), std::span<const Value>(args, 2));
    };

    state->subscriber->onLobbyUpdated = [](const steam::LobbyState& st) {
        auto* s = getSteamState();
        if (!s) return;
        s->lobbies[st.lobbyId] = st;
        if (!ev::isFunction(s->onLobbyUpdated.get())) return;
        Value arg = ev::fromUtf8(std::to_string(st.lobbyId));
        ev::call(s->onLobbyUpdated.get(), ev::undefined(), std::span<const Value>(&arg, 1));
    };

    state->subscriber->onLobbyLeft = [](uint64_t lobbyId) {
        auto* s = getSteamState();
        if (!s) return;
        s->lobbies.erase(lobbyId);
        if (!ev::isFunction(s->onLobbyLeft.get())) return;
        Value arg = ev::fromUtf8(std::to_string(lobbyId));
        ev::call(s->onLobbyLeft.get(), ev::undefined(), std::span<const Value>(&arg, 1));
    };

    state->subscriber->onLobbyList = [](uint32_t reqId, const std::vector<steam::LobbyState>& list) {
        auto* s = getSteamState();
        if (!s) return;
        auto it = s->pendingLobby.find(reqId);
        if (it != s->pendingLobby.end()) {
            ev::Persistent p = std::move(it->second);
            s->pendingLobby.erase(it);
            Value arr = hostArrayOf(list.size(), [&](size_t idx) {
                return lobbyStateToObject(list[idx]);
            });
            ev::resolvePromise(p.get(), arr);
        }
    };

    state->subscriber->onLobbyInvite = [](uint64_t friendId, uint64_t lobbyId) {
        auto* s = getSteamState();
        if (!s || !ev::isFunction(s->onLobbyInvite.get())) return;
        ev::Persistent a0(ev::fromUtf8(std::to_string(friendId)));
        ev::Persistent a1(ev::fromUtf8(std::to_string(lobbyId)));
        Value args[2] = {a0.get(), a1.get()};
        ev::call(s->onLobbyInvite.get(), ev::undefined(), std::span<const Value>(args, 2));
    };

    state->subscriber->onLobbyJoinRequested = [](uint64_t lobbyId, uint64_t friendId) {
        auto* s = getSteamState();
        if (!s || !ev::isFunction(s->onLobbyJoinRequest.get())) return;
        ev::Persistent a0(ev::fromUtf8(std::to_string(lobbyId)));
        ev::Persistent a1(ev::fromUtf8(std::to_string(friendId)));
        Value args[2] = {a0.get(), a1.get()};
        ev::call(s->onLobbyJoinRequest.get(), ev::undefined(), std::span<const Value>(args, 2));
    };

    state->subscriber->onVoiceCaptured = [](const uint8_t* data, size_t len) {
        auto* s = getSteamState();
        if (!s || !ev::isFunction(s->onVoiceCaptured.get())) return;
        Value arr = makeUint8Array(data, len);
        ev::call(s->onVoiceCaptured.get(), ev::undefined(), std::span<const Value>(&arr, 1));
    };

    state->subscriber->onVoiceDecoded = [](uint32_t reqId, int sampleRate,
                                           const uint8_t* pcm, size_t len) {
        auto* s = getSteamState();
        if (!s) return;
        auto it = s->pendingVoice.find(reqId);
        if (it == s->pendingVoice.end()) return;
        ev::Persistent p = std::move(it->second);
        s->pendingVoice.erase(it);

        size_t n = len / sizeof(int16_t);
        std::vector<float> f(n);
        const int16_t* src = reinterpret_cast<const int16_t*>(pcm);
        for (size_t i = 0; i < n; ++i) f[i] = src[i] / 32768.0f;

        ObjectBuilder res;
        res.set("pcm", makeFloat32Array(f));
        res.set("sampleRate", ev::fromDouble(sampleRate));
        ev::resolvePromise(p.get(), res.get());
    };
}

} // namespace

void drainSteamEvents() {
    if (s_steamState && s_steamState->subscriber) {
        s_steamState->subscriber->poll();
    }
}

void cleanupSteamBindings() {
    if (!s_steamState) return;
    if (s_steamState->service && s_steamState->subscriber) {
        s_steamState->service->destroySubscriber(s_steamState->subscriber);
    }
    delete s_steamState;
    s_steamState = nullptr;
}

Value makeBroSteamValue() {
    auto* eng = hostEngine();
    steam::SteamService* service = eng ? eng->steamService() : nullptr;

    if (!s_steamState) {
        s_steamState = new SteamCtxState();
        s_steamState->service = service;
        s_steamState->subscriber = service ? service->createSubscriber() : nullptr;
        initSubscriberCallbacks(s_steamState);
    }

    ObjectBuilder st;

    // --- Probe Getters ---
    st.accessor("available", [](Value, std::span<const Value>) -> Value {
        auto* s = getSteamState();
        return ev::fromBool(s && s->service && s->service->available());
    }, nullptr);

    st.accessor("reason", [](Value, std::span<const Value>) -> Value {
        auto* s = getSteamState();
        const char* r = (s && s->service) ? s->service->reason() : "bro.steam not installed";
        return ev::fromUtf8(r);
    }, nullptr);

    st.accessor("steamId", [](Value, std::span<const Value>) -> Value {
        auto* s = getSteamState();
        uint64_t id = (s && s->service && s->service->available()) ? s->service->localSteamId() : 0;
        return ev::fromUtf8(std::to_string(id));
    }, nullptr);

    st.accessor("personaName", [](Value, std::span<const Value>) -> Value {
        auto* s = getSteamState();
        if (s && s->service && s->service->available()) {
            return ev::fromUtf8(s->service->personaName());
        }
        return ev::fromUtf8("");
    }, nullptr);

    st.accessor("appId", [](Value, std::span<const Value>) -> Value {
        auto* s = getSteamState();
        uint32_t appId = (s && s->service && s->service->available()) ? s->service->appId() : 0;
        return ev::fromDouble(appId);
    }, nullptr);

    st.accessor("isVoiceRecording", [](Value, std::span<const Value>) -> Value {
        auto* s = getSteamState();
        return ev::fromBool(s && s->service && s->service->voiceRecording());
    }, nullptr);

    st.accessor("voiceSampleRate", [](Value, std::span<const Value>) -> Value {
        auto* s = getSteamState();
        uint32_t rate = (s && s->service) ? s->service->voiceSampleRate() : 0;
        return ev::fromDouble(rate);
    }, nullptr);

    st.accessor("isLoggedOn", [](Value, std::span<const Value>) -> Value {
        auto* s = getSteamState();
        return ev::fromBool(s && s->service && s->service->available());
    }, nullptr);

    // --- Callback Accessors ---
    auto registerCallbackAccessor = [&st](const char* propName, auto memberPtr) {
        st.accessor(propName,
            [memberPtr](Value, std::span<const Value>) -> Value {
                auto* s = getSteamState();
                if (!s) return ev::undefined();
                auto& p = s->*memberPtr;
                return p.get();
            },
            [memberPtr](Value, std::span<const Value> a) -> Value {
                auto* s = getSteamState();
                if (!s) return ev::undefined();
                auto& p = s->*memberPtr;
                if (a.empty() || ev::isUndefined(a[0]) || ev::isNull(a[0])) {
                    p.set(ev::undefined());
                } else {
                    p.set(a[0]);
                }
                return ev::undefined();
            });
    };

    registerCallbackAccessor("onpulse", &SteamCtxState::onPulse);
    registerCallbackAccessor("onfriends", &SteamCtxState::onFriends);
    registerCallbackAccessor("onoverlay", &SteamCtxState::onOverlay);
    registerCallbackAccessor("onjoinrequest", &SteamCtxState::onJoinRequest);
    registerCallbackAccessor("onlobbyentered", &SteamCtxState::onLobbyEntered);
    registerCallbackAccessor("onlobbyupdated", &SteamCtxState::onLobbyUpdated);
    registerCallbackAccessor("onlobbyleft", &SteamCtxState::onLobbyLeft);
    registerCallbackAccessor("onlobbyinvite", &SteamCtxState::onLobbyInvite);
    registerCallbackAccessor("onlobbyjoinrequest", &SteamCtxState::onLobbyJoinRequest);
    registerCallbackAccessor("onvoicecaptured", &SteamCtxState::onVoiceCaptured);

    // --- Friends & Presence API ---
    st.def("getFriends", 0, [](Value, std::span<const Value>) -> Value {
        auto* s = getSteamState();
        if (!s) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        return hostArrayOf(s->friends.size(), [s](size_t i) -> Value {
            return friendToObject(s->friends[i]);
        });
    });

    st.def("setRichPresence", 2, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (!s || !s->service || a.size() < 2) return ev::fromBool(false);
        std::string key = ev::toUtf8(a[0]);
        std::string val = ev::toUtf8(a[1]);
        s->service->setRichPresence(key, val);
        return ev::fromBool(true);
    });

    st.def("clearRichPresence", 0, [](Value, std::span<const Value>) -> Value {
        auto* s = getSteamState();
        if (s && s->service) s->service->clearRichPresence();
        return ev::undefined();
    });

    st.def("activateOverlay", 1, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (!s || !s->service) return ev::undefined();
        std::string dialog = (!a.empty() && ev::isString(a[0])) ? ev::toUtf8(a[0]) : "";
        s->service->activateOverlay(dialog);
        return ev::undefined();
    });

    st.def("activateOverlayToUser", 2, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (!s || !s->service || a.size() < 2) return ev::undefined();
        std::string dialog = ev::toUtf8(a[0]);
        uint64_t id = parseSteamId(a[1]);
        if (id) s->service->activateOverlayToUser(dialog, id);
        return ev::undefined();
    });

    st.def("activateInviteDialog", 1, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (!s || !s->service || a.empty()) return ev::undefined();
        uint64_t id = parseSteamId(a[0]);
        if (id) s->service->activateInviteDialog(id);
        return ev::undefined();
    });

    st.def("getAvatar", 2, [](Value, std::span<const Value> a) -> Value {
        ev::Persistent p(ev::createPromise());
        auto* s = getSteamState();
        if (!s || !s->service || !s->subscriber || !s->service->available() || a.empty()) {
            ev::resolvePromise(p.get(), ev::null());
            return p.get();
        }
        uint64_t id = parseSteamId(a[0]);
        if (!id) {
            ev::resolvePromise(p.get(), ev::null());
            return p.get();
        }
        int size = 1; // medium
        if (a.size() > 1) {
            if (ev::isString(a[1])) {
                std::string sz = ev::toUtf8(a[1]);
                if (sz == "small") size = 0;
                else if (sz == "large") size = 2;
                else size = 1;
            } else if (ev::isNumber(a[1])) {
                size = satCast<int>(ev::toDouble(a[1]));
            }
        }
        uint32_t reqId = s->nextAvatarReq++;
        s->pendingAvatars.emplace(reqId, ev::Persistent(p));
        s->service->requestAvatar(s->subscriber->id(), reqId, id, size);
        return p.get();
    });

    // --- Lobby API ---
    st.def("createLobby", 2, [](Value, std::span<const Value> a) -> Value {
        ev::Persistent p(ev::createPromise());
        auto* s = getSteamState();
        if (!s || !s->service || !s->subscriber || !s->service->available()) {
            ev::resolvePromise(p.get(), ev::null());
            return p.get();
        }
        int type = 2;
        if (!a.empty() && ev::isString(a[0])) {
            type = lobbyTypeFromString(ev::toUtf8(a[0]).c_str());
        }
        int maxMembers = 8;
        if (a.size() > 1 && ev::isNumber(a[1])) {
            int m = satCast<int>(ev::toDouble(a[1]));
            if (m > 0) maxMembers = m;
        }
        uint32_t reqId = s->nextLobbyReq++;
        s->pendingLobby.emplace(reqId, ev::Persistent(p));
        s->service->createLobby(s->subscriber->id(), reqId, type, maxMembers);
        return p.get();
    });

    st.def("joinLobby", 1, [](Value, std::span<const Value> a) -> Value {
        ev::Persistent p(ev::createPromise());
        auto* s = getSteamState();
        uint64_t id = !a.empty() ? parseSteamId(a[0]) : 0;
        if (!s || !s->service || !s->subscriber || !s->service->available() || !id) {
            ObjectBuilder res;
            res.set("success", ev::fromBool(false));
            res.set("lobbyId", ev::fromUtf8("0"));
            res.set("response", ev::fromDouble(0));
            ev::resolvePromise(p.get(), res.get());
            return p.get();
        }
        uint32_t reqId = s->nextLobbyReq++;
        s->pendingLobby.emplace(reqId, ev::Persistent(p));
        s->service->joinLobby(s->subscriber->id(), reqId, id);
        return p.get();
    });

    st.def("leaveLobby", 1, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (!s || !s->service || a.empty()) return ev::undefined();
        uint64_t id = parseSteamId(a[0]);
        if (id) {
            s->service->leaveLobby(id);
            s->lobbies.erase(id);
        }
        return ev::undefined();
    });

    st.def("setLobbyData", 3, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (!s || !s->service || a.size() < 3) return ev::fromBool(false);
        uint64_t id = parseSteamId(a[0]);
        std::string key = ev::toUtf8(a[1]);
        std::string val = ev::toUtf8(a[2]);
        if (id && !key.empty()) s->service->setLobbyData(id, key, val);
        return ev::fromBool(id != 0);
    });

    st.def("setLobbyMemberData", 3, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (!s || !s->service || a.size() < 3) return ev::undefined();
        uint64_t id = parseSteamId(a[0]);
        std::string key = ev::toUtf8(a[1]);
        std::string val = ev::toUtf8(a[2]);
        if (id && !key.empty()) s->service->setLobbyMemberData(id, key, val);
        return ev::undefined();
    });

    st.def("setLobbyJoinable", 2, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (!s || !s->service || a.size() < 2) return ev::undefined();
        uint64_t id = parseSteamId(a[0]);
        if (id) s->service->setLobbyJoinable(id, ev::toBool(a[1]));
        return ev::undefined();
    });

    st.def("setLobbyType", 2, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (!s || !s->service || a.size() < 2) return ev::undefined();
        uint64_t id = parseSteamId(a[0]);
        std::string t = ev::toUtf8(a[1]);
        if (id) s->service->setLobbyType(id, lobbyTypeFromString(t.c_str()));
        return ev::undefined();
    });

    st.def("setLobbyMemberLimit", 2, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (!s || !s->service || a.size() < 2) return ev::undefined();
        uint64_t id = parseSteamId(a[0]);
        int limit = satCast<int>(ev::toDouble(a[1]));
        if (id && limit > 0) s->service->setLobbyMemberLimit(id, limit);
        return ev::undefined();
    });

    st.def("getLobbyMembers", 1, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (!s || a.empty()) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        uint64_t id = parseSteamId(a[0]);
        auto it = s->lobbies.find(id);
        if (it == s->lobbies.end()) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        const auto& members = it->second.members;
        return hostArrayOf(members.size(), [&members](size_t i) -> Value {
            ObjectBuilder m;
            m.set("steamId", ev::fromUtf8(std::to_string(members[i].steamId)));
            m.set("name", ev::fromUtf8(members[i].name));
            return m.get();
        });
    });

    st.def("getLobbyOwner", 1, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        uint64_t owner = 0;
        if (s && !a.empty()) {
            uint64_t id = parseSteamId(a[0]);
            auto it = s->lobbies.find(id);
            if (it != s->lobbies.end()) owner = it->second.owner;
        }
        return ev::fromUtf8(std::to_string(owner));
    });

    st.def("getLobbyData", 2, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (!s || a.size() < 2) return ev::fromUtf8("");
        uint64_t id = parseSteamId(a[0]);
        std::string key = ev::toUtf8(a[1]);
        std::string out;
        auto it = s->lobbies.find(id);
        if (it != s->lobbies.end()) {
            for (const auto& [k, v] : it->second.data) {
                if (k == key) { out = v; break; }
            }
        }
        return ev::fromUtf8(out);
    });

    st.def("requestLobbyList", 1, [](Value, std::span<const Value> a) -> Value {
        ev::Persistent p(ev::createPromise());
        auto* s = getSteamState();
        if (!s || !s->service || !s->subscriber || !s->service->available()) {
            ev::Persistent empty(hostArrayOf(0, [](size_t) { return ev::undefined(); }));
            ev::resolvePromise(p.get(), empty.get());
            return p.get();
        }

        std::vector<steam::LobbyListFilter> filters;
        if (!a.empty() && ev::isObject(a[0])) {
            // Every value below is rooted: the property reads can run
            // getters and the Object.keys call allocates.
            ev::Persistent opts(a[0]);
            // Object.keys(obj) as (key, value) pairs, the value converted by
            // `read` while the key array is still rooted.
            auto forEachEntry = [](const char* field, const ev::Persistent& from,
                                   const std::function<void(const std::string&, Value)>& read) {
                ev::Persistent obj(ev::getProperty(from.get(), field));
                if (!ev::isObject(obj.get())) return;
                ev::Persistent objCtor(ev::globalValue("Object").value);
                ev::Persistent keysFn(ev::getProperty(objCtor.get(), "keys"));
                Value arg = obj.get();
                auto kres = ev::call(keysFn.get(), objCtor.get(), std::span<const Value>(&arg, 1));
                if (kres.thrown || !hostIsArray(kres.value)) return;
                ev::Persistent keys(kres.value);
                uint32_t len = satCast<uint32_t>(ev::toDouble(ev::getProperty(keys.get(), "length")));
                for (uint32_t i = 0; i < len; ++i) {
                    std::string key = ev::toUtf8(ev::getElement(keys.get(), i));
                    read(key, ev::getProperty(obj.get(), key));
                }
            };
            forEachEntry("stringFilters", opts, [&filters](const std::string& key, Value v) {
                steam::LobbyListFilter f;
                f.kind = steam::LobbyListFilter::String;
                f.key = key; f.sval = ev::toUtf8(v); f.comparison = 0;
                filters.push_back(std::move(f));
            });
            forEachEntry("numberFilters", opts, [&filters](const std::string& key, Value v) {
                steam::LobbyListFilter f;
                f.kind = steam::LobbyListFilter::Numeric;
                f.key = key; f.ival = satCast<int32_t>(ev::toDouble(v)); f.comparison = 0;
                filters.push_back(std::move(f));
            });

            Value dist = ev::getProperty(opts.get(), "distance");
            if (ev::isString(dist)) {
                std::string d = ev::toUtf8(dist);
                steam::LobbyListFilter f;
                f.kind = steam::LobbyListFilter::Distance;
                f.ival = distanceFromString(d.c_str());
                filters.push_back(std::move(f));
            }

            Value mr = ev::getProperty(opts.get(), "maxResults");
            if (ev::isNumber(mr)) {
                int32_t n = satCast<int32_t>(ev::toDouble(mr));
                if (n > 0) {
                    steam::LobbyListFilter f;
                    f.kind = steam::LobbyListFilter::ResultCount;
                    f.ival = n;
                    filters.push_back(std::move(f));
                }
            }
        }

        uint32_t reqId = s->nextLobbyReq++;
        s->pendingLobby.emplace(reqId, ev::Persistent(p));
        s->service->requestLobbyList(s->subscriber->id(), reqId, std::move(filters));
        return p.get();
    });

    st.def("inviteUserToLobby", 2, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (!s || !s->service || a.size() < 2) return ev::fromBool(false);
        uint64_t lobby = parseSteamId(a[0]);
        uint64_t invitee = parseSteamId(a[1]);
        if (lobby && invitee) s->service->inviteUserToLobby(lobby, invitee);
        return ev::fromBool(lobby != 0 && invitee != 0);
    });

    // --- Voice API ---
    st.def("startVoiceRecording", 0, [](Value, std::span<const Value>) -> Value {
        auto* s = getSteamState();
        if (s && s->service) s->service->startVoiceRecording();
        return ev::undefined();
    });

    st.def("stopVoiceRecording", 0, [](Value, std::span<const Value>) -> Value {
        auto* s = getSteamState();
        if (s && s->service) s->service->stopVoiceRecording();
        return ev::undefined();
    });

    st.def("decodeVoice", 2, [](Value, std::span<const Value> a) -> Value {
        ev::Persistent p(ev::createPromise());
        auto settleEmpty = [&p]() {
            ObjectBuilder res;
            res.set("pcm", makeFloat32Array(nullptr, 0));
            res.set("sampleRate", ev::fromDouble(0));
            ev::resolvePromise(p.get(), res.get());
        };

        auto* s = getSteamState();
        if (!s || !s->service || !s->subscriber || a.empty()) {
            settleEmpty();
            return p.get();
        }

        const uint8_t* base = nullptr;
        size_t viewLen = 0;
        if (auto info = ev::typedArrayInfo(a[0])) {
            base = reinterpret_cast<const uint8_t*>(info.data);
            viewLen = info.byteLength;
        } else if (auto abInfo = ev::arrayBufferInfo(a[0])) {
            base = reinterpret_cast<const uint8_t*>(abInfo.data);
            viewLen = abInfo.byteLength;
        }

        if (!base || viewLen == 0) {
            settleEmpty();
            return p.get();
        }

        if (!s->service->available()) {
            settleEmpty();
            return p.get();
        }

        int rate = 0;
        if (a.size() > 1 && ev::isNumber(a[1])) {
            rate = satCast<int>(ev::toDouble(a[1]));
        }

        uint32_t reqId = s->nextVoiceReq++;
        s->pendingVoice.emplace(reqId, ev::Persistent(p));
        s->service->decodeVoice(s->subscriber->id(), reqId, base, viewLen, rate);
        return p.get();
    });

    // Achievements & stats: placeholders. ISteamUserStats isn't bound in
    // steam_flat, so these answer false / 0 without contacting Steam, and
    // docs/steam-api.js says so.
    st.def("getAchievement", 1, [](Value, std::span<const Value>) { return ev::fromBool(false); });
    st.def("setAchievement", 1, [](Value, std::span<const Value>) { return ev::fromBool(false); });
    st.def("clearAchievement", 1, [](Value, std::span<const Value>) { return ev::fromBool(false); });
    st.def("getStat", 1, [](Value, std::span<const Value>) { return ev::fromDouble(0.0); });
    st.def("setStat", 2, [](Value, std::span<const Value>) { return ev::fromBool(false); });
    st.def("storeStats", 0, [](Value, std::span<const Value>) { return ev::fromBool(false); });
    st.def("activateOverlayToWebPage", 1, [](Value, std::span<const Value> a) -> Value {
        auto* s = getSteamState();
        if (s && s->service && !a.empty()) {
            s->service->activateOverlay(ev::toUtf8(a[0]));
        }
        return ev::undefined();
    });

    return st.get();
}

} // namespace bro::bronze_host
