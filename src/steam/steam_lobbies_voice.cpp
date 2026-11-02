// Steam lobbies, voice capture/decode, and callback dispatch for SteamService
// (split from steam_service.cpp).

#include "steam/steam_service.h"
#include "steam/steam_flat.h"
#include "util/log.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bro::steam {

namespace {

// Steam callback ids (k_iSteamFriendsCallbacks == 300). Stable across SDKs.
constexpr int kCbPersonaStateChange          = 300 + 4;  // 304
constexpr int kCbGameOverlayActivated        = 300 + 31; // 331
constexpr int kCbGameRichPresenceJoinRequest = 300 + 37; // 337

// Matchmaking callback ids (k_iSteamMatchmakingCallbacks == 500) + the async
// call-result completion id (k_iSteamUtilsCallbacks + 3 == 703).
constexpr int kCbLobbyInvite        = 500 + 3;  // 503
constexpr int kCbLobbyEnter         = 500 + 4;  // 504 (also JoinLobby call result)
constexpr int kCbLobbyDataUpdate    = 500 + 5;  // 505
constexpr int kCbLobbyChatUpdate    = 500 + 6;  // 506
constexpr int kCbLobbyMatchList     = 500 + 10; // 510 (RequestLobbyList call result)
constexpr int kCbLobbyCreated       = 500 + 13; // 513 (CreateLobby call result)
constexpr int kCbGameLobbyJoinRequested = 500 + 33; // 533
constexpr int kCbSteamAPICallCompleted = 700 + 3; // 703

// EChatMemberStateChange bits that mean "this member is gone".
constexpr uint32_t kChatMemberLeftMask = 0x0002 /*Left*/ | 0x0004 /*Disconnected*/ |
                                         0x0008 /*Kicked*/ | 0x0010 /*Banned*/;
constexpr int kEResultOK = 1;
constexpr int kEVoiceResultOK = 0;

#pragma pack(push, 8)
struct PersonaStateChange_t {
    uint64_t m_ulSteamID;
    int      m_nChangeFlags;
};
struct GameOverlayActivated_t {
    uint8_t  m_bActive;
    uint8_t  m_bUserInitiated;
    uint32_t m_nAppID;
    uint32_t m_dwOverlayPID;
};
struct GameRichPresenceJoinRequested_t {
    uint64_t m_steamIDFriend;
    char     m_rgchConnect[256];
};
struct SteamAPICallCompleted_t {
    uint64_t m_hAsyncCall;
    int      m_iCallback;
    uint32_t m_cubParam;
};
struct LobbyCreated_t {
    int      m_eResult;        // EResult (k_EResultOK == 1)
    uint64_t m_ulSteamIDLobby;
};
struct LobbyEnter_t {
    uint64_t m_ulSteamIDLobby;
    uint32_t m_rgfChatPermissions;     // legacy, unused
    uint8_t  m_bLocked;
    uint32_t m_EChatRoomEnterResponse; // k_EChatRoomEnterResponseSuccess == 1
};
struct LobbyDataUpdate_t {
    uint64_t m_ulSteamIDLobby;
    uint64_t m_ulSteamIDMember; // == lobby id when lobby data changed
    uint8_t  m_bSuccess;
};
struct LobbyChatUpdate_t {
    uint64_t m_ulSteamIDLobby;
    uint64_t m_ulSteamIDUserChanged;
    uint64_t m_ulSteamIDMakingChange;
    uint32_t m_rgfChatMemberStateChange; // EChatMemberStateChange bitfield
};
struct LobbyMatchList_t {
    uint32_t m_nLobbiesMatching;
};
struct LobbyInvite_t {
    uint64_t m_ulSteamIDUser;  // friend who invited us
    uint64_t m_ulSteamIDLobby;
    uint64_t m_ulGameID;
};
struct GameLobbyJoinRequested_t {
    uint64_t m_steamIDLobby;
    uint64_t m_steamIDFriend;
};
#pragma pack(pop)

}  // namespace

void SteamService::createLobby(uint32_t subscriberId, uint32_t reqId, int lobbyType, int maxMembers) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::CreateLobby;
    c->subscriberId = subscriberId;
    c->reqId = reqId;
    c->i32 = lobbyType;
    c->i32b = maxMembers;
    postCommand(c);
}

void SteamService::joinLobby(uint32_t subscriberId, uint32_t reqId, uint64_t lobbyId) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::JoinLobby;
    c->subscriberId = subscriberId;
    c->reqId = reqId;
    c->u64 = lobbyId;
    postCommand(c);
}

void SteamService::leaveLobby(uint64_t lobbyId) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::LeaveLobby;
    c->u64 = lobbyId;
    postCommand(c);
}

void SteamService::setLobbyData(uint64_t lobbyId, const std::string& key, const std::string& value) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::SetLobbyData;
    c->u64 = lobbyId;
    c->strA = key;
    c->strB = value;
    postCommand(c);
}

void SteamService::setLobbyMemberData(uint64_t lobbyId, const std::string& key, const std::string& value) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::SetLobbyMemberData;
    c->u64 = lobbyId;
    c->strA = key;
    c->strB = value;
    postCommand(c);
}

void SteamService::setLobbyJoinable(uint64_t lobbyId, bool joinable) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::SetLobbyJoinable;
    c->u64 = lobbyId;
    c->i32 = joinable ? 1 : 0;
    postCommand(c);
}

void SteamService::setLobbyType(uint64_t lobbyId, int lobbyType) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::SetLobbyType;
    c->u64 = lobbyId;
    c->i32 = lobbyType;
    postCommand(c);
}

void SteamService::setLobbyMemberLimit(uint64_t lobbyId, int maxMembers) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::SetLobbyMemberLimit;
    c->u64 = lobbyId;
    c->i32 = maxMembers;
    postCommand(c);
}

void SteamService::requestLobbyList(uint32_t subscriberId, uint32_t reqId,
                                    std::vector<LobbyListFilter> filters) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::RequestLobbyList;
    c->subscriberId = subscriberId;
    c->reqId = reqId;
    c->filters = std::move(filters);
    postCommand(c);
}

void SteamService::inviteUserToLobby(uint64_t lobbyId, uint64_t inviteeSteamId) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::InviteToLobby;
    c->u64 = lobbyId;
    c->u64b = inviteeSteamId;
    postCommand(c);
}

void SteamService::startVoiceRecording() {
    auto* c = new SteamCommand();
    c->type = SteamCommand::StartVoice;
    postCommand(c);
}

void SteamService::stopVoiceRecording() {
    auto* c = new SteamCommand();
    c->type = SteamCommand::StopVoice;
    postCommand(c);
}

void SteamService::decodeVoice(uint32_t subscriberId, uint32_t reqId,
                               const uint8_t* compressed, size_t len, int desiredSampleRate) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::DecodeVoice;
    c->subscriberId = subscriberId;
    c->reqId = reqId;
    c->i32 = desiredSampleRate;
    c->bytes.assign(compressed, compressed + len);
    postCommand(c);
}

LobbyState SteamService::snapshotLobby(uint64_t lobbyId, bool includeMembers) {
    LobbyState st;
    st.lobbyId = lobbyId;
    if (!iMatchmaking_) return st;

    if (api_.Matchmaking_GetLobbyOwner)
        st.owner = api_.Matchmaking_GetLobbyOwner(iMatchmaking_, lobbyId);
    if (api_.Matchmaking_GetLobbyMemberLimit)
        st.memberLimit = api_.Matchmaking_GetLobbyMemberLimit(iMatchmaking_, lobbyId);
    if (api_.Matchmaking_GetNumLobbyMembers)
        st.memberCount = api_.Matchmaking_GetNumLobbyMembers(iMatchmaking_, lobbyId);

    if (includeMembers && st.memberCount > 0) {
        st.members.reserve(static_cast<size_t>(st.memberCount));
        for (int i = 0; i < st.memberCount; ++i) {
            LobbyMember m;
            if (api_.Matchmaking_GetLobbyMemberByIndex)
                m.steamId = api_.Matchmaking_GetLobbyMemberByIndex(iMatchmaking_, lobbyId, i);
            if (m.steamId && iFriends_ && api_.Friends_GetFriendPersonaName) {
                const char* nm = api_.Friends_GetFriendPersonaName(iFriends_, m.steamId);
                m.name = nm ? nm : "";
            }
            st.members.push_back(std::move(m));
        }
    }

    int dc = api_.Matchmaking_GetLobbyDataCount
                 ? api_.Matchmaking_GetLobbyDataCount(iMatchmaking_, lobbyId) : 0;
    for (int i = 0; i < dc; ++i) {
        char key[256] = {0};
        char val[8192] = {0}; // k_cubChatMetadataMax
        if (api_.Matchmaking_GetLobbyDataByIndex &&
            api_.Matchmaking_GetLobbyDataByIndex(iMatchmaking_, lobbyId, i,
                                                 key, sizeof(key), val, sizeof(val)))
            st.data.emplace_back(key, val);
    }
    return st;
}

void SteamService::buildAndEmitLobby(uint64_t lobbyId) {
    if (!iMatchmaking_) return;
    LobbyState st = snapshotLobby(lobbyId, /*includeMembers=*/true);
    for (auto& [sid, sub] : subscribers_) {
        auto* ev = new SteamEvent();
        ev->type = SteamEvent::LobbyUpdated;
        ev->lobby = new LobbyState(st);
        postEventTo(sid, ev);
    }
}

void SteamService::emitPairToAll(SteamEvent::Type type, uint64_t u64, uint64_t u64b) {
    for (auto& [sid, sub] : subscribers_) {
        auto* ev = new SteamEvent();
        ev->type = type;
        ev->u64 = u64;
        ev->u64b = u64b;
        postEventTo(sid, ev);
    }
}

void SteamService::pumpVoiceCapture() {
    if (!iUser_ || !api_.User_GetAvailableVoice || !api_.User_GetVoice) return;
    uint32_t cbCompressed = 0;
    int avail = api_.User_GetAvailableVoice(iUser_, &cbCompressed, nullptr, 0);
    if (avail != kEVoiceResultOK || cbCompressed == 0) return;

    if (voiceBuf_.size() < cbCompressed) voiceBuf_.resize(cbCompressed);
    uint32_t written = 0;
    int vr = api_.User_GetVoice(iUser_, /*bWantCompressed=*/true, voiceBuf_.data(),
                                static_cast<uint32_t>(voiceBuf_.size()), &written,
                                /*deprecated tail:*/ false, nullptr, 0, nullptr, 0);
    if (vr != kEVoiceResultOK || written == 0) return;

    for (auto& [sid, sub] : subscribers_) {
        auto* ev = new SteamEvent();
        ev->type = SteamEvent::VoiceCaptured;
        ev->blob = new std::vector<uint8_t>(voiceBuf_.begin(), voiceBuf_.begin() + written);
        postEventTo(sid, ev);
    }
}

void SteamService::emitLobbyEntered(uint32_t requesterSub, uint32_t reqId,
                                    uint64_t lobbyId, int response, bool broadcast) {
    if (!broadcast) {
        if (requesterSub == 0) return;
        auto* ev = new SteamEvent();
        ev->type = SteamEvent::LobbyEntered;
        ev->u64 = lobbyId;
        ev->i32 = response;
        ev->reqId = reqId;
        ev->u64b = 1; // promise-only: settle, but don't refire onlobbyentered
        postEventTo(requesterSub, ev);
        return;
    }
    for (auto& [sid, sub] : subscribers_) {
        auto* ev = new SteamEvent();
        ev->type = SteamEvent::LobbyEntered;
        ev->u64 = lobbyId;
        ev->i32 = response;
        ev->reqId = (sid == requesterSub) ? reqId : 0;
        postEventTo(sid, ev);
    }
}

void SteamService::enterLobby(uint32_t requesterSub, uint32_t reqId,
                              uint64_t lobbyId, int response) {
    if (!lobbyId) {
        emitLobbyEntered(requesterSub, reqId, lobbyId, response, /*broadcast=*/false);
        return;
    }
    bool first = joinedLobbies_.insert(lobbyId).second;
    if (first) {
        buildAndEmitLobby(lobbyId);
        emitLobbyEntered(requesterSub, reqId, lobbyId, response, /*broadcast=*/true);
    } else {
        emitLobbyEntered(requesterSub, reqId, lobbyId, response, /*broadcast=*/false);
    }
}

void SteamService::dispatchCallback(const CallbackMsg_t& msg) {
    switch (msg.m_iCallback) {
        case kCbPersonaStateChange:
            buildAndEmitFriends();
            if (msg.m_pubParam && msg.m_cubParam >= (int)sizeof(PersonaStateChange_t)) {
                auto* p = reinterpret_cast<const PersonaStateChange_t*>(msg.m_pubParam);
                retryPendingAvatars(p->m_ulSteamID);
            }
            break;
        case kCbGameOverlayActivated:
            if (msg.m_pubParam && msg.m_cubParam >= (int)sizeof(GameOverlayActivated_t)) {
                auto* p = reinterpret_cast<const GameOverlayActivated_t*>(msg.m_pubParam);
                emitToAll(SteamEvent::OverlayActivated, p->m_bActive ? 1 : 0);
            }
            break;
        case kCbGameRichPresenceJoinRequest:
            if (msg.m_pubParam && msg.m_cubParam >= (int)sizeof(GameRichPresenceJoinRequested_t)) {
                auto* p = reinterpret_cast<const GameRichPresenceJoinRequested_t*>(msg.m_pubParam);
                emitToAll(SteamEvent::JoinRequested, p->m_steamIDFriend, p->m_rgchConnect);
            }
            break;

        case kCbSteamAPICallCompleted: {
            if (!msg.m_pubParam || !api_.ManualDispatch_GetAPICallResult) break;
            auto* cc = reinterpret_cast<const SteamAPICallCompleted_t*>(msg.m_pubParam);
            auto it = pendingCalls_.find(cc->m_hAsyncCall);
            if (it == pendingCalls_.end()) break;
            PendingCall pc = it->second;
            pendingCalls_.erase(it);

            std::vector<uint8_t> buf(cc->m_cubParam ? cc->m_cubParam : 1);
            bool failed = false;
            bool ok = api_.ManualDispatch_GetAPICallResult(
                          pipe_, cc->m_hAsyncCall, buf.data(),
                          static_cast<int>(cc->m_cubParam), cc->m_iCallback, &failed) && !failed;

            if (cc->m_iCallback == kCbLobbyCreated) {
                uint64_t lobby = 0;
                bool success = false;
                if (ok && cc->m_cubParam >= sizeof(LobbyCreated_t)) {
                    auto* r = reinterpret_cast<const LobbyCreated_t*>(buf.data());
                    success = (r->m_eResult == kEResultOK);
                    lobby = r->m_ulSteamIDLobby;
                }
                auto* ev = new SteamEvent();
                ev->type = SteamEvent::LobbyCreated;
                ev->reqId = pc.reqId;
                ev->u64 = lobby;
                ev->i32 = success ? 1 : 0;
                postEventTo(pc.subscriberId, ev);
                if (success && lobby) enterLobby(0, 0, lobby, 1 /*success*/);
            } else if (cc->m_iCallback == kCbLobbyEnter) {
                uint64_t lobby = 0;
                int response = 0;
                if (ok && cc->m_cubParam >= sizeof(LobbyEnter_t)) {
                    auto* r = reinterpret_cast<const LobbyEnter_t*>(buf.data());
                    lobby = r->m_ulSteamIDLobby;
                    response = static_cast<int>(r->m_EChatRoomEnterResponse);
                }
                enterLobby(pc.subscriberId, pc.reqId, lobby, response);
            } else if (cc->m_iCallback == kCbLobbyMatchList) {
                uint32_t count = 0;
                if (ok && cc->m_cubParam >= sizeof(LobbyMatchList_t))
                    count = reinterpret_cast<const LobbyMatchList_t*>(buf.data())->m_nLobbiesMatching;
                auto* list = new std::vector<LobbyState>();
                list->reserve(count);
                for (uint32_t i = 0; i < count && api_.Matchmaking_GetLobbyByIndex; ++i) {
                    uint64_t id = api_.Matchmaking_GetLobbyByIndex(iMatchmaking_, static_cast<int>(i));
                    if (id) list->push_back(snapshotLobby(id, /*includeMembers=*/false));
                }
                auto* ev = new SteamEvent();
                ev->type = SteamEvent::LobbyList;
                ev->reqId = pc.reqId;
                ev->lobbyList = list;
                postEventTo(pc.subscriberId, ev);
            }
            break;
        }
        case kCbLobbyEnter:
            if (msg.m_pubParam && msg.m_cubParam >= (int)sizeof(LobbyEnter_t)) {
                auto* p = reinterpret_cast<const LobbyEnter_t*>(msg.m_pubParam);
                enterLobby(0, 0, p->m_ulSteamIDLobby,
                           static_cast<int>(p->m_EChatRoomEnterResponse));
            }
            break;
        case kCbLobbyDataUpdate:
            if (msg.m_pubParam && msg.m_cubParam >= (int)sizeof(LobbyDataUpdate_t)) {
                auto* p = reinterpret_cast<const LobbyDataUpdate_t*>(msg.m_pubParam);
                buildAndEmitLobby(p->m_ulSteamIDLobby);
            }
            break;
        case kCbLobbyChatUpdate:
            if (msg.m_pubParam && msg.m_cubParam >= (int)sizeof(LobbyChatUpdate_t)) {
                auto* p = reinterpret_cast<const LobbyChatUpdate_t*>(msg.m_pubParam);
                uint64_t me = localSteamId_.load(std::memory_order_relaxed);
                if (p->m_ulSteamIDUserChanged == me &&
                    (p->m_rgfChatMemberStateChange & kChatMemberLeftMask)) {
                    joinedLobbies_.erase(p->m_ulSteamIDLobby);
                    emitToAll(SteamEvent::LobbyLeft, p->m_ulSteamIDLobby);
                } else {
                    buildAndEmitLobby(p->m_ulSteamIDLobby);
                }
            }
            break;
        case kCbLobbyInvite:
            if (msg.m_pubParam && msg.m_cubParam >= (int)sizeof(LobbyInvite_t)) {
                auto* p = reinterpret_cast<const LobbyInvite_t*>(msg.m_pubParam);
                emitPairToAll(SteamEvent::LobbyInvite, p->m_ulSteamIDUser, p->m_ulSteamIDLobby);
            }
            break;
        case kCbGameLobbyJoinRequested:
            if (msg.m_pubParam && msg.m_cubParam >= (int)sizeof(GameLobbyJoinRequested_t)) {
                auto* p = reinterpret_cast<const GameLobbyJoinRequested_t*>(msg.m_pubParam);
                emitPairToAll(SteamEvent::LobbyJoinRequested, p->m_steamIDLobby, p->m_steamIDFriend);
            }
            break;
        default:
            break;
    }
}

}  // namespace bro::steam
