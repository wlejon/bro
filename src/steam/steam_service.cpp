#include "steam/steam_service.h"
#include "steam/steam_flat.h"
#include "util/log.h"

#include <chrono>
#include <cstring>

namespace bro::steam {

SteamService* SteamService::s_instance = nullptr;

// Interface accessor version strings track SDK releases; the flat *method*
// wrappers do not. We probe a list until FindOrCreateUserInterface resolves.
//
// IMPORTANT ordering hazard: the flat wrappers in steam_api64.dll are compiled
// against ONE SDK version and bake in that interface's vtable layout. Asking for
// a version NEWER than the DLL can hand back a non-null but layout-wrong object —
// early methods may appear to work while a later one jumps off the vtable and
// crashes. (Observed: requesting "SteamFriends018" against the SDK 1.57 DLL made
// GetFriendCount segfault while GetPersonaName still returned the right name.)
// So the version MATCHING the shipped redistributable must come first; these
// lists lead with SDK 1.57's versions (our reference redistributable).
namespace {
const char* kUserVersions[]    = { "SteamUser023", "SteamUser022", "SteamUser021", "SteamUser020" };
const char* kFriendsVersions[] = { "SteamFriends017", "SteamFriends018", "SteamFriends015" };
const char* kUtilsVersions[]   = { "SteamUtils010", "SteamUtils009" };
// ISteamMatchmaking has been version 009 since ~SDK 1.36 and is 009 in our
// reference redistributable (SDK 1.57). Lead with the matching version (see the
// vtable hazard note above); 008 is the only realistic older fallback.
const char* kMatchmakingVersions[] = { "SteamMatchMaking009", "SteamMatchMaking008" };

// k_EFriendFlagImmediate — "regular" friends (the people on your friends list).
constexpr int kFriendFlagImmediate = 0x04;

// Rebuild the friends snapshot at ~2 Hz (every 50 * 10 ms pump iterations).
constexpr uint64_t kFriendsRebuildEvery = 50;

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

// Param structs for the callbacks above — layouts match Steam's headers exactly.
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
// --- matchmaking (M3). Layouts match Steam's isteammatchmaking.h exactly. ---
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

void* findInterface(const SteamFlatApi& api, HSteamUser user,
                    const char* const* versions, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        void* iface = api.FindOrCreateUserInterface(user, versions[i]);
        if (iface) return iface;
    }
    return nullptr;
}
} // namespace

// =============================================================================
// SteamSubscriber
// =============================================================================

void SteamSubscriber::poll() {
    while (SteamEvent* ev = events_.pop()) {
        switch (ev->type) {
            case SteamEvent::Pulse:
                if (onPulse) onPulse(ev->u64);
                break;
            case SteamEvent::FriendsUpdated:
                if (ev->friends) {
                    if (onFriends) onFriends(*ev->friends);
                    delete ev->friends; // ownership transferred to us via the event
                }
                break;
            case SteamEvent::OverlayActivated:
                if (onOverlay) onOverlay(ev->u64 != 0);
                break;
            case SteamEvent::JoinRequested:
                if (onJoinRequest) onJoinRequest(ev->u64, ev->str);
                break;
            case SteamEvent::AvatarData_:
                if (ev->avatar) {
                    if (onAvatar)
                        onAvatar(ev->avatar->reqId, ev->avatar->w, ev->avatar->h,
                                 ev->avatar->rgba.data(), ev->avatar->rgba.size());
                    delete ev->avatar; // ownership transferred via the event
                }
                break;
            case SteamEvent::LobbyCreated:
                if (onLobbyCreated) onLobbyCreated(ev->reqId, ev->u64, ev->i32 != 0);
                break;
            case SteamEvent::LobbyEntered:
                if (onLobbyEntered) onLobbyEntered(ev->reqId, ev->u64, ev->i32, ev->u64b == 0);
                break;
            case SteamEvent::LobbyUpdated:
                if (ev->lobby) {
                    if (onLobbyUpdated) onLobbyUpdated(*ev->lobby);
                    delete ev->lobby; // ownership transferred via the event
                }
                break;
            case SteamEvent::LobbyLeft:
                if (onLobbyLeft) onLobbyLeft(ev->u64);
                break;
            case SteamEvent::LobbyList:
                if (ev->lobbyList) {
                    if (onLobbyList) onLobbyList(ev->reqId, *ev->lobbyList);
                    delete ev->lobbyList; // ownership transferred via the event
                }
                break;
            case SteamEvent::LobbyInvite:
                if (onLobbyInvite) onLobbyInvite(ev->u64, ev->u64b);
                break;
            case SteamEvent::LobbyJoinRequested:
                if (onLobbyJoinRequested) onLobbyJoinRequested(ev->u64, ev->u64b);
                break;
        }
        delete ev;
    }
}

// =============================================================================
// SteamService
// =============================================================================

SteamService::SteamService() {
    if (s_instance) {
        LOG_ERROR("[steam] SteamService: second instance created — there can be only one.");
    }
    s_instance = this;
    status_.store(Status::Initializing, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&SteamService::threadMain, this);
}

SteamService::~SteamService() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();

    // Free any command nodes that arrived after the thread drained/exited. Do
    // NOT delete subscriberPtr here — the thread's shutdown already freed the
    // subscribers it owned (double-free risk otherwise).
    SteamCommand* head = cmdHead_.exchange(nullptr, std::memory_order_acquire);
    while (head) {
        SteamCommand* next = head->next;
        delete head;
        head = next;
    }

    if (s_instance == this) s_instance = nullptr;
}

const char* SteamService::reason() const {
    switch (status_.load(std::memory_order_acquire)) {
        case Status::Initializing:    return "initializing";
        case Status::Available:       return "ok";
        case Status::LibraryNotFound: return "Steam library not found — place the redistributable "
                                             "(steam_api64.dll / libsteam_api.*) next to the executable "
                                             "with the Steam client running";
        case Status::InitFailed:      return "SteamAPI init failed — is the Steam client running and "
                                             "logged in? (steam_appid.txt next to the executable for dev)";
    }
    return "unknown";
}

SteamSubscriber* SteamService::createSubscriber() {
    uint32_t id = nextSubscriberId_.fetch_add(1, std::memory_order_relaxed);
    auto* sub = new SteamSubscriber(this, id);
    auto* c = new SteamCommand();
    c->type = SteamCommand::Register;
    c->subscriberId = id;
    c->subscriberPtr = sub;
    postCommand(c);
    return sub;
}

void SteamService::destroySubscriber(SteamSubscriber* sub) {
    if (!sub) return;
    auto* c = new SteamCommand();
    c->type = SteamCommand::Unregister;
    c->subscriberId = sub->id();
    c->subscriberPtr = sub;
    postCommand(c);
}

void SteamService::setRichPresence(const std::string& key, const std::string& value) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::SetRichPresence;
    c->strA = key;
    c->strB = value;
    postCommand(c);
}

void SteamService::clearRichPresence() {
    auto* c = new SteamCommand();
    c->type = SteamCommand::ClearRichPresence;
    postCommand(c);
}

void SteamService::activateOverlay(const std::string& dialog) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::ActivateOverlay;
    c->strA = dialog;
    postCommand(c);
}

void SteamService::activateOverlayToUser(const std::string& dialog, uint64_t steamId) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::ActivateOverlayToUser;
    c->strA = dialog;
    c->u64 = steamId;
    postCommand(c);
}

void SteamService::requestAvatar(uint32_t subscriberId, uint32_t reqId, uint64_t steamId, int size) {
    auto* c = new SteamCommand();
    c->type = SteamCommand::RequestAvatar;
    c->subscriberId = subscriberId;
    c->reqId = reqId;
    c->u64 = steamId;
    c->i32 = size;
    postCommand(c);
}

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

// ---------------------------------------------------------------------------
// Lock-free MPSC push — CAS the node onto the stack head.
// ---------------------------------------------------------------------------
void SteamService::postCommand(SteamCommand* cmd) {
    SteamCommand* old = cmdHead_.load(std::memory_order_relaxed);
    do {
        cmd->next = old;
    } while (!cmdHead_.compare_exchange_weak(
        old, cmd,
        std::memory_order_release,
        std::memory_order_relaxed));
}

void SteamService::postEventTo(uint32_t subscriberId, SteamEvent* ev) {
    // Service-thread only. subscribers_ is owned by this thread.
    auto it = subscribers_.find(subscriberId);
    if (it == subscribers_.end()) { delete ev; return; }
    if (!it->second->events_.push(ev)) {
        LOG_WARN("[steam] Event queue full for subscriber %u — dropping event", subscriberId);
        delete ev;
    }
}

void SteamService::handleCommand(SteamCommand& cmd) {
    switch (cmd.type) {
        case SteamCommand::Register:
            subscribers_[cmd.subscriberId] = cmd.subscriberPtr;
            // Hand the newcomer the current snapshot so it isn't blank until the
            // next change (buildAndEmitFriends only emits on a diff).
            emitFriendsTo(cmd.subscriberId);
            break;
        case SteamCommand::Unregister:
            subscribers_.erase(cmd.subscriberId);
            delete cmd.subscriberPtr;
            break;
        case SteamCommand::SetRichPresence:
            if (iFriends_ && api_.Friends_SetRichPresence)
                api_.Friends_SetRichPresence(iFriends_, cmd.strA.c_str(), cmd.strB.c_str());
            break;
        case SteamCommand::ClearRichPresence:
            if (iFriends_ && api_.Friends_ClearRichPresence)
                api_.Friends_ClearRichPresence(iFriends_);
            break;
        case SteamCommand::ActivateOverlay:
            if (iFriends_ && api_.Friends_ActivateGameOverlay)
                api_.Friends_ActivateGameOverlay(iFriends_, cmd.strA.c_str());
            break;
        case SteamCommand::ActivateOverlayToUser:
            if (iFriends_ && api_.Friends_ActivateGameOverlayToUser)
                api_.Friends_ActivateGameOverlayToUser(iFriends_, cmd.strA.c_str(), cmd.u64);
            break;
        case SteamCommand::RequestAvatar: {
            auto* ad = new AvatarData();
            ad->reqId = cmd.reqId;

            int handle = 0;
            if (iFriends_) {
                if (cmd.i32 <= 0 && api_.Friends_GetSmallFriendAvatar)
                    handle = api_.Friends_GetSmallFriendAvatar(iFriends_, cmd.u64);
                else if (cmd.i32 == 1 && api_.Friends_GetMediumFriendAvatar)
                    handle = api_.Friends_GetMediumFriendAvatar(iFriends_, cmd.u64);
                else if (api_.Friends_GetLargeFriendAvatar)
                    handle = api_.Friends_GetLargeFriendAvatar(iFriends_, cmd.u64);
            }
            // handle 0 = none, -1 = still loading; either way return empty and let
            // the app re-request after the avatar-loaded PersonaStateChange.
            if (handle > 0 && iUtils_ && api_.Utils_GetImageSize && api_.Utils_GetImageRGBA) {
                uint32_t w = 0, h = 0;
                if (api_.Utils_GetImageSize(iUtils_, handle, &w, &h) && w > 0 && h > 0) {
                    ad->rgba.resize(static_cast<size_t>(w) * h * 4);
                    if (api_.Utils_GetImageRGBA(iUtils_, handle, ad->rgba.data(),
                                                static_cast<int>(ad->rgba.size()))) {
                        ad->w = static_cast<int>(w);
                        ad->h = static_cast<int>(h);
                    } else {
                        ad->rgba.clear();
                    }
                }
            }

            auto* ev = new SteamEvent();
            ev->type = SteamEvent::AvatarData_;
            ev->avatar = ad;
            postEventTo(cmd.subscriberId, ev);
            break;
        }
        case SteamCommand::CreateLobby: {
            uint64_t call = 0;
            if (iMatchmaking_ && api_.Matchmaking_CreateLobby)
                call = api_.Matchmaking_CreateLobby(iMatchmaking_, cmd.i32, cmd.i32b);
            if (call) {
                pendingCalls_[call] = { cmd.subscriberId, cmd.reqId, kCbLobbyCreated };
            } else {
                // Couldn't even issue the call — settle the promise as failure.
                auto* ev = new SteamEvent();
                ev->type = SteamEvent::LobbyCreated;
                ev->reqId = cmd.reqId;
                postEventTo(cmd.subscriberId, ev);
            }
            break;
        }
        case SteamCommand::JoinLobby: {
            uint64_t call = 0;
            if (iMatchmaking_ && api_.Matchmaking_JoinLobby)
                call = api_.Matchmaking_JoinLobby(iMatchmaking_, cmd.u64);
            if (call) {
                pendingCalls_[call] = { cmd.subscriberId, cmd.reqId, kCbLobbyEnter };
            } else {
                auto* ev = new SteamEvent();
                ev->type = SteamEvent::LobbyEntered;
                ev->reqId = cmd.reqId;
                ev->u64 = cmd.u64;
                ev->i32 = 0; // no response
                postEventTo(cmd.subscriberId, ev);
            }
            break;
        }
        case SteamCommand::LeaveLobby:
            if (iMatchmaking_ && api_.Matchmaking_LeaveLobby)
                api_.Matchmaking_LeaveLobby(iMatchmaking_, cmd.u64);
            joinedLobbies_.erase(cmd.u64);
            // Steam doesn't deliver a self-leave callback, so notify here.
            emitToAll(SteamEvent::LobbyLeft, cmd.u64);
            break;
        case SteamCommand::SetLobbyData:
            if (iMatchmaking_ && api_.Matchmaking_SetLobbyData)
                api_.Matchmaking_SetLobbyData(iMatchmaking_, cmd.u64, cmd.strA.c_str(), cmd.strB.c_str());
            break;
        case SteamCommand::SetLobbyMemberData:
            if (iMatchmaking_ && api_.Matchmaking_SetLobbyMemberData)
                api_.Matchmaking_SetLobbyMemberData(iMatchmaking_, cmd.u64, cmd.strA.c_str(), cmd.strB.c_str());
            break;
        case SteamCommand::SetLobbyJoinable:
            if (iMatchmaking_ && api_.Matchmaking_SetLobbyJoinable)
                api_.Matchmaking_SetLobbyJoinable(iMatchmaking_, cmd.u64, cmd.i32 != 0);
            break;
        case SteamCommand::SetLobbyType:
            if (iMatchmaking_ && api_.Matchmaking_SetLobbyType)
                api_.Matchmaking_SetLobbyType(iMatchmaking_, cmd.u64, cmd.i32);
            break;
        case SteamCommand::SetLobbyMemberLimit:
            if (iMatchmaking_ && api_.Matchmaking_SetLobbyMemberLimit)
                api_.Matchmaking_SetLobbyMemberLimit(iMatchmaking_, cmd.u64, cmd.i32);
            break;
        case SteamCommand::RequestLobbyList: {
            uint64_t call = 0;
            if (iMatchmaking_ && api_.Matchmaking_RequestLobbyList) {
                // Filters are interface state — apply them right before the call.
                for (const auto& f : cmd.filters) {
                    switch (f.kind) {
                        case LobbyListFilter::String:
                            if (api_.Matchmaking_AddRequestLobbyListStringFilter)
                                api_.Matchmaking_AddRequestLobbyListStringFilter(
                                    iMatchmaking_, f.key.c_str(), f.sval.c_str(), f.comparison);
                            break;
                        case LobbyListFilter::Numeric:
                            if (api_.Matchmaking_AddRequestLobbyListNumericalFilter)
                                api_.Matchmaking_AddRequestLobbyListNumericalFilter(
                                    iMatchmaking_, f.key.c_str(), f.ival, f.comparison);
                            break;
                        case LobbyListFilter::ResultCount:
                            if (api_.Matchmaking_AddRequestLobbyListResultCountFilter)
                                api_.Matchmaking_AddRequestLobbyListResultCountFilter(iMatchmaking_, f.ival);
                            break;
                        case LobbyListFilter::Distance:
                            if (api_.Matchmaking_AddRequestLobbyListDistanceFilter)
                                api_.Matchmaking_AddRequestLobbyListDistanceFilter(iMatchmaking_, f.ival);
                            break;
                    }
                }
                call = api_.Matchmaking_RequestLobbyList(iMatchmaking_);
            }
            if (call) {
                pendingCalls_[call] = { cmd.subscriberId, cmd.reqId, kCbLobbyMatchList };
            } else {
                auto* ev = new SteamEvent();
                ev->type = SteamEvent::LobbyList;
                ev->reqId = cmd.reqId;
                ev->lobbyList = new std::vector<LobbyState>(); // empty result
                postEventTo(cmd.subscriberId, ev);
            }
            break;
        }
        case SteamCommand::InviteToLobby:
            if (iMatchmaking_ && api_.Matchmaking_InviteUserToLobby)
                api_.Matchmaking_InviteUserToLobby(iMatchmaking_, cmd.u64, cmd.u64b);
            break;
    }
}

// Build a fresh friends snapshot from the Steam API (service thread only). Emits
// a FriendsUpdated event to every subscriber only when the list actually changed,
// so a steady state produces no traffic.
void SteamService::buildAndEmitFriends() {
    if (!iFriends_ || !api_.Friends_GetFriendCount || !api_.Friends_GetFriendByIndex)
        return;

    std::vector<FriendInfo> list;
    int n = api_.Friends_GetFriendCount(iFriends_, kFriendFlagImmediate);
    if (n > 0) list.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        uint64_t fid = api_.Friends_GetFriendByIndex(iFriends_, i, kFriendFlagImmediate);
        FriendInfo fi;
        fi.steamId = fid;
        if (api_.Friends_GetFriendPersonaName) {
            const char* nm = api_.Friends_GetFriendPersonaName(iFriends_, fid);
            fi.name = nm ? nm : "";
        }
        if (api_.Friends_GetFriendPersonaState)
            fi.personaState = api_.Friends_GetFriendPersonaState(iFriends_, fid);
        if (api_.Friends_GetFriendRelationship)
            fi.relationship = api_.Friends_GetFriendRelationship(iFriends_, fid);
        list.push_back(std::move(fi));
    }

    if (list == lastFriends_) return; // no change → no event
    lastFriends_ = std::move(list);
    for (auto& [sid, sub] : subscribers_) emitFriendsTo(sid);
}

void SteamService::emitFriendsTo(uint32_t subscriberId) {
    auto* ev = new SteamEvent();
    ev->type = SteamEvent::FriendsUpdated;
    ev->friends = new std::vector<FriendInfo>(lastFriends_); // per-subscriber copy
    postEventTo(subscriberId, ev);
}

void SteamService::emitToAll(SteamEvent::Type type, uint64_t u64, const std::string& str) {
    for (auto& [sid, sub] : subscribers_) {
        auto* ev = new SteamEvent();
        ev->type = type;
        ev->u64 = u64;
        ev->str = str;
        postEventTo(sid, ev);
    }
}

// Snapshot a lobby's owner/limit/count/data — and, when includeMembers (only
// possible for lobbies we've joined), the member identities. Service thread only.
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

// Snapshot a joined lobby and ship it to every subscriber as a LobbyUpdated
// event (each gets its own copy). The JS thread keeps this as a cache and
// serves getLobbyMembers/Owner/Data from it. Service thread only.
void SteamService::buildAndEmitLobby(uint64_t lobbyId) {
    if (!iMatchmaking_) return;
    LobbyState st = snapshotLobby(lobbyId, /*includeMembers=*/true);
    for (auto& [sid, sub] : subscribers_) {
        auto* ev = new SteamEvent();
        ev->type = SteamEvent::LobbyUpdated;
        ev->lobby = new LobbyState(st); // per-subscriber copy
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

// Emit a LobbyEntered event. When broadcast, every subscriber gets the event
// (only the requester carries `reqId`, so only its promise resolves). When not
// broadcast, only the requester is notified — used when the physical enter was
// already announced and all that's left is to settle a late-arriving promise.
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

// Steam can deliver entry for one lobby twice (a CreateLobby/JoinLobby call
// result AND a spontaneous LobbyEnter_t). Funnel both through here so the app
// sees onlobbyentered exactly once: the first arrival snapshots + broadcasts;
// a later duplicate only settles a still-pending promise.
void SteamService::enterLobby(uint32_t requesterSub, uint32_t reqId,
                              uint64_t lobbyId, int response) {
    if (!lobbyId) {                       // failed enter — settle the promise only
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

// Translate a raw Steam callback (pulled via ManualDispatch) into our events.
// Service thread only. Unknown callbacks are ignored — they're already freed by
// the caller's FreeLastCallback.
void SteamService::dispatchCallback(const CallbackMsg_t& msg) {
    switch (msg.m_iCallback) {
        case kCbPersonaStateChange:
            // A friend's name/state/avatar changed — refresh the snapshot. The
            // diff in buildAndEmitFriends() suppresses no-op emits, so this is
            // cheap even though PersonaStateChange fires frequently.
            buildAndEmitFriends();
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
                // m_rgchConnect is NUL-terminated by Steam.
                emitToAll(SteamEvent::JoinRequested, p->m_steamIDFriend, p->m_rgchConnect);
            }
            break;

        // --- lobbies (M3) ---
        case kCbSteamAPICallCompleted: {
            // The result of an async SteamAPICall_t (CreateLobby / JoinLobby) is
            // ready. Pull it and route it back to the request that issued it.
            if (!msg.m_pubParam || !api_.ManualDispatch_GetAPICallResult) break;
            auto* cc = reinterpret_cast<const SteamAPICallCompleted_t*>(msg.m_pubParam);
            auto it = pendingCalls_.find(cc->m_hAsyncCall);
            if (it == pendingCalls_.end()) break; // not one of ours
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
                // The creator is auto-joined; publish entry here too (reqId 0 —
                // the create promise already carried the id). enterLobby() dedups
                // against the spontaneous LobbyEnter_t that may also arrive.
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
            // A spontaneous enter (e.g. accepting an invite) — no pending promise.
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
                    // I was kicked/banned/disconnected — treat as leaving.
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

// ---------------------------------------------------------------------------
// Drain the MPSC stack and reverse LIFO → FIFO for ordered processing.
// ---------------------------------------------------------------------------
static SteamCommand* drainAndReverse(std::atomic<SteamCommand*>& head) {
    SteamCommand* lifo = head.exchange(nullptr, std::memory_order_acquire);
    SteamCommand* fifo = nullptr;
    while (lifo) {
        SteamCommand* next = lifo->next;
        lifo->next = fifo;
        fifo = lifo;
        lifo = next;
    }
    return fifo;
}

// ---------------------------------------------------------------------------
// Service thread main — loads the Steam redistributable, owns SteamAPI init,
// the RunCallbacks pump, and (as the surface grows) every callback handler. ALL
// SteamAPI calls happen on this thread.
//
// The command loop runs whether or not Steam is available, so subscriber
// register/unregister is always handled correctly — when Steam is down the loop
// simply skips the pump.
// ---------------------------------------------------------------------------
void SteamService::threadMain() {
    bool steamUp = false;
    bool manualDispatch = false;
    HSteamPipe pipe = 0;

    if (!loadSteamFlat(api_)) {
        status_.store(Status::LibraryNotFound, std::memory_order_release);
        LOG_INFO("[steam] Steam redistributable not found — bro.steam.available = false");
    } else {
        // steam_appid.txt (containing 480 for dev against Spacewar) must sit
        // next to the executable, or the process must own the app id.
        bool ok = false;
        if (api_.Init) {
            ok = api_.Init();
        } else if (api_.InitFlat) {
            char err[1024] = {0};
            ok = (api_.InitFlat(err) == 0); // ESteamAPIInitResult: 0 == OK
            if (!ok && err[0]) LOG_ERROR("[steam] SteamAPI_InitFlat: %s", err);
        }

        if (!ok) {
            status_.store(Status::InitFailed, std::memory_order_release);
            LOG_ERROR("[steam] SteamAPI init failed — is the Steam client running and logged in?");
            unloadSteamFlat(api_);
        } else {
            // Resolve the interface pointers once, here on the service thread;
            // they stay valid for the process and are touched only by this thread.
            HSteamUser hUser = api_.GetHSteamUser();
            iUser_    = findInterface(api_, hUser, kUserVersions,
                                      sizeof(kUserVersions)/sizeof(*kUserVersions));
            iFriends_ = findInterface(api_, hUser, kFriendsVersions,
                                      sizeof(kFriendsVersions)/sizeof(*kFriendsVersions));
            iUtils_   = findInterface(api_, hUser, kUtilsVersions,
                                      sizeof(kUtilsVersions)/sizeof(*kUtilsVersions));
            iMatchmaking_ = findInterface(api_, hUser, kMatchmakingVersions,
                                      sizeof(kMatchmakingVersions)/sizeof(*kMatchmakingVersions));

            // Identity snapshot. Written before the Status::Available release
            // store, so JS-thread reads that observe Available see complete,
            // immutable values (lock-free publication).
            if (iUser_ && api_.User_GetSteamID)
                localSteamId_.store(api_.User_GetSteamID(iUser_), std::memory_order_relaxed);
            if (iFriends_ && api_.Friends_GetPersonaName) {
                const char* name = api_.Friends_GetPersonaName(iFriends_);
                personaName_ = name ? name : "";
            }
            if (iUtils_ && api_.Utils_GetAppID)
                appId_.store(api_.Utils_GetAppID(iUtils_), std::memory_order_relaxed);

            // Switch to ManualDispatch so we can pull callbacks ourselves on
            // this thread (PersonaStateChange, overlay, join requests, and —
            // later — lobby/voice). Falls back to RunCallbacks if the symbols
            // aren't present (older redistributable).
            if (api_.ManualDispatch_Init && api_.GetHSteamPipe &&
                api_.ManualDispatch_RunFrame && api_.ManualDispatch_GetNextCallback &&
                api_.ManualDispatch_FreeLastCallback) {
                api_.ManualDispatch_Init();
                pipe = api_.GetHSteamPipe();
                pipe_ = pipe; // dispatchCallback uses it for GetAPICallResult
                manualDispatch = (pipe != 0);
            }

            status_.store(Status::Available, std::memory_order_release);
            steamUp = true;
            LOG_INFO("[steam] SteamService started (steamId=%llu persona='%s' appId=%u dispatch=%s)",
                     static_cast<unsigned long long>(localSteamId_.load(std::memory_order_relaxed)),
                     personaName_.c_str(),
                     appId_.load(std::memory_order_relaxed),
                     manualDispatch ? "manual" : "auto");
        }
    }

    uint64_t iter = 0;
    while (running_.load(std::memory_order_acquire)) {
        // 1. Drain and process commands (FIFO) — always, Steam up or not.
        SteamCommand* cmd = drainAndReverse(cmdHead_);
        while (cmd) {
            SteamCommand* next = cmd->next;
            handleCommand(*cmd);
            delete cmd;
            cmd = next;
        }

        if (steamUp) {
            // 2. Pump Steam callbacks. ManualDispatch lets us translate each
            //    callback into our own events on this thread; otherwise the
            //    classic auto path still keeps the client serviced.
            if (manualDispatch) {
                api_.ManualDispatch_RunFrame(pipe);
                CallbackMsg_t msg;
                while (api_.ManualDispatch_GetNextCallback(pipe, &msg)) {
                    dispatchCallback(msg);
                    api_.ManualDispatch_FreeLastCallback(pipe);
                }
            } else {
                api_.RunCallbacks();
            }

            ++iter;

            // 3. Friends snapshot (~2 Hz). Emits only on a diff, so a steady
            //    list produces no events.
            if (iter % kFriendsRebuildEvery == 0) buildAndEmitFriends();

            // 4. Heartbeat: ~1 Hz pulse to every subscriber so the lab can
            //    confirm the pump is alive (M1).
            if (iter % 100 == 0) {
                for (auto& [sid, sub] : subscribers_) {
                    auto* ev = new SteamEvent();
                    ev->type = SteamEvent::Pulse;
                    ev->u64 = iter / 100;
                    postEventTo(sid, ev);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Drain late commands (e.g. Unregister posted between the last loop and the
    // running_=false observation) so subscribers don't leak.
    {
        SteamCommand* cmd = drainAndReverse(cmdHead_);
        while (cmd) {
            SteamCommand* next = cmd->next;
            handleCommand(*cmd);
            delete cmd;
            cmd = next;
        }
    }

    for (auto& [sid, sub] : subscribers_) delete sub;
    subscribers_.clear();

    if (steamUp) {
        api_.Shutdown();
        unloadSteamFlat(api_);
    }
    LOG_INFO("[steam] SteamService stopped");
}

} // namespace bro::steam
